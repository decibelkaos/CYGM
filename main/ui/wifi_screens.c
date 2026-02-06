/*
 * wifi_screens.c
 *
 * WiFi configuration screens implementation
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

static const char *TAG = "WIFI_SCREENS";

// WiFi scanning overlay
static lv_obj_t *wifi_scanning_overlay = NULL;
static lv_obj_t *wifi_scanning_bar = NULL;

// WiFi removal confirmation overlay
static lv_obj_t *wifi_removal_overlay = NULL;
static lv_obj_t *wifi_removal_slider = NULL;
static char wifi_removal_ssid[MAX_SSID_LEN] = {0};

// WiFi connecting status overlay
static lv_obj_t *wifi_connecting_overlay = NULL;
static lv_obj_t *wifi_connecting_status_label = NULL;
static lv_obj_t *wifi_connecting_bar = NULL;
static lv_obj_t *wifi_connecting_details_label = NULL;  // Shows SSID and IP
static lv_obj_t *wifi_connecting_ok_btn = NULL;  // OK button to close overlay

// Forward declarations for internal event callbacks
static void network_item_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void password_field_event_cb(lv_event_t *e);
static void wifi_password_cancel_btn_cb(lv_event_t *e);
static void wifi_password_ok_btn_cb(lv_event_t *e);
static void wifi_delete_btn_event_cb(lv_event_t *e);  // Delete button for saved networks
static void wifi_connect_saved_event_cb(lv_event_t *e);  // Connect to saved network
static void wifi_switch_btn_event_cb(lv_event_t *e);  // Switch to saved network (green check)
static void wifi_connecting_ok_btn_cb(lv_event_t *e);  // OK button on connecting overlay
static void hide_wifi_connecting_overlay(void);  // Hide connecting overlay
static void wifi_connect_task(void *pvParameters);  // WiFi connection background task

// Animation callback for scanning bar
static void anim_bar_set_value(void *bar, int32_t val) {
    lv_bar_set_value((lv_obj_t *)bar, val, LV_ANIM_OFF);
}

// Helper to update WiFi status display
static void update_wifi_status_display(bool connected) {
    if (lvgl_port_lock(0)) {
        if (connected) {
            // Green WiFi icon for connected (no text)
            lv_label_set_recolor(label_wifi_status, false);  // Disable recolor
            lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
        } else {
            // Gray WiFi icon with RED warning for disconnected
            lv_label_set_recolor(label_wifi_status, true);  // Enable recolor for red warning
            lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
            lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
        lvgl_port_unlock();
    }
}

// Show WiFi scanning overlay with animated loading bar
void show_wifi_scanning_overlay(void) {
    if (!lvgl_port_lock(0)) {
        return;
    }

    // Create semi-transparent overlay
    wifi_scanning_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_scanning_overlay, 280, 200);
    lv_obj_align(wifi_scanning_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_scanning_overlay, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(wifi_scanning_overlay, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(wifi_scanning_overlay, 2, 0);
    lv_obj_set_style_radius(wifi_scanning_overlay, 12, 0);
    lv_obj_set_style_pad_all(wifi_scanning_overlay, 20, 0);
    lv_obj_clear_flag(wifi_scanning_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // "Scanning..." label
    lv_obj_t *label = lv_label_create(wifi_scanning_overlay);
    lv_label_set_text(label, "Scanning WiFi Networks...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    // Loading bar
    wifi_scanning_bar = lv_bar_create(wifi_scanning_overlay);
    lv_obj_set_size(wifi_scanning_bar, 240, 10);
    lv_obj_align(wifi_scanning_bar, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(wifi_scanning_bar, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_bg_color(wifi_scanning_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_bar_set_range(wifi_scanning_bar, 0, 100);
    lv_bar_set_value(wifi_scanning_bar, 0, LV_ANIM_OFF);

    // Animate loading bar
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wifi_scanning_bar);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_bar_set_value);
    lv_anim_start(&a);

    lvgl_port_unlock();
}

// Hide WiFi scanning overlay
void hide_wifi_scanning_overlay(void) {
    if (!lvgl_port_lock(0)) {
        return;
    }

    if (wifi_scanning_overlay != NULL) {
        // Stop all animations on the bar before deleting
        if (wifi_scanning_bar != NULL) {
            lv_anim_del(wifi_scanning_bar, NULL);
        }

        // Delete overlay (this deletes all children too)
        lv_obj_del(wifi_scanning_overlay);
        wifi_scanning_overlay = NULL;
        wifi_scanning_bar = NULL;
    }

    lvgl_port_unlock();
}

// WiFi removal slider event callback
static void wifi_removal_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        int value = lv_slider_get_value(wifi_removal_slider);

        // If slider is at 100 (fully right), remove the network
        if (value >= 100) {
            ESP_LOGI(TAG, "Slider confirmed - removing WiFi network: %s", wifi_removal_ssid);

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
                led_set_breathing_color(127, 127, 0);  // Yellow = disconnected
            }

            // Close overlay
            if (lvgl_port_lock(0)) {
                if (wifi_removal_overlay != NULL) {
                    lv_obj_del(wifi_removal_overlay);
                    wifi_removal_overlay = NULL;
                    wifi_removal_slider = NULL;
                }
                lvgl_port_unlock();
            }

            // Rescan WiFi to update list
            show_wifi_scanning_overlay();
            xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
        }
    }
}

// Cancel button callback for WiFi removal overlay
static void wifi_removal_cancel_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "WiFi removal cancelled");

        if (lvgl_port_lock(0)) {
            if (wifi_removal_overlay != NULL) {
                lv_obj_del(wifi_removal_overlay);
                wifi_removal_overlay = NULL;
                wifi_removal_slider = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Show WiFi removal confirmation overlay
void show_wifi_removal_overlay(const char *ssid) {
    if (ssid == NULL) {
        return;
    }

    // Save SSID for removal
    strncpy(wifi_removal_ssid, ssid, sizeof(wifi_removal_ssid) - 1);

    if (!lvgl_port_lock(0)) {
        return;
    }

    // Close if already open
    if (wifi_removal_overlay != NULL) {
        lv_obj_del(wifi_removal_overlay);
        wifi_removal_overlay = NULL;
        wifi_removal_slider = NULL;
    }

    // Create overlay (280x200, centered)
    wifi_removal_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_removal_overlay, 280, 200);
    lv_obj_center(wifi_removal_overlay);
    lv_obj_set_style_bg_color(wifi_removal_overlay, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(wifi_removal_overlay, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(wifi_removal_overlay, 2, 0);
    lv_obj_set_style_radius(wifi_removal_overlay, 12, 0);
    lv_obj_clear_flag(wifi_removal_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(title, "Remove WiFi?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // SSID display
    lv_obj_t *ssid_label = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(ssid_label, ssid);
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_width(ssid_label, 250);
    lv_obj_set_style_text_align(ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_MID, 0, 45);

    // Instruction text
    lv_obj_t *instruction = lv_label_create(wifi_removal_overlay);
    lv_label_set_text(instruction, "Slide right to confirm");
    lv_obj_set_style_text_font(instruction, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(instruction, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(instruction, LV_ALIGN_TOP_MID, 0, 75);

    // Slider (horizontal, slide right to confirm)
    wifi_removal_slider = lv_slider_create(wifi_removal_overlay);
    lv_obj_set_size(wifi_removal_slider, 240, 12);
    lv_obj_align(wifi_removal_slider, LV_ALIGN_CENTER, 0, 10);
    lv_slider_set_range(wifi_removal_slider, 0, 100);
    lv_slider_set_value(wifi_removal_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(wifi_removal_slider, lv_color_hex(COLOR_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_removal_slider, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(wifi_removal_slider, lv_color_hex(COLOR_RED), LV_PART_KNOB);
    lv_obj_add_event_cb(wifi_removal_slider, wifi_removal_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(wifi_removal_overlay);
    lv_obj_set_size(cancel_btn, 100, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);

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

    ESP_LOGI(TAG, "Filtered to %d unique networks", wifi_network_count);

    // Hide scanning overlay
    hide_wifi_scanning_overlay();

    if (wifi_network_count == 0) {
        ESP_LOGW(TAG, "No networks found");
        return;
    }

    // Create network list UI
    if (lvgl_port_lock(0)) {
        // Remove any existing network list
        lv_obj_clean(screen_wifi_list);

        // Recreate header and back button
        lv_obj_t *header = lv_label_create(screen_wifi_list);
        lv_label_set_text(header, "WiFi Networks");
        lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

        // Back button (matching Settings screen style)
        lv_obj_t *back_btn = lv_btn_create(screen_wifi_list);
        lv_obj_set_size(back_btn, 36, 36);
        lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
        lv_obj_add_style(back_btn, &style_card, 0);

        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(back_lbl);
        lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

        // Create scrollable list
        lv_obj_t *list = lv_obj_create(screen_wifi_list);
        lv_obj_set_size(list, 300, 180);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_radius(list, 12, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 5, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

        // Add network items (up to 5)
        int max_networks = wifi_network_count < 5 ? wifi_network_count : 5;
        for (int i = 0; i < max_networks; i++) {
            lv_obj_t *row = lv_btn_create(list);
            lv_obj_set_width(row, 290);
            lv_obj_set_height(row, 35);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);

            // Signal strength bars (based on RSSI)
            int8_t rssi = wifi_networks[i].rssi;
            int bars = 0;
            if (rssi > -50) bars = 3;
            else if (rssi > -60) bars = 2;
            else if (rssi > -70) bars = 1;

            // Draw signal bars (3 vertical bars of increasing heights, bottom-aligned with text baseline)
            for (int b = 0; b < 3; b++) {
                lv_obj_t *bar = lv_obj_create(row);
                int bar_height = 6 + (b * 4);  // 6px, 10px, 14px
                lv_obj_set_size(bar, 4, bar_height);  // 4px wide vertical bars

                // Position: x increases, y positioned so all bars bottom-align at y=16 (text baseline)
                int bar_x = 8 + (b * 5);  // Space bars 5px apart
                int bar_y = 16 - bar_height;  // All bars bottom-align at y=16
                lv_obj_set_pos(bar, bar_x, bar_y);

                if (b < bars) {
                    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
                } else {
                    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TEXT_DIM), 0);
                }
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 1, 0);  // Slight rounding
                lv_obj_set_style_pad_all(bar, 0, 0);
            }

            // SSID
            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, (const char *)wifi_networks[i].ssid);
            lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 25, 0);

            // Check if this network is saved
            bool is_saved = nvs_is_wifi_saved((const char *)wifi_networks[i].ssid);

            if (is_saved) {
                // Green square with checkmark for switching networks
                lv_obj_t *switch_btn = lv_obj_create(row);
                lv_obj_set_size(switch_btn, 18, 18);
                lv_obj_align(switch_btn, LV_ALIGN_RIGHT_MID, -28, 0);  // Left of delete button
                lv_obj_set_style_bg_color(switch_btn, lv_color_hex(COLOR_GREEN), 0);
                lv_obj_set_style_radius(switch_btn, 4, 0);
                lv_obj_set_style_border_width(switch_btn, 0, 0);
                lv_obj_set_style_pad_all(switch_btn, 0, 0);
                lv_obj_add_flag(switch_btn, LV_OBJ_FLAG_CLICKABLE);

                // Checkmark icon inside square
                lv_obj_t *check_icon = lv_label_create(switch_btn);
                lv_label_set_text(check_icon, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(check_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
                lv_obj_set_style_text_font(check_icon, &lv_font_montserrat_12, 0);
                lv_obj_center(check_icon);

                // Add click handler for switch button
                lv_obj_add_event_cb(switch_btn, wifi_switch_btn_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
                lv_obj_clear_flag(switch_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

                // Red square with X for saved networks (removal button)
                lv_obj_t *delete_btn = lv_obj_create(row);
                lv_obj_set_size(delete_btn, 18, 18);
                lv_obj_align(delete_btn, LV_ALIGN_RIGHT_MID, -5, 0);
                lv_obj_set_style_bg_color(delete_btn, lv_color_hex(COLOR_RED), 0);
                lv_obj_set_style_radius(delete_btn, 4, 0);
                lv_obj_set_style_border_width(delete_btn, 0, 0);
                lv_obj_set_style_pad_all(delete_btn, 0, 0);
                lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_CLICKABLE);

                // X icon inside square
                lv_obj_t *x_icon = lv_label_create(delete_btn);
                lv_label_set_text(x_icon, LV_SYMBOL_CLOSE);
                lv_obj_set_style_text_color(x_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
                lv_obj_set_style_text_font(x_icon, &lv_font_montserrat_12, 0);
                lv_obj_center(x_icon);

                // Add click handler for delete button (separate from row)
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

// Create WiFi list screen
void create_wifi_list_screen(void) {
    screen_wifi_list = lv_obj_create(NULL);
    lv_obj_add_style(screen_wifi_list, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_wifi_list);
    lv_label_set_text(header, "WiFi Networks");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button (matching Settings screen style)
    lv_obj_t *back_btn = lv_btn_create(screen_wifi_list);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Network list will be populated dynamically by wifi_scan_and_show_networks()
}

// Create WiFi password entry screen
void create_wifi_password_screen(void) {
    screen_wifi_password = lv_obj_create(NULL);
    lv_obj_add_style(screen_wifi_password, &style_bg, 0);

    // Cancel button (red, top-left)
    lv_obj_t *cancel_btn = lv_btn_create(screen_wifi_password);
    lv_obj_set_size(cancel_btn, 70, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);

    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, wifi_password_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    // OK button (green, top-right)
    lv_obj_t *ok_btn = lv_btn_create(screen_wifi_password);
    lv_obj_set_size(ok_btn, 70, 30);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_radius(ok_btn, 6, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok_btn, wifi_password_ok_btn_cb, LV_EVENT_CLICKED, NULL);

    // Header (centered between buttons)
    lv_obj_t *header = lv_label_create(screen_wifi_password);
    lv_label_set_text(header, "Enter Password");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 8);

    // Password card
    lv_obj_t *card = lv_obj_create(screen_wifi_password);
    lv_obj_set_size(card, 300, 80);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Password field (text area for keyboard input)
    password_field = lv_textarea_create(card);
    lv_obj_set_size(password_field, 280, 36);
    lv_obj_align(password_field, LV_ALIGN_CENTER, 0, 0);
    lv_textarea_set_placeholder_text(password_field, "Password");
    lv_textarea_set_one_line(password_field, true);
    lv_textarea_set_password_mode(password_field, true);
    lv_textarea_set_text_selection(password_field, false);
    lv_obj_set_style_text_font(password_field, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(password_field, password_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Password label (visual display, hidden behind password field)
    password_label = lv_label_create(card);
    lv_label_set_text(password_label, "");
    lv_obj_set_style_text_font(password_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(password_label, LV_ALIGN_CENTER, 0, 0);

    // Keyboard
    keyboard = lv_keyboard_create(screen_wifi_password);
    lv_obj_set_size(keyboard, 320, 110);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, password_field);
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
        if (lvgl_port_lock(0)) {
            // Create password screen if it doesn't exist
            if (screen_wifi_password == NULL) {
                create_wifi_password_screen();
            }

            // Clear password field and display
            lv_textarea_set_text(password_field, "");
            lv_label_set_text(password_label, "");

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
                xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, 5, NULL);
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
                xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, 5, NULL);
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

        // Show removal confirmation overlay (task 26 - to be implemented)
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

// Cancel button callback for WiFi password screen
static void wifi_password_cancel_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Password entry cancelled, returning to network list");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_wifi_list);
            lvgl_port_unlock();
        }
    }
}

// Show WiFi connecting overlay
static void show_wifi_connecting_overlay(void) {
    if (!lvgl_port_lock(0)) {
        return;
    }

    // Create smaller overlay (260x180, centered) to save memory
    wifi_connecting_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_connecting_overlay, 260, 180);
    lv_obj_center(wifi_connecting_overlay);
    lv_obj_set_style_bg_color(wifi_connecting_overlay, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(wifi_connecting_overlay, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(wifi_connecting_overlay, 2, 0);
    lv_obj_set_style_radius(wifi_connecting_overlay, 12, 0);
    lv_obj_clear_flag(wifi_connecting_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Loading bar (animated progress bar)
    wifi_connecting_bar = lv_bar_create(wifi_connecting_overlay);
    lv_obj_set_size(wifi_connecting_bar, 220, 8);  // Smaller bar
    lv_obj_align(wifi_connecting_bar, LV_ALIGN_CENTER, 0, -35);  // Moved up
    lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_bg_color(wifi_connecting_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_bar_set_range(wifi_connecting_bar, 0, 100);
    lv_bar_set_value(wifi_connecting_bar, 0, LV_ANIM_OFF);

    // Animate loading bar
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wifi_connecting_bar);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_bar_set_value);
    lv_anim_start(&a);

    // Status label (near top of overlay)
    wifi_connecting_status_label = lv_label_create(wifi_connecting_overlay);
    lv_label_set_text(wifi_connecting_status_label, "Connecting...");
    lv_obj_set_style_text_font(wifi_connecting_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wifi_connecting_status_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(wifi_connecting_status_label, LV_ALIGN_TOP_MID, 0, 20);  // Near top

    // Details label (SSID and IP - initially hidden, positioned below status)
    wifi_connecting_details_label = lv_label_create(wifi_connecting_overlay);
    lv_label_set_text(wifi_connecting_details_label, "");
    lv_obj_set_style_text_font(wifi_connecting_details_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_connecting_details_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_align(wifi_connecting_details_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(wifi_connecting_details_label, 240);  // Smaller to match overlay
    lv_obj_align(wifi_connecting_details_label, LV_ALIGN_TOP_MID, 0, 50);  // Below status label
    lv_obj_add_flag(wifi_connecting_details_label, LV_OBJ_FLAG_HIDDEN);

    // OK button (initially hidden, shown when connected)
    wifi_connecting_ok_btn = lv_btn_create(wifi_connecting_overlay);
    lv_obj_set_size(wifi_connecting_ok_btn, 80, 35);
    lv_obj_align(wifi_connecting_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(wifi_connecting_ok_btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_radius(wifi_connecting_ok_btn, 8, 0);
    lv_obj_add_event_cb(wifi_connecting_ok_btn, wifi_connecting_ok_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(wifi_connecting_ok_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ok_lbl = lv_label_create(wifi_connecting_ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_lbl);

    lvgl_port_unlock();
}

// Update WiFi connecting status text
static void update_wifi_connecting_status(const char *status) {
    if (wifi_connecting_status_label != NULL && lvgl_port_lock(0)) {
        lv_label_set_text(wifi_connecting_status_label, status);
        lvgl_port_unlock();
    }
}

// WiFi connecting OK button callback
static void wifi_connecting_ok_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "WiFi connection confirmed - returning to home screen");

        // Hide overlay
        hide_wifi_connecting_overlay();

        // Small delay to let LVGL cleanup freed objects
        vTaskDelay(pdMS_TO_TICKS(100));

        // CRITICAL: Load home screen FIRST before deleting any screens
        // Deleting a screen while it's active corrupts LVGL internal state!
        if (lvgl_port_lock(0)) {
            // Verify screen_home is valid
            if (screen_home != NULL) {
                lv_scr_load(screen_home);
                home_screen_active = true;
                pause_background_tasks = false;
                ESP_LOGI(TAG, "Home screen loaded successfully");
            } else {
                ESP_LOGE(TAG, "CRITICAL: screen_home is NULL! Cannot return to home screen!");
                lvgl_port_unlock();
                return;
            }
            lvgl_port_unlock();
        }

        // Small delay to let screen transition complete
        vTaskDelay(pdMS_TO_TICKS(100));

        // NOW it's safe to delete WiFi screens (they're no longer active)
        if (lvgl_port_lock(0)) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            ESP_LOGI(TAG, "LVGL memory before cleanup: used=%d, free=%d, frag=%d%%",
                     mon.total_size - mon.free_size, mon.free_size, mon.frag_pct);

            // Delete WiFi password screen
            if (screen_wifi_password != NULL) {
                lv_obj_del(screen_wifi_password);
                screen_wifi_password = NULL;
                password_field = NULL;
                password_label = NULL;
                keyboard = NULL;
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

        // Small delay for memory to settle and cleanup to complete
        vTaskDelay(pdMS_TO_TICKS(200));

        // Log LVGL memory state after cleanup
        if (lvgl_port_lock(0)) {
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
    if (!lvgl_port_lock(0)) {
        return;
    }

    if (wifi_connecting_overlay != NULL) {
        // Stop all animations on the bar before deleting
        if (wifi_connecting_bar != NULL) {
            lv_anim_del(wifi_connecting_bar, NULL);
        }

        // Delete overlay (this deletes all children too)
        lv_obj_del(wifi_connecting_overlay);
        wifi_connecting_overlay = NULL;
        wifi_connecting_status_label = NULL;
        wifi_connecting_bar = NULL;
        wifi_connecting_details_label = NULL;
        wifi_connecting_ok_btn = NULL;
    }

    lvgl_port_unlock();
}

// WiFi connection background task
static void wifi_connect_task(void *pvParameters) {
    // Show connecting overlay
    show_wifi_connecting_overlay();

    // CRITICAL: Fully disconnect and clear old connection
    ESP_LOGI(TAG, "Forcefully disconnecting from current WiFi...");
    wifi_connected = false;

    // Stop WiFi completely to ensure clean disconnect
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for disconnect to complete

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));  // Wait for WiFi to stop

    ESP_LOGI(TAG, "WiFi stopped, now configuring for: %s", selected_ssid);

    // Connect to WiFi in background using selected SSID/password
    led_set_breathing_color(0, 0, 127);  // Blue = connecting
    update_wifi_connecting_status("Connecting to WiFi...");

    // Configure WiFi with selected credentials
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, selected_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password));

    // Set the new config BEFORE starting WiFi
    ESP_LOGI(TAG, "Setting WiFi config for: %s", selected_ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Start WiFi with new config
    ESP_LOGI(TAG, "Starting WiFi...");
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for WiFi to start

    // Now connect
    ESP_LOGI(TAG, "Connecting to: %s", selected_ssid);
    esp_wifi_connect();

    // Wait up to 15 seconds for connection (increased timeout)
    vTaskDelay(pdMS_TO_TICKS(15000));

    // Verify we connected to the RIGHT network
    bool connected_to_correct_network = false;
    if (wifi_manager_is_connected()) {
        // Check the actual connected SSID
        wifi_config_t current_config;
        if (esp_wifi_get_config(WIFI_IF_STA, &current_config) == ESP_OK) {
            char *connected_ssid = (char *)current_config.sta.ssid;
            ESP_LOGI(TAG, "Connected to SSID: %s (requested: %s)", connected_ssid, selected_ssid);

            if (strcmp(connected_ssid, selected_ssid) == 0) {
                connected_to_correct_network = true;
            } else {
                ESP_LOGW(TAG, "Connected to wrong network! Disconnecting...");
                wifi_manager_disconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    if (connected_to_correct_network) {
        ESP_LOGI(TAG, "WiFi connection successful to correct network");
        wifi_connected = true;
        led_set_breathing_color(0, 127, 0);  // Green = connected

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
        if (lvgl_port_lock(0)) {
            // Stop loading bar animation and hide it
            lv_anim_del(wifi_connecting_bar, NULL);
            lv_obj_add_flag(wifi_connecting_bar, LV_OBJ_FLAG_HIDDEN);

            // Show details
            char details[128];
            snprintf(details, sizeof(details), "Network: %s\nIP Address: %s", selected_ssid, ip_str);
            lv_label_set_text(wifi_connecting_details_label, details);
            lv_obj_clear_flag(wifi_connecting_details_label, LV_OBJ_FLAG_HIDDEN);

            // Show OK button
            lv_obj_clear_flag(wifi_connecting_ok_btn, LV_OBJ_FLAG_HIDDEN);

            lvgl_port_unlock();
        }

        // Update WiFi status on home screen
        ESP_LOGI(TAG, "Updating WiFi status label to Connected");
        update_wifi_status_display(true);

        // Initialize SNTP and start background tasks
        initialize_sntp();

        // Start time update task if not already running
        xTaskCreate(time_update_task, "time_update", 4096, NULL, 1, NULL);

        // Start weather task if not already running (HTTP mode, no SSL)
        if (weather_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting weather task (HTTP mode)...");
            xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
        }

        // Start glucose task if not already running and Dexcom tokens exist
        if (glucose_task_handle == NULL && nvs_has_dexcom_tokens()) {
            ESP_LOGI(TAG, "Dexcom credentials found - starting glucose update task...");
            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
        }

        // Don't auto-close - wait for user to press OK button
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
        led_set_breathing_color(127, 0, 0);  // Red = failed

        update_wifi_connecting_status("Connection failed!");
        vTaskDelay(pdMS_TO_TICKS(2000));

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
        xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, 5, NULL);
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
        if (lvgl_port_lock(0)) {
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
        xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, 5, NULL);
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
