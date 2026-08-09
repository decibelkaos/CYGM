/*
 * wifi_screens.c - WiFi scan/list, password entry, and connection overlays.
 */

#include "wifi_screens.h"
#include "shared_state.h"
#include "main.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "hardware/led.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include <string.h>
#include "sd_logger.h"

static const char *TAG = "WIFI_SCREENS";

// LVGL pool bytes kept free while building the network list, so the password
// screen plus its keyboard (~5KB) can still be created afterwards.
#define WIFI_LIST_POOL_RESERVE 8192
// Largest contiguous block a row plus its labels needs. Total free alone is not
// evidence the next allocation fits — a fragmented pool can pass that check and
// still trip LV_ASSERT_MALLOC, which halts the device.
#define WIFI_LIST_BLOCK_MIN    1024

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *wifi_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 4, 4);
    cygm_apply_ghost_btn(btn);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// WiFi scanning overlay
static lv_obj_t *wifi_scanning_overlay = NULL;
static lv_obj_t *wifi_scanning_bar = NULL;

// WiFi removal confirmation overlay
static lv_obj_t *wifi_removal_overlay = NULL;
static lv_obj_t *wifi_removal_hold_bar = NULL;
static uint32_t wifi_removal_hold_start_ms = 0;
static bool wifi_removal_initiated = false;
static char wifi_removal_ssid[MAX_SSID_LEN] = {0};

// WiFi connecting status overlay
static lv_obj_t *wifi_connecting_overlay = NULL;
static lv_obj_t *wifi_connecting_status_label = NULL;
static lv_obj_t *wifi_connecting_bar = NULL;
static lv_obj_t *wifi_connecting_ssid_label = NULL;   // SSID shown during connection
static lv_obj_t *wifi_connecting_details_label = NULL;  // Shows SSID and IP on success
static lv_obj_t *wifi_connecting_ok_btn = NULL;  // OK button to close overlay

// Password visibility toggle
static bool wifi_password_visible = false;
static lv_obj_t *wifi_eye_label = NULL;

// Forward declarations for internal event callbacks
static void network_item_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void password_field_event_cb(lv_event_t *e);
static void wifi_password_cancel_btn_cb(lv_event_t *e);
static void wifi_password_ok_btn_cb(lv_event_t *e);
static void wifi_eye_btn_cb(lv_event_t *e);
static void wifi_delete_btn_event_cb(lv_event_t *e);  // Delete button for saved networks
static void wifi_connect_saved_event_cb(lv_event_t *e);  // Connect to saved network
static void wifi_switch_btn_event_cb(lv_event_t *e);  // Switch to saved network (green check)
static void wifi_connecting_ok_btn_cb(lv_event_t *e);  // OK button on connecting overlay
static void hide_wifi_connecting_overlay(void);  // Hide connecting overlay
static void wifi_connect_task(void *pvParameters);  // WiFi connection background task
static void wifi_refresh_btn_event_cb(lv_event_t *e);  // Refresh/rescan button
static void wifi_list_add_header(lv_obj_t *parent);   // Header with back + refresh buttons

// Progress-bar fill WITHOUT lv_anim: LVGL animations freeze this hardware, so
// a 10Hz lv_timer steps the bar toward 90% with LV_ANIM_OFF instead. Only one
// transient overlay shows at a time, so one shared timer/bar pointer suffices.
static lv_timer_t *wifi_progress_timer = NULL;
static lv_obj_t   *wifi_progress_bar = NULL;
// Kept in HUNDREDTHS of a percent so a slow fill does not truncate: an integer
// 90/ticks step rounds to 0 on an 18s fill and the bar stalls at 90% halfway.
static int         wifi_progress_val = 0;     // 0..9000 (hundredths of %)
static int         wifi_progress_step = 100;  // hundredths of % per 100ms tick

static void wifi_progress_timer_cb(lv_timer_t *t) {
    (void)t;
    if (wifi_progress_bar == NULL || wifi_progress_val >= 9000) return;
    wifi_progress_val += wifi_progress_step;
    if (wifi_progress_val > 9000) wifi_progress_val = 9000;
    lv_bar_set_value(wifi_progress_bar, wifi_progress_val / 100, LV_ANIM_OFF);
}

// Start stepping `bar` from 0 to ~90% over duration_ms. Call under LVGL lock.
static void start_wifi_progress(lv_obj_t *bar, int duration_ms) {
    wifi_progress_bar = bar;
    wifi_progress_val = 0;
    int ticks = duration_ms / 100;
    wifi_progress_step = (ticks > 0) ? (9000 / ticks) : 9000;
    if (wifi_progress_step < 1) wifi_progress_step = 1;
    if (wifi_progress_timer == NULL) {
        wifi_progress_timer = lv_timer_create(wifi_progress_timer_cb, 100, NULL);
    }
}

// Stop and free the progress timer. Call under LVGL lock before deleting bars.
static void stop_wifi_progress(void) {
    if (wifi_progress_timer != NULL) {
        lv_timer_del(wifi_progress_timer);
        wifi_progress_timer = NULL;
    }
    wifi_progress_bar = NULL;
}

// Update WiFi status icon on home screen (non-static for auto-reconnect)
void update_wifi_status_display(bool connected) {
    // Retry up to 500ms — called from wifi_task (Core 0), LVGL may be busy
    for (int retry = 0; retry < 50; retry++) {
        if (lvgl_port_lock(1)) {
            if (connected) {
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
            } else {
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " " LV_SYMBOL_WARNING);
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
            }
            lvgl_port_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW("WIFI_UI", "Failed to update WiFi status icon (LVGL busy)");
}

// LVGL fires DELETE on EVERY teardown path (explicit del, lv_obj_clean of the
// parent, screen delete by the inactivity watchdog), so clearing the statics
// and the fill timer here is what stops them outliving the overlay.
static void wifi_scanning_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    stop_wifi_progress();
    wifi_scanning_overlay = NULL;
    wifi_scanning_bar = NULL;
}

// Show WiFi scanning overlay — glass-themed, overlays the list semi-transparently
void show_wifi_scanning_overlay(void) {
    if (!lvgl_port_lock(1)) {
        return;
    }

    // Remove previous overlay if still present (e.g. rapid refresh taps);
    // its DELETE callback clears the statics and stops the fill timer.
    if (wifi_scanning_overlay != NULL) {
        lv_obj_del(wifi_scanning_overlay);
    }

    // Full-screen semi-transparent backdrop (blocks clicks to list beneath)
    wifi_scanning_overlay = lv_obj_create(lv_scr_act());
    lv_obj_add_event_cb(wifi_scanning_overlay, wifi_scanning_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);
    lv_obj_set_size(wifi_scanning_overlay, 320, 240);
    lv_obj_set_pos(wifi_scanning_overlay, 0, 0);
    lv_obj_set_style_bg_color(wifi_scanning_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_scanning_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(wifi_scanning_overlay, 0, 0);
    lv_obj_set_style_radius(wifi_scanning_overlay, 0, 0);
    lv_obj_clear_flag(wifi_scanning_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Glass card (centered)
    lv_obj_t *card = lv_obj_create(wifi_scanning_overlay);
    lv_obj_set_size(card, 240, 110);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi icon + "Scanning..." label
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, LV_SYMBOL_WIFI "  Scanning...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 16);

    // Progress bar
    wifi_scanning_bar = lv_bar_create(card);
    lv_obj_set_size(wifi_scanning_bar, 200, 16);
    lv_obj_align(wifi_scanning_bar, LV_ALIGN_CENTER, 0, 10);

    // Bar track (dark glass)
    lv_obj_set_style_bg_color(wifi_scanning_bar, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(wifi_scanning_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_scanning_bar, 8, 0);

    // Bar indicator (blue gradient)
    lv_obj_set_style_bg_color(wifi_scanning_bar, lv_color_hex(0x1D4ED8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(wifi_scanning_bar, lv_color_hex(COLOR_ACCENT_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(wifi_scanning_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(wifi_scanning_bar, 8, LV_PART_INDICATOR);

    lv_bar_set_range(wifi_scanning_bar, 0, 100);
    lv_bar_set_value(wifi_scanning_bar, 0, LV_ANIM_OFF);

    // Slow fill: 0 -> 90 over 6 seconds (scan takes ~3-5s), timer-driven
    start_wifi_progress(wifi_scanning_bar, 6000);

    lvgl_port_unlock();
}

// Hide WiFi scanning overlay
void hide_wifi_scanning_overlay(void) {
    if (!lvgl_port_lock(1)) {
        return;
    }

    if (wifi_scanning_overlay != NULL) {
        // Stop the progress timer before deleting the bar it points at
        stop_wifi_progress();

        // Delete overlay (this deletes all children too)
        lv_obj_del(wifi_scanning_overlay);
        wifi_scanning_overlay = NULL;
        wifi_scanning_bar = NULL;
    }

    lvgl_port_unlock();
}

#define WIFI_REMOVAL_HOLD_DURATION_MS 1500

static void wifi_removal_hold_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (wifi_removal_hold_bar == NULL) return;

    if (code == LV_EVENT_PRESSED) {
        wifi_removal_hold_start_ms = lv_tick_get();
    } else if (code == LV_EVENT_PRESSING) {
        uint32_t elapsed = lv_tick_elaps(wifi_removal_hold_start_ms);
        int progress = (elapsed * 100) / WIFI_REMOVAL_HOLD_DURATION_MS;
        if (progress > 100) progress = 100;
        lv_bar_set_value(wifi_removal_hold_bar, progress, LV_ANIM_OFF);

        if (progress >= 100 && !wifi_removal_initiated) {
            wifi_removal_initiated = true;
            ESP_LOGI(TAG, "Hold confirmed - removing WiFi network: %s", wifi_removal_ssid);
            sd_log(TAG, "WiFi: removed network %s", wifi_removal_ssid);

            // Check if this is the currently connected network
            bool disconnect_needed = false;
            if (wifi_connected) {
                wifi_config_t current_config;
                if (esp_wifi_get_config(WIFI_IF_STA, &current_config) == ESP_OK) {
                    if (strcmp((char *)current_config.sta.ssid, wifi_removal_ssid) == 0) {
                        ESP_LOGI(TAG, "Removing currently connected network - disconnecting...");
                        disconnect_needed = true;
                    }
                }
            }

            // Remove from NVS
            esp_err_t ret = nvs_remove_wifi_network(wifi_removal_ssid);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "WiFi network removed successfully");
            } else {
                ESP_LOGE(TAG, "Failed to remove WiFi network: %s", esp_err_to_name(ret));
            }

            // Disconnect if needed
            if (disconnect_needed) {
                wifi_manager_disconnect();
                wifi_connected = false;
                update_wifi_status_display(false);
                led_off();
            }

            // Close overlay
            if (wifi_removal_overlay != NULL) {
                lv_obj_del(wifi_removal_overlay);
                wifi_removal_overlay = NULL;
                wifi_removal_hold_bar = NULL;
            }

            // Rescan WiFi to update list
            show_wifi_scanning_overlay();
            xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!wifi_removal_initiated) {
            lv_bar_set_value(wifi_removal_hold_bar, 0, LV_ANIM_OFF);  // LV_ANIM_ON triggers lv_anim_start (freeze risk)
        }
    }
}

// Cancel button callback for WiFi removal overlay
static void wifi_removal_cancel_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "WiFi removal cancelled");

        if (lvgl_port_lock(1)) {
            if (wifi_removal_overlay != NULL) {
                lv_obj_del(wifi_removal_overlay);
                wifi_removal_overlay = NULL;
                wifi_removal_hold_bar = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Same as the scanning overlay: this one is parented to the active screen, and
// a rescan's lv_obj_clean() or the inactivity watchdog can take that screen
// away, so the statics must be cleared on every delete path.
static void wifi_removal_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    wifi_removal_overlay = NULL;
    wifi_removal_hold_bar = NULL;
    wifi_removal_initiated = false;
}

// Show WiFi removal confirmation overlay
void show_wifi_removal_overlay(const char *ssid) {
    if (ssid == NULL) {
        return;
    }

    // Save SSID for removal
    strncpy(wifi_removal_ssid, ssid, sizeof(wifi_removal_ssid) - 1);

    if (!lvgl_port_lock(1)) {
        return;
    }

    // Close if already open
    if (wifi_removal_overlay != NULL) {
        lv_obj_del(wifi_removal_overlay);
        wifi_removal_overlay = NULL;
        wifi_removal_hold_bar = NULL;
    }

    // Create overlay (280x200, centered)
    wifi_removal_overlay = lv_obj_create(lv_scr_act());
    lv_obj_add_event_cb(wifi_removal_overlay, wifi_removal_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);
    lv_obj_set_size(wifi_removal_overlay, 280, 200);
    lv_obj_center(wifi_removal_overlay);
    lv_obj_set_style_bg_color(wifi_removal_overlay, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_border_color(wifi_removal_overlay, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(wifi_removal_overlay, 2, 0);
    lv_obj_set_style_radius(wifi_removal_overlay, 12, 0);
    lv_obj_clear_flag(wifi_removal_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Pinned to 0 so every child offset below measures from a known origin:
    // the 276x196 content box (280x200 less the 2px border).
    lv_obj_set_style_pad_all(wifi_removal_overlay, 0, 0);

    // Title
    lv_obj_t *title = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(title, "Remove WiFi?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // A long SSID wraps, so the row below is placed clear of 2 lines, not 1.
    lv_obj_t *ssid_label = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(ssid_label, ssid);
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_width(ssid_label, 250);
    lv_obj_set_style_text_align(ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_MID, 0, 40);

    // Instruction text
    lv_obj_t *instruction = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(instruction, "Hold button below to confirm");
    lv_obj_set_style_text_font(instruction, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(instruction, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(instruction, LV_ALIGN_TOP_MID, 0, 82);

    // Hold-to-confirm container (replaces slider)
    lv_obj_t *hold_container = lv_obj_create(wifi_removal_overlay);
    lv_obj_remove_style_all(hold_container);
    lv_obj_set_size(hold_container, 240, 30);
    lv_obj_align(hold_container, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_set_style_bg_color(hold_container, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(hold_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hold_container, 15, 0);
    lv_obj_set_style_border_color(hold_container, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(hold_container, 1, 0);
    lv_obj_clear_flag(hold_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hold_container, LV_OBJ_FLAG_CLICKABLE);
    cygm_apply_ghost_btn(hold_container);

    // Progress bar (fills red as you hold)
    wifi_removal_hold_bar = lv_bar_create(hold_container);
    lv_obj_set_size(wifi_removal_hold_bar, 238, 28);
    lv_obj_center(wifi_removal_hold_bar);
    lv_bar_set_range(wifi_removal_hold_bar, 0, 100);
    lv_bar_set_value(wifi_removal_hold_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(wifi_removal_hold_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_removal_hold_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(wifi_removal_hold_bar, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(wifi_removal_hold_bar, 14, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_removal_hold_bar, 14, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(wifi_removal_hold_bar, 300, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_removal_hold_bar, LV_OBJ_FLAG_CLICKABLE);

    // Label on top
    lv_obj_t *hold_label = lv_label_create(hold_container);
    lv_label_set_text(hold_label, LV_SYMBOL_TRASH "  Hold to Remove");
    lv_obj_set_style_text_color(hold_label, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(hold_label);
    lv_obj_clear_flag(hold_label, LV_OBJ_FLAG_CLICKABLE);

    // Events on container
    wifi_removal_initiated = false;
    lv_obj_add_event_cb(hold_container, wifi_removal_hold_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hold_container, wifi_removal_hold_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hold_container, wifi_removal_hold_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(hold_container, wifi_removal_hold_event_cb, LV_EVENT_PRESS_LOST, NULL);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(wifi_removal_overlay);
    lv_obj_set_size(cancel_btn, CYGM_BTN_W_TEXT, CYGM_BTN_H);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    // Neutral border — this is the "do nothing" action, not the accent one.
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(COLOR_MODAL_BORDER), 0);
    cygm_apply_ghost_btn(cancel_btn);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, wifi_removal_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lvgl_port_unlock();
}

// WiFi scan task
void wifi_scan_task(void *pvParameters) {
    wifi_scan_and_show_networks();
    vTaskDelete(NULL);  // Delete task when done
}

// WiFi scan and show networks function
void wifi_scan_and_show_networks(void) {
    ESP_LOGI(TAG, "Scanning for WiFi networks...");

    // Scan for networks
    uint16_t raw_count = wifi_manager_scan_networks(wifi_networks, 10);
    ESP_LOGI(TAG, "Found %d networks (raw)", raw_count);

    // Filter duplicate SSIDs - keep only strongest signal for each SSID
    wifi_network_count = 0;
    for (int i = 0; i < raw_count && i < 10; i++) {
        bool is_duplicate = false;

        // Check if this SSID already exists in filtered list
        for (int j = 0; j < wifi_network_count; j++) {
            if (strcmp((char *)wifi_networks[i].ssid, (char *)wifi_networks[j].ssid) == 0) {
                // Duplicate found - keep the one with stronger signal (higher RSSI)
                if (wifi_networks[i].rssi > wifi_networks[j].rssi) {
                    // Replace weaker signal with stronger one
                    memcpy(&wifi_networks[j], &wifi_networks[i], sizeof(wifi_ap_record_t));
                }
                is_duplicate = true;
                break;
            }
        }

        // If not a duplicate, add to filtered list
        if (!is_duplicate) {
            if (wifi_network_count < i) {
                // Move to filtered position
                memcpy(&wifi_networks[wifi_network_count], &wifi_networks[i], sizeof(wifi_ap_record_t));
            }
            wifi_network_count++;
        }
    }

    ESP_LOGI(TAG, "Filtered to %d unique scanned networks", wifi_network_count);

    // Load saved networks and add any that aren't in scan results
    wifi_credentials_t saved_networks[MAX_SAVED_WIFI_NETWORKS];
    uint8_t saved_count = 0;
    if (nvs_get_saved_wifi_networks(saved_networks, &saved_count) == ESP_OK) {
        ESP_LOGI(TAG, "Found %d saved networks in NVS", saved_count);

        for (int i = 0; i < saved_count && wifi_network_count < 10; i++) {
            // Check if already in scanned list
            bool already_in_list = false;
            for (int j = 0; j < wifi_network_count; j++) {
                if (strcmp((char *)wifi_networks[j].ssid, saved_networks[i].ssid) == 0) {
                    already_in_list = true;
                    break;
                }
            }

            // If saved network not in scan results, add it with RSSI = -128 (marker for "not visible")
            if (!already_in_list) {
                memset(&wifi_networks[wifi_network_count], 0, sizeof(wifi_ap_record_t));
                strncpy((char *)wifi_networks[wifi_network_count].ssid, saved_networks[i].ssid, sizeof(wifi_networks[wifi_network_count].ssid) - 1);
                wifi_networks[wifi_network_count].rssi = -128;  // Special marker for not visible
                wifi_network_count++;
                ESP_LOGI(TAG, "Added saved-but-not-visible network: %s", saved_networks[i].ssid);
            }
        }
    }

    ESP_LOGI(TAG, "Total networks to display (scanned + saved): %d", wifi_network_count);

    // Tear down the overlay and build the list in ONE locked transaction, and
    // retry the try-lock: a silent skip leaves the bar stuck and lets a later
    // lv_obj_clean() free the overlay behind the statics' back.
    bool ui_locked = false;
    for (int retry = 0; retry < 50 && !ui_locked; retry++) {
        ui_locked = lvgl_port_lock(1);
        if (!ui_locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ui_locked) {
        ESP_LOGW(TAG, "LVGL busy for 500ms — scan results not displayed");
        return;
    }

    // Overlay's DELETE callback clears the statics and stops the fill timer.
    if (wifi_scanning_overlay != NULL) {
        lv_obj_del(wifi_scanning_overlay);
    }

    if (wifi_network_count == 0) {
        ESP_LOGW(TAG, "No networks found");
        lvgl_port_unlock();
        return;
    }

    // Create network list UI (lock already held)
    {
        // Remove any existing network list
        lv_obj_clean(screen_wifi_list);

        // Recreate header with back + refresh buttons
        wifi_list_add_header(screen_wifi_list);

        // Create scrollable list
        lv_obj_t *list = lv_obj_create(screen_wifi_list);
        lv_obj_set_size(list, 300, 180);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_radius(list, 12, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 5, 0);
        // Rows tile with no gap, so scrolled to the end the last one lands hard
        // against the card edge. Reserve a strip of clearance under it.
        lv_obj_set_style_pad_bottom(list, 8, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

        // Rows tile edge to edge and the list scrolls vertically: every scanned
        // and saved network is reachable, not just the first screenful.
        lv_obj_set_style_pad_row(list, 0, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

        // Determine currently connected SSID for status indicators
        char connected_ssid[33] = {0};
        bool have_connected_ssid = false;
        if (wifi_connected) {
            wifi_config_t current_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK) {
                strncpy(connected_ssid, (char *)current_cfg.sta.ssid, sizeof(connected_ssid) - 1);
                have_connected_ssid = (strlen(connected_ssid) > 0);
            }
        }

        // Add every network we know about — the list scrolls
        for (int i = 0; i < wifi_network_count; i++) {
            // The 36KB pool still has to hold the password screen and its
            // keyboard after this list. Stop adding rows rather than let
            // LV_ASSERT_MALLOC halt the device.
            lv_mem_monitor_t pool;
            lv_mem_monitor(&pool);
            if (pool.free_size < WIFI_LIST_POOL_RESERVE ||
                pool.free_biggest_size < WIFI_LIST_BLOCK_MIN) {
                ESP_LOGW(TAG, "LVGL pool low (%lu free, %lu biggest): showing %d of %d networks",
                         (unsigned long)pool.free_size, (unsigned long)pool.free_biggest_size,
                         i, wifi_network_count);
                break;
            }

            bool is_saved = nvs_is_wifi_saved((const char *)wifi_networks[i].ssid);
            bool is_connected = have_connected_ssid &&
                                strcmp((const char *)wifi_networks[i].ssid, connected_ssid) == 0;

            lv_obj_t *row = lv_btn_create(list);
            lv_obj_set_width(row, 290);
            lv_obj_set_height(row, 40);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            cygm_apply_ghost_btn(row);
            // Rows already tile without gaps, so the shared touch-box growth
            // would make neighbours overlap and select the wrong network.
            lv_obj_set_ext_click_area(row, 0);

            // Left offset: saved networks get a status dot, so shift content right
            int left_offset = is_saved ? 12 : 0;

            // Status indicator dot (saved networks only)
            if (is_saved) {
                lv_obj_t *dot = lv_obj_create(row);
                lv_obj_set_size(dot, 10, 10);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
                lv_obj_set_style_shadow_width(dot, 0, 0);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);

                if (is_connected) {
                    // Green filled dot (static — no animation to avoid freeze)
                    lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_GREEN), 0);
                    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(dot, 0, 0);
                } else {
                    // Empty circle (border only) for saved but not connected
                    lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_color(dot, lv_color_hex(COLOR_TEXT_DIM), 0);
                    lv_obj_set_style_border_width(dot, 1, 0);
                    lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
                }
            }

            // Signal strength bars (based on RSSI)
            int8_t rssi = wifi_networks[i].rssi;
            bool network_visible = (rssi > -128);  // RSSI = -128 means saved but not visible
            int bars = 0;
            if (network_visible) {
                if (rssi > -50) bars = 3;
                else if (rssi > -60) bars = 2;
                else if (rssi > -70) bars = 1;
            }

            // Draw signal bars (3 vertical bars of increasing heights, bottom-aligned with text baseline)
            for (int b = 0; b < 3; b++) {
                lv_obj_t *bar = lv_obj_create(row);
                int bar_height = 6 + (b * 4);  // 6px, 10px, 14px
                lv_obj_set_size(bar, 4, bar_height);  // 4px wide vertical bars

                int bar_x = left_offset + 8 + (b * 5);
                int bar_y = 16 - bar_height;  // All bars bottom-align at y=16
                lv_obj_set_pos(bar, bar_x, bar_y);

                if (!network_visible || b >= bars) {
                    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TEXT_DIM), 0);
                } else {
                    // Connected network gets green signal bars
                    uint32_t bar_color = is_connected ? COLOR_GREEN : COLOR_ACCENT_BLUE;
                    lv_obj_set_style_bg_color(bar, lv_color_hex(bar_color), 0);
                }
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 1, 0);
                lv_obj_set_style_pad_all(bar, 0, 0);
            }

            // Ellipsis-clipped so a long name never runs under the action
            // buttons; the widths below stop it 8px clear of the green square
            // on a saved row and the arrow on an open one. LONG_DOT is static —
            // LONG_SCROLL would start an lv_anim, which freezes the device.
            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, (const char *)wifi_networks[i].ssid);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_width(label, (is_saved ? 167 : 215) - left_offset);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, left_offset + 25, 0);

            // Connected SSID in green, others in white
            if (is_connected) {
                lv_obj_set_style_text_color(label, lv_color_hex(COLOR_GREEN), 0);
            } else {
                lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
            }

            if (is_saved) {
                // Connect + remove squares, 8px apart so their 4px touch boxes
                // meet without overlapping — a mis-tap here would delete a
                // network instead of joining it.
                lv_obj_t *switch_btn = lv_obj_create(row);
                lv_obj_set_size(switch_btn, 26, 26);
                lv_obj_align(switch_btn, LV_ALIGN_RIGHT_MID, -38, 0);

                if (network_visible) {
                    lv_obj_set_style_bg_color(switch_btn, lv_color_hex(COLOR_GREEN), 0);
                    lv_obj_add_flag(switch_btn, LV_OBJ_FLAG_CLICKABLE);
                } else {
                    lv_obj_set_style_bg_color(switch_btn, lv_color_hex(COLOR_TEXT_DIM), 0);
                    lv_obj_clear_flag(switch_btn, LV_OBJ_FLAG_CLICKABLE);
                }
                lv_obj_set_style_radius(switch_btn, 4, 0);
                lv_obj_set_style_border_width(switch_btn, 0, 0);
                lv_obj_set_style_pad_all(switch_btn, 0, 0);
                // Explicit local opacity: the shared ghost style is transparent
                // and would otherwise erase this square's fill.
                lv_obj_set_style_bg_opa(switch_btn, LV_OPA_COVER, 0);
                cygm_apply_ghost_btn(switch_btn);
                lv_obj_set_ext_click_area(switch_btn, 4);

                lv_obj_t *check_icon = lv_label_create(switch_btn);
                lv_label_set_text(check_icon, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(check_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
                lv_obj_set_style_text_font(check_icon, &lv_font_montserrat_12, 0);
                lv_obj_center(check_icon);

                if (network_visible) {
                    lv_obj_add_event_cb(switch_btn, wifi_switch_btn_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
                    lv_obj_clear_flag(switch_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
                }

                // Red square with X for removal
                lv_obj_t *delete_btn = lv_obj_create(row);
                lv_obj_set_size(delete_btn, 26, 26);
                lv_obj_align(delete_btn, LV_ALIGN_RIGHT_MID, -4, 0);
                lv_obj_set_style_bg_color(delete_btn, lv_color_hex(COLOR_RED), 0);
                lv_obj_set_style_bg_opa(delete_btn, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(delete_btn, 4, 0);
                lv_obj_set_style_border_width(delete_btn, 0, 0);
                lv_obj_set_style_pad_all(delete_btn, 0, 0);
                lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_CLICKABLE);
                cygm_apply_ghost_btn(delete_btn);
                lv_obj_set_ext_click_area(delete_btn, 4);

                lv_obj_t *x_icon = lv_label_create(delete_btn);
                lv_label_set_text(x_icon, LV_SYMBOL_CLOSE);
                lv_obj_set_style_text_color(x_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
                lv_obj_set_style_text_font(x_icon, &lv_font_montserrat_12, 0);
                lv_obj_center(x_icon);

                lv_obj_add_event_cb(delete_btn, wifi_delete_btn_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
                lv_obj_clear_flag(delete_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

                // Row click connects to saved network
                lv_obj_add_event_cb(row, wifi_connect_saved_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
            } else {
                // Arrow for unsaved networks (password entry)
                lv_obj_t *arrow = lv_label_create(row);
                lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
                lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
                lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -5, 0);

                // Row click opens password screen
                lv_obj_add_event_cb(row, network_item_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
            }
        }

        // Load the WiFi list screen
        lv_scr_load(screen_wifi_list);
        lvgl_port_unlock();
    }
}

// Refresh button callback — rescan WiFi networks
static void wifi_refresh_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "WiFi refresh requested");
    show_wifi_scanning_overlay();
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
}

// Helper: add header bar with back + refresh buttons to wifi list screen
static void wifi_list_add_header(lv_obj_t *parent) {
    // Header
    lv_obj_t *header = lv_label_create(parent);
    lv_label_set_text(header, "WiFi Networks");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    wifi_std_back_btn(parent, back_btn_event_cb, NULL);

    // Ghost refresh button (top-right)
    lv_obj_t *refresh_btn = lv_btn_create(parent);
    lv_obj_set_size(refresh_btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(refresh_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    cygm_apply_ghost_btn(refresh_btn);

    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(refresh_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(refresh_lbl);
    lv_obj_add_event_cb(refresh_btn, wifi_refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

// Create WiFi list screen
void create_wifi_list_screen(void) {
    // The inactivity watchdog can reclaim a screen mid-flow, so rebuilding
    // without this guard orphans the previous one in the LVGL pool. Deleting
    // the ACTIVE screen synchronously would panic, hence the branch.
    if (screen_wifi_list != NULL) {
        if (screen_wifi_list == lv_scr_act()) {
            lv_obj_del_async(screen_wifi_list);
        } else {
            lv_obj_del(screen_wifi_list);
        }
        screen_wifi_list = NULL;
    }

    screen_wifi_list = lv_obj_create(NULL);
    lv_obj_add_style(screen_wifi_list, &style_bg, 0);
    // Outer screen only — the network list built inside it stays scrollable.
    lv_obj_clear_flag(screen_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    wifi_list_add_header(screen_wifi_list);

    // Network list will be populated dynamically by wifi_scan_and_show_networks()
}

// password_field and keyboard are read from elsewhere, so a teardown this file
// did not initiate (the inactivity watchdog) would leave them dangling. Only
// the screen the global still points at clears them: a deferred delete of a
// superseded screen must not disown the live one's widgets.
static void wifi_password_screen_delete_cb(lv_event_t *e) {
    if (lv_event_get_target(e) != screen_wifi_password) return;
    password_field = NULL;
    keyboard = NULL;
    wifi_eye_label = NULL;
}

// Create WiFi password entry screen
void create_wifi_password_screen(void) {
    if (screen_wifi_password != NULL) {
        if (screen_wifi_password == lv_scr_act()) {
            lv_obj_del_async(screen_wifi_password);
        } else {
            lv_obj_del(screen_wifi_password);
        }
        screen_wifi_password = NULL;
    }

    screen_wifi_password = lv_obj_create(NULL);
    if (screen_wifi_password == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi password screen - out of memory!");
        return;
    }
    lv_obj_add_style(screen_wifi_password, &style_bg, 0);
    lv_obj_clear_flag(screen_wifi_password, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_wifi_password, wifi_password_screen_delete_cb,
                        LV_EVENT_DELETE, NULL);

    // Reset password visibility state
    wifi_password_visible = false;

    // Section header
    lv_obj_t *header = lv_label_create(screen_wifi_password);
    lv_label_set_text(header, "PASSWORD");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(header, 2, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    wifi_std_back_btn(screen_wifi_password, wifi_password_cancel_btn_cb, NULL);

    // OK button (accent blue, top-right)
    lv_obj_t *ok_btn = lv_btn_create(screen_wifi_password);
    lv_obj_set_size(ok_btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    cygm_apply_ghost_btn(ok_btn);

    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok_btn, wifi_password_ok_btn_cb, LV_EVENT_CLICKED, NULL);

    // Eye toggle button. Sits a full touch-box clear of OK so the enlarged hit
    // areas never overlap — a mis-tap here would fire a connect attempt.
    lv_obj_t *eye_btn = lv_btn_create(screen_wifi_password);
    lv_obj_set_size(eye_btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, -62, 4);
    lv_obj_set_style_border_color(eye_btn, lv_color_hex(COLOR_MODAL_BORDER), 0);
    cygm_apply_ghost_btn(eye_btn);

    wifi_eye_label = lv_label_create(eye_btn);
    lv_label_set_text(wifi_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(wifi_eye_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(wifi_eye_label);
    lv_obj_add_event_cb(eye_btn, wifi_eye_btn_cb, LV_EVENT_CLICKED, NULL);

    // Password field (text area for keyboard input)
    password_field = lv_textarea_create(screen_wifi_password);
    lv_obj_set_size(password_field, 240, 36);
    lv_obj_align(password_field, LV_ALIGN_TOP_MID, 0, 60);
    lv_textarea_set_placeholder_text(password_field, "Password");
    lv_textarea_set_one_line(password_field, true);
    lv_textarea_set_password_mode(password_field, true);
    lv_textarea_set_password_show_time(password_field, 1000);
    lv_textarea_set_text_selection(password_field, false);
    lv_textarea_set_max_length(password_field, 63);
    lv_obj_set_style_text_font(password_field, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(password_field, lv_color_hex(COLOR_INPUT_BG), 0);
    lv_obj_set_style_bg_opa(password_field, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(password_field, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_border_color(password_field, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(password_field, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(password_field, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(password_field, 1, 0);
    lv_obj_set_style_radius(password_field, 8, 0);
    lv_obj_add_event_cb(password_field, password_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Blue accent underline
    lv_obj_t *accent = lv_obj_create(screen_wifi_password);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 240, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Keyboard — glass theme
    keyboard = lv_keyboard_create(screen_wifi_password);
    lv_obj_set_size(keyboard, 320, 130);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, password_field);

    // Glass keyboard background
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_pad_all(keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight (accent blue)
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
}

// Network item click event callback
static void network_item_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        const char *ssid = (const char *)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Selected network: %s", ssid);

        // Copy SSID and reset password
        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        wifi_password_len = 0;
        wifi_password[0] = '\0';

        // Create and load password screen
        if (lvgl_port_lock(1)) {
            // Create password screen if it doesn't exist
            if (screen_wifi_password == NULL) {
                create_wifi_password_screen();
            }
            if (screen_wifi_password == NULL) {
                ESP_LOGE(TAG, "WiFi password screen creation failed");
                lvgl_port_unlock();
                return;
            }

            // Clear password field
            lv_textarea_set_text(password_field, "");
            wifi_password_visible = false;
            if (wifi_eye_label) lv_label_set_text(wifi_eye_label, LV_SYMBOL_EYE_CLOSE);

            // Reset keyboard state
            caps_lock_enabled = false;
            last_shift_press_time = 0;
            shift_waiting_for_char = false;
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

            lv_scr_load(screen_wifi_password);
            lvgl_port_unlock();
        }
    }
}

// Switch to saved network (green checkmark button)
static void wifi_switch_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        const char *ssid = (const char *)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Switching to saved network: %s", ssid);

        // Get saved credentials
        wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
        uint8_t count = 0;
        if (nvs_get_saved_wifi_networks(networks, &count) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load saved networks");
            return;
        }

        // Find matching network
        for (int i = 0; i < count; i++) {
            if (strcmp(networks[i].ssid, ssid) == 0) {
                // Copy SSID and password
                strncpy(selected_ssid, networks[i].ssid, sizeof(selected_ssid) - 1);
                strncpy(wifi_password, networks[i].password, sizeof(wifi_password) - 1);
                wifi_password_len = strlen(wifi_password);

                // Start WiFi connection in background task (don't block UI)
                xTaskCreate(wifi_connect_task, "wifi_connect", 8192, NULL, 5, NULL);
                return;
            }
        }

        ESP_LOGE(TAG, "Saved network not found: %s", ssid);
    }
}

// Connect to saved network event callback (row click when saved)
static void wifi_connect_saved_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        const char *ssid = (const char *)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Connecting to saved network: %s", ssid);

        // Get saved credentials
        wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
        uint8_t count = 0;
        if (nvs_get_saved_wifi_networks(networks, &count) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load saved networks");
            return;
        }

        // Find matching network
        for (int i = 0; i < count; i++) {
            if (strcmp(networks[i].ssid, ssid) == 0) {
                // Copy SSID and password
                strncpy(selected_ssid, networks[i].ssid, sizeof(selected_ssid) - 1);
                strncpy(wifi_password, networks[i].password, sizeof(wifi_password) - 1);
                wifi_password_len = strlen(wifi_password);

                // Start WiFi connection in background task (don't block UI)
                xTaskCreate(wifi_connect_task, "wifi_connect", 8192, NULL, 5, NULL);
                return;
            }
        }

        ESP_LOGE(TAG, "Saved network not found: %s", ssid);
    }
}

// Delete button event callback (shows removal confirmation overlay)
static void wifi_delete_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        const char *ssid = (const char *)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Delete button clicked for: %s", ssid);

        show_wifi_removal_overlay(ssid);
    }
}

// Password field value changed event callback
static void password_field_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_textarea_get_text(password_field);
        strncpy(wifi_password, text, sizeof(wifi_password) - 1);
        wifi_password[sizeof(wifi_password) - 1] = '\0';
        wifi_password_len = strlen(wifi_password);
    }
}

// Eye toggle callback for WiFi password screen
static void wifi_eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (password_field == NULL) return;

    // Capture the real password BEFORE the mode change, into a LOCAL copy:
    // lv_textarea_set_password_mode() fires LV_EVENT_VALUE_CHANGED
    // re-entrantly and can overwrite the global buffer with partial data.
    const char *real_text = lv_textarea_get_text(password_field);
    char saved_pw[64];
    strncpy(saved_pw, real_text ? real_text : "", sizeof(saved_pw) - 1);
    saved_pw[sizeof(saved_pw) - 1] = '\0';

    // Sync global buffer from authoritative source
    strncpy(wifi_password, saved_pw, sizeof(wifi_password) - 1);
    wifi_password[sizeof(wifi_password) - 1] = '\0';
    wifi_password_len = strlen(wifi_password);

    wifi_password_visible = !wifi_password_visible;
    lv_textarea_set_password_mode(password_field, !wifi_password_visible);
    if (wifi_eye_label) {
        lv_label_set_text(wifi_eye_label,
                          wifi_password_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }

    // Force-set text from local copy (immune to re-entrant corruption)
    lv_textarea_set_text(password_field, saved_pw);

    ESP_LOGI(TAG, "Password %s (len=%d)",
             wifi_password_visible ? "shown" : "hidden", wifi_password_len);
}

// Cancel button callback for WiFi password screen
static void wifi_password_cancel_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Password entry cancelled, returning to network list");
        if (lvgl_port_lock(1)) {
            lv_scr_load(screen_wifi_list);
            lvgl_port_unlock();
        }
    }
}

// Fires on every teardown path (explicit hide, screen delete by the inactivity
// watchdog), so the statics and the shared fill timer can never dangle.
static void wifi_connecting_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    stop_wifi_progress();
    wifi_connecting_overlay = NULL;
    wifi_connecting_status_label = NULL;
    wifi_connecting_bar = NULL;
    wifi_connecting_ssid_label = NULL;
    wifi_connecting_details_label = NULL;
    wifi_connecting_ok_btn = NULL;
}

static void show_wifi_connecting_overlay(void) {
    // Runs in wifi_connect_task (Core 0), not LVGL context, so retry the
    // try-lock — the LVGL task may still be inside the button callback.
    bool locked = false;
    for (int retry = 0; retry < 50 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) {
        ESP_LOGE(TAG, "Failed to get LVGL lock for connecting overlay");
        return;
    }

    // Remove a previous overlay if one is still up (rapid retry taps)
    if (wifi_connecting_overlay != NULL) {
        lv_obj_del(wifi_connecting_overlay);
    }

    // Overlay card (centered)
    wifi_connecting_overlay = lv_obj_create(lv_scr_act());
    lv_obj_add_event_cb(wifi_connecting_overlay, wifi_connecting_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);
    lv_obj_set_size(wifi_connecting_overlay, 260, 170);
    lv_obj_center(wifi_connecting_overlay);
    lv_obj_set_style_bg_color(wifi_connecting_overlay, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_border_color(wifi_connecting_overlay, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(wifi_connecting_overlay, 2, 0);
    lv_obj_set_style_radius(wifi_connecting_overlay, 12, 0);
    lv_obj_clear_flag(wifi_connecting_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Status label at top
    wifi_connecting_status_label = lv_label_create(wifi_connecting_overlay);
    lv_label_set_text(wifi_connecting_status_label, "Connecting...");
    lv_obj_set_style_text_font(wifi_connecting_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wifi_connecting_status_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(wifi_connecting_status_label, LV_ALIGN_TOP_MID, 0, 12);

    // SSID label (small, dim, below status)
    wifi_connecting_ssid_label = lv_label_create(wifi_connecting_overlay);
    lv_label_set_text(wifi_connecting_ssid_label, selected_ssid);
    lv_obj_set_style_text_font(wifi_connecting_ssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_connecting_ssid_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(wifi_connecting_ssid_label, LV_ALIGN_TOP_MID, 0, 36);

    // Fat progress bar - CENTERED in overlay
    wifi_connecting_bar = lv_bar_create(wifi_connecting_overlay);
    lv_obj_set_size(wifi_connecting_bar, 220, 20);
    lv_obj_align(wifi_connecting_bar, LV_ALIGN_CENTER, 0, 8);

    // Bar track (dark background)
    lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(COLOR_INPUT_BG), 0);
    lv_obj_set_style_bg_opa(wifi_connecting_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_connecting_bar, 10, 0);

    // Bar indicator (blue gradient - darker left, brighter right edge)
    lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(0x1D4ED8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(wifi_connecting_bar, lv_color_hex(COLOR_ACCENT_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(wifi_connecting_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(wifi_connecting_bar, 10, LV_PART_INDICATOR);

    lv_bar_set_range(wifi_connecting_bar, 0, 100);
    lv_bar_set_value(wifi_connecting_bar, 0, LV_ANIM_OFF);

    // Slow fill: 0 -> 90 over 18 seconds, timer-driven (no lv_anim)
    start_wifi_progress(wifi_connecting_bar, 18000);

    // Details label (hidden until connected - shows SSID + IP)
    wifi_connecting_details_label = lv_label_create(wifi_connecting_overlay);
    lv_label_set_text(wifi_connecting_details_label, "");
    lv_obj_set_style_text_font(wifi_connecting_details_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_connecting_details_label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_align(wifi_connecting_details_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(wifi_connecting_details_label, 240);
    lv_obj_align(wifi_connecting_details_label, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(wifi_connecting_details_label, LV_OBJ_FLAG_HIDDEN);

    // OK button (hidden until connected)
    wifi_connecting_ok_btn = lv_btn_create(wifi_connecting_overlay);
    lv_obj_set_size(wifi_connecting_ok_btn, CYGM_BTN_W_TEXT, CYGM_BTN_H);
    lv_obj_align(wifi_connecting_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(wifi_connecting_ok_btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(wifi_connecting_ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_connecting_ok_btn, 0, 0);
    cygm_apply_ghost_btn(wifi_connecting_ok_btn);
    lv_obj_add_event_cb(wifi_connecting_ok_btn, wifi_connecting_ok_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(wifi_connecting_ok_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ok_lbl = lv_label_create(wifi_connecting_ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_lbl);

    lvgl_port_unlock();
}

// Update WiFi connecting status text (called from wifi_connect_task on Core 0)
static void update_wifi_connecting_status(const char *status) {
    if (wifi_connecting_status_label == NULL) return;
    for (int retry = 0; retry < 50; retry++) {
        if (lvgl_port_lock(1)) {
            lv_label_set_text(wifi_connecting_status_label, status);
            lvgl_port_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// WiFi connecting OK button callback
static void wifi_connecting_ok_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "WiFi connection confirmed - returning to home screen");

        // Hide overlay
        hide_wifi_connecting_overlay();

        // Load home screen first before deleting any WiFi screens
        if (screen_home != NULL) {
            lv_scr_load(screen_home);
            home_screen_active = true;
            pause_background_tasks = false;
            ESP_LOGI(TAG, "Home screen loaded successfully");
        } else {
            ESP_LOGE(TAG, "CRITICAL: screen_home is NULL! Cannot return to home screen!");
            return;
        }

        // Now it's safe to delete WiFi screens (they're no longer active)
        if (lvgl_port_lock(1)) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            ESP_LOGI(TAG, "LVGL memory before cleanup: used=%d, free=%d, frag=%d%%",
                     mon.total_size - mon.free_size, mon.free_size, mon.frag_pct);

            // Delete WiFi password screen
            if (screen_wifi_password != NULL) {
                lv_obj_del(screen_wifi_password);
                screen_wifi_password = NULL;
                password_field = NULL;
                keyboard = NULL;
                wifi_eye_label = NULL;
                ESP_LOGI(TAG, "WiFi password screen deleted");
            }

            // Delete WiFi list screen to free memory
            if (screen_wifi_list != NULL) {
                lv_obj_del(screen_wifi_list);
                screen_wifi_list = NULL;
                ESP_LOGI(TAG, "WiFi list screen deleted");
            }

            lvgl_port_unlock();
        }

        // Log LVGL memory state after cleanup
        if (lvgl_port_lock(1)) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            ESP_LOGI(TAG, "LVGL memory after cleanup: used=%d, free=%d, frag=%d%%",
                     mon.total_size - mon.free_size, mon.free_size, mon.frag_pct);
            lvgl_port_unlock();
        }

        // Log system memory after cleanup
        ESP_LOGI(TAG, "System memory after WiFi switch: free=%lu, internal=%lu, min_ever=%lu",
                 esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 esp_get_minimum_free_heap_size());
    }
}

// Hide WiFi connecting overlay
static void hide_wifi_connecting_overlay(void) {
    bool locked = false;
    for (int retry = 0; retry < 50 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) {
        ESP_LOGE(TAG, "Failed to get LVGL lock for hiding overlay");
        return;
    }

    if (wifi_connecting_overlay != NULL) {
        // Stop the progress timer before deleting the bar it points at
        stop_wifi_progress();

        // Delete overlay (this deletes all children too)
        lv_obj_del(wifi_connecting_overlay);
        wifi_connecting_overlay = NULL;
        wifi_connecting_status_label = NULL;
        wifi_connecting_bar = NULL;
        wifi_connecting_ssid_label = NULL;
        wifi_connecting_details_label = NULL;
        wifi_connecting_ok_btn = NULL;
    }

    lvgl_port_unlock();
}

// Turn the stack's disconnect reason into a headline plus one concrete next
// step, falling back to a generic pair when no reason was reported.
static void wifi_failure_message(const char **headline, const char **next_step) {
    uint8_t reason = wifi_manager_last_disconnect_reason();

    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            *headline  = "Wrong password";
            *next_step = "Re-enter it and try again";
            return;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            *headline  = "Network not found";
            *next_step = "Is it 2.4GHz? 5GHz won't work";
            return;
        default:
            break;
    }

    // Still associated after the attempt timed out: the join worked and the
    // network simply never handed out an address.
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        // Plain ASCII only: the Montserrat builds carry no typographic dashes.
        *headline  = "Connected, no internet";
        *next_step = "No address from the router";
        return;
    }

    *headline  = "Connection failed";
    *next_step = "Check the network, then retry";
}

// WiFi connection background task
static void wifi_connect_task(void *pvParameters) {
    bool locked = false;  // Reused for LVGL lock retries below

    // Show connecting overlay
    show_wifi_connecting_overlay();

    led_start_wifi_boot_blink();  // Blue blink = connecting
    update_wifi_connecting_status("Connecting to WiFi...");

    ESP_LOGI(TAG, "Connecting to: %s via wifi_manager", selected_ssid);

    // Use wifi_manager for proper state management (retry_count, event bits, auth mode)
    esp_err_t ret = wifi_manager_connect_to(selected_ssid, wifi_password);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connection successful to correct network");
        sd_log(TAG, "WiFi UI: connected to %s", selected_ssid);
        wifi_connected = true;
        led_show_success();  // Green fade = connected

        // Save credentials to NVS now that connection is successful
        nvs_save_wifi_credentials(selected_ssid, wifi_password);

        // Save as last successfully connected WiFi for boot preference
        nvs_save_last_wifi_ssid(selected_ssid);

        // Get IP address
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        char ip_str[16] = "Unknown";
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", ip_str);
        }

        // Update status and show details
        update_wifi_connecting_status("Connected!");

        // Show connection details and OK button
        locked = false;
        for (int retry = 0; retry < 50 && !locked; retry++) {
            locked = lvgl_port_lock(1);
            if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (locked) {
            if (wifi_connecting_bar != NULL) {
                // Stop the progress timer FIRST, else its 10Hz stepper overwrites
                // the 100% snap back down to ~90% within 100ms.
                stop_wifi_progress();
                lv_bar_set_value(wifi_connecting_bar, 100, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(0x15803D), LV_PART_INDICATOR);
                lv_obj_set_style_bg_grad_color(wifi_connecting_bar, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
            }

            if (wifi_connecting_overlay != NULL) {
                // Change overlay border to green
                lv_obj_set_style_border_color(wifi_connecting_overlay, lv_color_hex(COLOR_GREEN), 0);
            }

            // Hide SSID label, show details with SSID + IP
            if (wifi_connecting_ssid_label != NULL) {
                lv_obj_add_flag(wifi_connecting_ssid_label, LV_OBJ_FLAG_HIDDEN);
            }
            if (wifi_connecting_details_label != NULL) {
                char details[128];
                snprintf(details, sizeof(details), "Network: %s\nIP: %s", selected_ssid, ip_str);
                lv_label_set_text(wifi_connecting_details_label, details);
                lv_obj_clear_flag(wifi_connecting_details_label, LV_OBJ_FLAG_HIDDEN);
            }

            // Show OK button
            if (wifi_connecting_ok_btn != NULL) {
                lv_obj_clear_flag(wifi_connecting_ok_btn, LV_OBJ_FLAG_HIDDEN);
            }

            lvgl_port_unlock();
        }

        // Update WiFi status on home screen
        ESP_LOGI(TAG, "Updating WiFi status label to Connected");
        update_wifi_status_display(true);

        // Initialize SNTP and start background tasks
        initialize_sntp();

        // First-run setup never reaches the boot path that starts these, so
        // this is where they get created for a manually configured network.
        // Idempotent, and the canonical table owns stack/prio/core.
        ensure_tasks_running();

        // Joining the network is not the same as reaching the internet. SNTP is
        // the first traffic to leave the LAN, so a sync that never lands means
        // no glucose either — say so instead of a green "Connected!".
        bool have_internet = is_time_synced();
        for (int i = 0; i < 12 && !have_internet; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            have_internet = is_time_synced();
        }

        if (!have_internet) {
            ESP_LOGW(TAG, "WiFi joined but no time sync — treating as no internet");
            sd_log(TAG, "WiFi UI: %s joined, no internet (SNTP never synced)", selected_ssid);

            locked = false;
            for (int retry = 0; retry < 50 && !locked; retry++) {
                locked = lvgl_port_lock(1);
                if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (locked) {
                // The user may already have dismissed the overlay — every
                // pointer is NULLed under this same lock when that happens.
                if (wifi_connecting_status_label != NULL) {
                    lv_label_set_text(wifi_connecting_status_label, "Connected, no internet");
                    lv_obj_set_style_text_color(wifi_connecting_status_label,
                                                lv_color_hex(COLOR_ORANGE), 0);
                }
                if (wifi_connecting_details_label != NULL) {
                    char details[160];
                    snprintf(details, sizeof(details), "Network: %s\nRouter has no internet",
                             selected_ssid);
                    lv_label_set_text(wifi_connecting_details_label, details);
                    lv_obj_set_style_text_color(wifi_connecting_details_label,
                                                lv_color_hex(COLOR_TEXT_GRAY), 0);
                }
                if (wifi_connecting_overlay != NULL) {
                    lv_obj_set_style_border_color(wifi_connecting_overlay,
                                                  lv_color_hex(COLOR_ORANGE), 0);
                }
                if (wifi_connecting_bar != NULL) {
                    lv_obj_set_style_bg_color(wifi_connecting_bar,
                                              lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
                    lv_obj_set_style_bg_grad_color(wifi_connecting_bar,
                                                   lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
                }
                lvgl_port_unlock();
            }
        }

        // Don't auto-close - wait for user to press OK button
    } else {
        const char *fail_head = NULL;
        const char *fail_hint = NULL;
        wifi_failure_message(&fail_head, &fail_hint);
        uint8_t fail_reason = wifi_manager_last_disconnect_reason();

        ESP_LOGE(TAG, "WiFi connection failed: %s (reason=%d)", fail_head, fail_reason);
        sd_log(TAG, "WiFi UI: FAILED to connect to %s — %s (reason=%d)",
               selected_ssid, fail_head, fail_reason);
        led_start_error_blink();  // Red pulse = failed

        // Turn bar red at current position
        locked = false;
        for (int retry = 0; retry < 50 && !locked; retry++) {
            locked = lvgl_port_lock(1);
            if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (locked) {
            if (wifi_connecting_bar != NULL) {
                // Stop the progress timer so the red failure bar holds steady
                stop_wifi_progress();
                lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
                lv_obj_set_style_bg_grad_color(wifi_connecting_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
            }
            if (wifi_connecting_overlay != NULL) {
                lv_obj_set_style_border_color(wifi_connecting_overlay, lv_color_hex(COLOR_RED), 0);
            }
            if (wifi_connecting_status_label != NULL) {
                lv_label_set_text(wifi_connecting_status_label, fail_head);
                lv_obj_set_style_text_color(wifi_connecting_status_label, lv_color_hex(COLOR_RED), 0);
            }
            // Reuse the (still hidden) details label for the SSID + next step.
            if (wifi_connecting_ssid_label != NULL) {
                lv_obj_add_flag(wifi_connecting_ssid_label, LV_OBJ_FLAG_HIDDEN);
            }
            if (wifi_connecting_details_label != NULL) {
                char details[160];
                snprintf(details, sizeof(details), "%s\n%s", selected_ssid, fail_hint);
                lv_label_set_text(wifi_connecting_details_label, details);
                lv_obj_set_style_text_color(wifi_connecting_details_label,
                                            lv_color_hex(COLOR_TEXT_GRAY), 0);
                lv_obj_clear_flag(wifi_connecting_details_label, LV_OBJ_FLAG_HIDDEN);
            }
            lvgl_port_unlock();
        }

        // Long enough to read the reason and the next step.
        vTaskDelay(pdMS_TO_TICKS(4500));

        // Hide connecting overlay
        hide_wifi_connecting_overlay();

        // Update WiFi status on home screen
        ESP_LOGI(TAG, "Updating WiFi status label to Failed");
        update_wifi_status_display(false);
    }

    vTaskDelete(NULL);  // Delete task when done
}

// OK button callback for WiFi password screen
static void wifi_password_ok_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        // OK button pressed - start connection in background task
        ESP_LOGI(TAG, "Connecting to WiFi: %s with password: %s", selected_ssid, wifi_password);

        // Start WiFi connection in background task (don't block UI)
        xTaskCreate(wifi_connect_task, "wifi_connect", 8192, NULL, 5, NULL);
    }
}

// Keyboard event callback
static void keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    static lv_keyboard_mode_t prev_mode = LV_KEYBOARD_MODE_TEXT_LOWER;

    // Safety check
    if (keyboard == NULL) {
        return;
    }

    // Handle cancel/close button
    if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Password entry cancelled, returning to network list");
        if (lvgl_port_lock(1)) {
            lv_scr_load(screen_wifi_list);
            lvgl_port_unlock();
        }
        return;
    }

    // Handle OK/Ready button
    if (code == LV_EVENT_READY) {
        // Keyboard ready button pressed - start connection in background task
        ESP_LOGI(TAG, "Connecting to WiFi: %s with password: %s", selected_ssid, wifi_password);

        // Start WiFi connection in background task (don't block UI)
        xTaskCreate(wifi_connect_task, "wifi_connect", 8192, NULL, 5, NULL);
        return;
    }

    // Detect shift/caps button presses by monitoring mode changes
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_keyboard_mode_t curr_mode = lv_keyboard_get_mode(keyboard);

        // Check if mode changed (shift button pressed)
        if (curr_mode != prev_mode) {
            if (curr_mode == LV_KEYBOARD_MODE_TEXT_UPPER) {
                // Shift pressed - record time and wait for character or timeout
                last_shift_press_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                shift_waiting_for_char = true;
                ESP_LOGI(TAG, "Shift pressed, waiting for character or long press (2000ms for caps lock)");
            } else if (curr_mode == LV_KEYBOARD_MODE_TEXT_LOWER) {
                // Mode changed to lowercase - disable caps lock if it was on
                if (caps_lock_enabled) {
                    caps_lock_enabled = false;
                    shift_waiting_for_char = false;
                    ESP_LOGI(TAG, "Caps lock disabled");
                }
            }
            prev_mode = curr_mode;
        }
    }
}
