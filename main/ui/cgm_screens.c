/*
 * cgm_screens.c - provider setup screens for Dexcom, LibreLinkUp and
 * Nightscout: credential entry, sign-in, and status.
 */

#include "cgm_screens.h"
#include "shared_state.h"
#include "main.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "nightscout_api.h"
#include "cgm_types.h"
#include "nvs_config.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "sd_logger.h"
#include "ui/menu_screen.h"
#include "ui/home_screen.h"
#include "ui/wifi_screens.h"   // create_wifi_list_screen() for the WiFi-required prompt
#include "wifi_manager.h"      // wifi_manager_is_connected()

static const char *TAG = "CGM_SCREENS";
static volatile bool dexcom_login_in_progress = false;

// Connecting overlay (shown while TLS handshake + auth runs)
static lv_obj_t *connecting_overlay = NULL;

// "WiFi required" prompt (shown when a CGM login is attempted with no network)
static lv_obj_t *wifi_required_overlay = NULL;

// Password visibility toggle
static bool dexcom_pwd_visible = false;
static lv_obj_t *dexcom_pwd_eye_label = NULL;

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *cgm_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
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

// ---------------------------------------------------------------------------
// Connecting overlay — shown immediately when the user taps Sign In
// ---------------------------------------------------------------------------

// LVGL fires DELETE on EVERY teardown path (explicit del, lv_obj_clean of the
// parent, screen delete by the inactivity watchdog), so clearing the static
// here is what stops it outliving the overlay and pointing at freed memory.
static void connecting_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    connecting_overlay = NULL;
}

static void show_connecting_overlay(const char *provider_name) {
    if (connecting_overlay != NULL) return;

    // Dimmed backdrop
    connecting_overlay = lv_obj_create(lv_scr_act());
    lv_obj_add_event_cb(connecting_overlay, connecting_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(connecting_overlay);
    lv_obj_set_size(connecting_overlay, 320, 240);
    lv_obj_set_style_bg_color(connecting_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(connecting_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(connecting_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(connecting_overlay);

    // Glass card — themed connecting overlay
    lv_obj_t *card = lv_obj_create(connecting_overlay);
    lv_obj_set_size(card, 220, 130);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Spinner
    lv_obj_t *spinner = lv_spinner_create(card, 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_arc_width(spinner, 4, 0);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Connecting...");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    // Subtitle
    char sub_text[48];
    snprintf(sub_text, sizeof(sub_text), "Signing in to %s", provider_name);
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, sub_text);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, 200);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 90);

    ESP_LOGI(TAG, "Connecting overlay shown for %s", provider_name);
}

static void hide_connecting_overlay(void) {
    if (connecting_overlay != NULL) {
        lv_obj_del(connecting_overlay);
        connecting_overlay = NULL;
    }
}

// ---------------------------------------------------------------------------
// "WiFi required" prompt — pushes the user to WiFi setup instead of letting a
// CGM login hang on "Connecting..." with no network (TLS would never complete).
// ---------------------------------------------------------------------------
// Same delete contract as the connecting overlay above.
static void wifi_required_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    wifi_required_overlay = NULL;
}

static void hide_wifi_required_prompt(void) {
    if (wifi_required_overlay != NULL) {
        lv_obj_del(wifi_required_overlay);
        wifi_required_overlay = NULL;
    }
}

// "Set Up WiFi" — dismiss the prompt and jump straight to the WiFi list screen.
static void wifi_required_setup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_required_prompt();
    if (screen_wifi_list != NULL) {
        lv_obj_del(screen_wifi_list);
        screen_wifi_list = NULL;
    }
    create_wifi_list_screen();
    lv_scr_load(screen_wifi_list);
}

static void wifi_required_cancel_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_required_prompt();
}

static void show_wifi_required_prompt(void) {
    if (wifi_required_overlay != NULL) return;

    // Dimmed backdrop
    wifi_required_overlay = lv_obj_create(lv_scr_act());
    lv_obj_add_event_cb(wifi_required_overlay, wifi_required_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(wifi_required_overlay);
    lv_obj_set_size(wifi_required_overlay, 320, 240);
    lv_obj_set_style_bg_color(wifi_required_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_required_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(wifi_required_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wifi_required_overlay);

    // Glass card
    lv_obj_t *card = lv_obj_create(wifi_required_overlay);
    lv_obj_set_size(card, 250, 165);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi glyph
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "WiFi Required");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    // Message
    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, "Connect to WiFi before\nsigning in to your CGM.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 68);

    // "Set Up WiFi" — primary action
    lv_obj_t *setup_btn = lv_btn_create(card);
    lv_obj_set_size(setup_btn, 130, 36);
    lv_obj_align(setup_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(setup_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(setup_btn, 8, 0);
    lv_obj_set_style_shadow_width(setup_btn, 0, 0);
    lv_obj_add_event_cb(setup_btn, wifi_required_setup_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *setup_lbl = lv_label_create(setup_btn);
    lv_label_set_text(setup_lbl, LV_SYMBOL_WIFI "  Set Up");
    lv_obj_set_style_text_font(setup_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(setup_lbl);

    // "Cancel" — ghost dismiss
    lv_obj_t *cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, 90, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COLOR_PRESSED), 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, wifi_required_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_center(cancel_lbl);

    ESP_LOGI(TAG, "WiFi-required prompt shown (login blocked, no network)");
}

// Gate for CGM logins: returns true if WiFi is up; otherwise shows the
// WiFi-required prompt and returns false so the caller bails out early.
static bool require_wifi_or_prompt(void) {
    if (wifi_manager_is_connected()) return true;
    ESP_LOGW(TAG, "CGM login blocked — WiFi not connected, prompting for setup");
    show_wifi_required_prompt();
    return false;
}

static void dexcom_login_task(void *pvParameters) {
    char username[64] = {0};
    char password[64] = {0};
    strncpy(username, dexcom_username_buf, sizeof(username) - 1);
    strncpy(password, dexcom_password_buf, sizeof(password) - 1);

    ESP_LOGI(TAG, "Dexcom login task started for user: %s", username);

    // Free heap for TLS handshake — same pattern as glucose_update_task re-auth
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Dexcom login: network mutex timeout");
        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
        dexcom_login_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    bool tasks_deleted = delete_background_tasks_for_ssl("dexcom-login");

    ESP_LOGI(TAG, "Dexcom login: heap free=%lu largest=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    if (dexcom_authenticate(username, password) == ESP_OK) {
        ESP_LOGI(TAG, "Dexcom login successful");
        sd_log(TAG, "Dexcom: login OK for %s", username);
        nvs_set_dexcom_credentials(username, password);
        nvs_save_cgm_type("dexcom");

        dexcom_close_persistent_client();  // Close auth connection; glucose task opens fresh

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after Dexcom login...");
            ensure_tasks_running();  // Canonical stack/prio/core
        }

        ESP_LOGI(TAG, "Triggering immediate glucose fetch after successful login");
        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lv_scr_load(screen_home);
            // Clean up all CGM screens
            if (screen_dexcom_login != NULL) { lv_obj_del(screen_dexcom_login); screen_dexcom_login = NULL; }
            if (screen_dexcom_auth != NULL) { lv_obj_del(screen_dexcom_auth); screen_dexcom_auth = NULL; }
            if (screen_cgm_menu != NULL) { lv_obj_del(screen_cgm_menu); screen_cgm_menu = NULL; }
            home_screen_active = true;
            pause_background_tasks = false;

            // Show one-time login success overlay
            show_login_success_overlay_ui();

            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "Failed to lock LVGL to return to home screen after login");
        }
    } else {
        ESP_LOGE(TAG, "Dexcom login failed");
        sd_log(TAG, "Dexcom: login FAILED for %s", username);

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
    }

    dexcom_login_in_progress = false;
    vTaskDelete(NULL);
}

static void dexcom_back_event_cb(lv_event_t *e);
static void dexcom_logout_btn_event_cb(lv_event_t *e);
static void cgm_back_event_cb(lv_event_t *e);
static void dexcom_btn_event_cb(lv_event_t *e);
static void librelinkup_btn_event_cb(lv_event_t *e);
static void nightscout_btn_event_cb(lv_event_t *e);
static void dexcom_view_status_btn_event_cb(lv_event_t *e);
static void dexcom_open_login_event_cb(lv_event_t *e);
static void dexcom_username_tap_event_cb(lv_event_t *e);
static void dexcom_password_tap_event_cb(lv_event_t *e);
static void dexcom_login_btn_event_cb(lv_event_t *e);
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e);
static void dexcom_username_entry_field_event_cb(lv_event_t *e);
static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e);
static void dexcom_password_entry_field_event_cb(lv_event_t *e);
static void dexcom_to_auth_event_cb(lv_event_t *e);

// Dexcom back button event handler — return to CGM menu
static void dexcom_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_cgm_menu_screen();
            lv_scr_load(screen_cgm_menu);
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_logout_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Dexcom logout requested");

        // Clear credentials from NVS
        nvs_clear_dexcom_credentials();

        // Logout from API
        dexcom_logout();

        // Clear local buffers
        memset(dexcom_username_buf, 0, sizeof(dexcom_username_buf));
        memset(dexcom_password_buf, 0, sizeof(dexcom_password_buf));

        // Clear glucose data
        glucose_data_valid = false;
        current_glucose = 0;
        glucose_status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
        first_glucose_received = false;  // Reset so success sound plays on next login
        update_glucose_display();

        ESP_LOGI(TAG, "Dexcom logout complete");
        sd_log(TAG, "Dexcom: user logout");

        // Recreate auth screen to show login option
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Delete status screen we came from
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void cgm_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_menu_screen();
            lv_scr_load(screen_menu);
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Single-screen rule: delete cgm_menu after loading auth
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void librelinkup_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "LibreLinkUp button pressed");
        if (lvgl_port_lock(1)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            create_dexcom_status_screen();
            lv_scr_load(screen_dexcom_status);
            // Single-screen rule: delete auth after loading status
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
            }
            create_dexcom_login_screen();
            lv_scr_load(screen_dexcom_login);
            // Single-screen rule: delete auth after loading login
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_username_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_username_entry != NULL) {
                lv_obj_del(screen_dexcom_username_entry);
                screen_dexcom_username_entry = NULL;
            }
            create_dexcom_username_entry_screen();
            lv_scr_load(screen_dexcom_username_entry);
            // Single-screen rule: delete login after loading entry
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
                dexcom_username_label = NULL;
                dexcom_password_label = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_password_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            if (screen_dexcom_password_entry != NULL) {
                lv_obj_del(screen_dexcom_password_entry);
                screen_dexcom_password_entry = NULL;
            }
            create_dexcom_password_entry_screen();
            lv_scr_load(screen_dexcom_password_entry);
            // Single-screen rule: delete login after loading entry
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
                dexcom_username_label = NULL;
                dexcom_password_label = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void dexcom_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!require_wifi_or_prompt()) return;  // push to WiFi setup if offline
        if (dexcom_login_in_progress) {
            ESP_LOGW(TAG, "Dexcom login already in progress");
            return;
        }

        dexcom_login_in_progress = true;
        show_connecting_overlay("Dexcom");

        // Stop existing glucose task — it reads provider type at start,
        // so it must be deleted and recreated after provider switch.
        if (glucose_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting glucose task for provider switch");
            vTaskDelete(glucose_task_handle);
            glucose_task_handle = NULL;
        }

        // Close any existing connections before switching
        if (libre_is_authenticated()) {
            ESP_LOGI(TAG, "Closing Libre connection for provider switch");
            libre_close_persistent_client();
            libre_logout();
        }
        nightscout_close_persistent_client();
        dexcom_close_persistent_client();
        delete_background_tasks_for_ssl("dexcom-login");

        if (xTaskCreate(dexcom_login_task, "dexcom_login", 6144, NULL, 2, NULL) != pdPASS) {
            dexcom_login_in_progress = false;
            hide_connecting_overlay();
            ESP_LOGE(TAG, "Failed to create Dexcom login task");
        }
    }
}

static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Username entry cancelled");
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Username entry completed: %s", dexcom_username_buf);
        }
        // Both CANCEL and READY return to login screen
        if (simulated_code == LV_EVENT_CANCEL || simulated_code == LV_EVENT_READY) {
            if (lvgl_port_lock(1)) {
                // Recreate login (was deleted; labels auto-populate from buffers)
                create_dexcom_login_screen();
                lv_scr_load(screen_dexcom_login);
                if (screen_dexcom_username_entry != NULL) {
                    lv_obj_del(screen_dexcom_username_entry);
                    screen_dexcom_username_entry = NULL;
                }
                lvgl_port_unlock();
            }
        }
    }
}

// Dexcom username entry field event handler (updates display label as user types)
static void dexcom_username_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                strncpy(dexcom_username_buf, text, sizeof(dexcom_username_buf) - 1);
                dexcom_username_buf[sizeof(dexcom_username_buf) - 1] = '\0';

                // Update display label - placeholder disappears when typing starts
                if (display_label != NULL) {
                    if (strlen(text) > 0) {
                        lv_label_set_text(display_label, text);
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "Username");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Password entry cancelled");
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Password entry completed (masked)");
        }
        // Both CANCEL and READY return to login screen
        if (simulated_code == LV_EVENT_CANCEL || simulated_code == LV_EVENT_READY) {
            if (lvgl_port_lock(1)) {
                // Recreate login (was deleted; labels auto-populate from buffers)
                create_dexcom_login_screen();
                lv_scr_load(screen_dexcom_login);
                if (screen_dexcom_password_entry != NULL) {
                    lv_obj_del(screen_dexcom_password_entry);
                    screen_dexcom_password_entry = NULL;
                }
                lvgl_port_unlock();
            }
        }
    }
}

// Eye toggle for the Dexcom password field
static void dexcom_eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dexcom_pwd_visible = !dexcom_pwd_visible;
    if (dexcom_pwd_eye_label) {
        lv_label_set_text(dexcom_pwd_eye_label,
                          dexcom_pwd_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
    if (dexcom_password_entry_label) {
        int len = strlen(dexcom_password_buf);
        if (len > 0) {
            if (dexcom_pwd_visible) {
                lv_label_set_text(dexcom_password_entry_label, dexcom_password_buf);
            } else {
                char display[65];
                for (int i = 0; i < len; i++) display[i] = '*';
                display[len] = '\0';
                lv_label_set_text(dexcom_password_entry_label, display);
            }
            lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        }
    }
}

static void dexcom_password_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                strncpy(dexcom_password_buf, text, sizeof(dexcom_password_buf) - 1);
                dexcom_password_buf[sizeof(dexcom_password_buf) - 1] = '\0';

                if (display_label != NULL) {
                    int len = strlen(text);
                    if (len > 0) {
                        if (dexcom_pwd_visible) {
                            lv_label_set_text(display_label, text);
                        } else {
                            char display[65];
                            // Masked except the last character typed
                            for (int i = 0; i < len - 1; i++) {
                                display[i] = '*';
                            }
                            display[len - 1] = text[len - 1];
                            display[len] = '\0';
                            lv_label_set_text(display_label, display);
                        }
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "Password");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

// Navigate back to dexcom auth screen (used by login and status screens)
static void dexcom_to_auth_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            // Recreate auth screen (was deleted when navigating deeper)
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Delete whichever screen we came from
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
            }
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Every builder below re-entry-guards its own screen global: the inactivity
// watchdog can reclaim a screen mid-flow, so a rebuild must free whatever the
// global still references or orphan it in the LVGL pool. Deleting the *active*
// screen synchronously would panic, hence the async branch.
void create_cgm_menu_screen(void) {
    if (screen_cgm_menu != NULL) {
        if (screen_cgm_menu == lv_scr_act()) {
            lv_obj_del_async(screen_cgm_menu);
        } else {
            lv_obj_del(screen_cgm_menu);
        }
        screen_cgm_menu = NULL;
    }

    screen_cgm_menu = lv_obj_create(NULL);
    lv_obj_add_style(screen_cgm_menu, &style_bg, 0);
    lv_obj_clear_flag(screen_cgm_menu, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *title = lv_label_create(screen_cgm_menu);
    lv_label_set_text(title, "CGM Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_cgm_menu, cgm_back_event_cb, NULL);

    // Dexcom button card
    lv_obj_t *dexcom_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(dexcom_btn, 290, 50);
    lv_obj_align(dexcom_btn, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(dexcom_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dexcom_btn, 10, 0);
    lv_obj_set_style_border_width(dexcom_btn, 0, 0);
    lv_obj_set_style_shadow_width(dexcom_btn, 0, 0);
    lv_obj_set_style_pad_left(dexcom_btn, 14, 0);
    lv_obj_set_style_bg_color(dexcom_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

    lv_obj_t *dexcom_icon = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(dexcom_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(dexcom_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dexcom_label = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_label, "Dexcom Share");
    lv_obj_set_style_text_font(dexcom_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dexcom_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *dexcom_arrow = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(dexcom_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(dexcom_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(dexcom_btn, dexcom_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // LibreLinkUp button card (active)
    lv_obj_t *libre_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(libre_btn, 290, 50);
    lv_obj_align(libre_btn, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(libre_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(libre_btn, 10, 0);
    lv_obj_set_style_border_width(libre_btn, 0, 0);
    lv_obj_set_style_shadow_width(libre_btn, 0, 0);
    lv_obj_set_style_pad_left(libre_btn, 14, 0);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

    lv_obj_t *libre_icon = lv_label_create(libre_btn);
    lv_label_set_text(libre_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(libre_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(libre_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *libre_label = lv_label_create(libre_btn);
    lv_label_set_text(libre_label, "LibreLinkUp");
    lv_obj_set_style_text_font(libre_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(libre_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(libre_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *libre_arrow = lv_label_create(libre_btn);
    lv_label_set_text(libre_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(libre_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(libre_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(libre_btn, librelinkup_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Nightscout button card
    lv_obj_t *nightscout_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(nightscout_btn, 290, 50);
    lv_obj_align(nightscout_btn, LV_ALIGN_TOP_MID, 0, 169);
    lv_obj_set_style_bg_color(nightscout_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(nightscout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nightscout_btn, 10, 0);
    lv_obj_set_style_border_width(nightscout_btn, 0, 0);
    lv_obj_set_style_shadow_width(nightscout_btn, 0, 0);
    lv_obj_set_style_pad_left(nightscout_btn, 14, 0);
    lv_obj_set_style_bg_color(nightscout_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

    lv_obj_t *nightscout_icon = lv_label_create(nightscout_btn);
    lv_label_set_text(nightscout_icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_color(nightscout_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(nightscout_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *nightscout_label = lv_label_create(nightscout_btn);
    lv_label_set_text(nightscout_label, "Nightscout");
    lv_obj_set_style_text_font(nightscout_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(nightscout_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(nightscout_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *nightscout_arrow = lv_label_create(nightscout_btn);
    lv_label_set_text(nightscout_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nightscout_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(nightscout_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(nightscout_btn, nightscout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_auth_screen(void) {
    if (screen_dexcom_auth != NULL) {
        if (screen_dexcom_auth == lv_scr_act()) {
            lv_obj_del_async(screen_dexcom_auth);
        } else {
            lv_obj_del(screen_dexcom_auth);
        }
        screen_dexcom_auth = NULL;
    }

    screen_dexcom_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_auth, &style_bg, 0);
    lv_obj_clear_flag(screen_dexcom_auth, LV_OBJ_FLAG_SCROLLABLE);

    cgm_std_back_btn(screen_dexcom_auth, dexcom_back_event_cb, NULL);

    bool authenticated = dexcom_is_authenticated();

    if (authenticated) {
        // Title
        lv_obj_t *title = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(title, "Dexcom");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Green hero circle with checkmark
        lv_obj_t *hero = lv_obj_create(screen_dexcom_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 52, 52);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_width(hero, 2, 0);
        lv_obj_set_style_border_opa(hero, LV_OPA_COVER, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(hero_icon);

        // "Connected" status
        lv_obj_t *connected = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(connected, "Connected");
        lv_obj_set_style_text_font(connected, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(connected, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(connected, LV_ALIGN_TOP_MID, 0, 112);

        // Username
        lv_obj_t *username = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(username, dexcom_username_buf);
        lv_obj_set_style_text_font(username, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(username, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(username, LV_ALIGN_TOP_MID, 0, 134);

        // Account Details card-button
        lv_obj_t *view_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(view_btn, 290, 44);
        lv_obj_align(view_btn, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(view_btn, 10, 0);
        lv_obj_set_style_border_width(view_btn, 0, 0);
        lv_obj_set_style_shadow_width(view_btn, 0, 0);
        lv_obj_set_style_pad_left(view_btn, 14, 0);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

        lv_obj_t *view_icon = lv_label_create(view_btn);
        lv_label_set_text(view_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(view_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(view_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *view_label = lv_label_create(view_btn);
        lv_label_set_text(view_label, "Account Details");
        lv_obj_set_style_text_font(view_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(view_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(view_label, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *view_arrow = lv_label_create(view_btn);
        lv_label_set_text(view_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(view_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(view_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(view_btn, dexcom_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Hero circle with connection icon
        lv_obj_t *hero = lv_obj_create(screen_dexcom_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 56, 56);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(hero_icon);

        // Brand title
        lv_obj_t *title = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(title, "Dexcom Share");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 88);

        // Subtitle
        lv_obj_t *subtitle = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(subtitle, "Connect your account to view\nreal-time glucose readings");
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 112);

        // Sign In button
        lv_obj_t *login_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(login_btn, 260, 36);
        lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 156);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Sign In");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, dexcom_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        // Footer note
        lv_obj_t *note = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(note, "Requires Dexcom Share enabled");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

void create_dexcom_login_screen(void) {
    if (screen_dexcom_login != NULL) {
        if (screen_dexcom_login == lv_scr_act()) {
            lv_obj_del_async(screen_dexcom_login);
        } else {
            lv_obj_del(screen_dexcom_login);
        }
        screen_dexcom_login = NULL;
    }

    screen_dexcom_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_login, &style_bg, 0);
    lv_obj_clear_flag(screen_dexcom_login, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(title, "Sign In");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_dexcom_login, dexcom_to_auth_event_cb, NULL);

    // Unified form card with username + password rows
    lv_obj_t *form_card = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(form_card, 290, 88);
    lv_obj_align(form_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(form_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(form_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(form_card, 10, 0);
    lv_obj_set_style_border_width(form_card, 0, 0);
    lv_obj_set_style_pad_all(form_card, 0, 0);
    lv_obj_clear_flag(form_card, LV_OBJ_FLAG_SCROLLABLE);

    // Username row (top half, clickable)
    lv_obj_t *user_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(user_row);
    lv_obj_set_size(user_row, 290, 43);
    lv_obj_align(user_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(user_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(user_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(user_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *user_icon = lv_label_create(user_row);
    lv_label_set_text(user_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(user_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(user_icon, LV_ALIGN_LEFT_MID, 14, 0);

    dexcom_username_label = lv_label_create(user_row);
    lv_obj_set_style_text_font(dexcom_username_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(dexcom_username_label, 195);
    lv_label_set_long_mode(dexcom_username_label, LV_LABEL_LONG_DOT);
    if (strlen(dexcom_username_buf) > 0) {
        lv_label_set_text(dexcom_username_label, dexcom_username_buf);
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_username_label, "Username");
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_username_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *user_arrow = lv_label_create(user_row);
    lv_label_set_text(user_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(user_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(user_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(user_row, dexcom_username_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Divider between rows
    lv_obj_t *div = lv_obj_create(form_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 258, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Password row (bottom half, clickable)
    lv_obj_t *pass_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_size(pass_row, 290, 44);
    lv_obj_align(pass_row, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(pass_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pass_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pass_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *pass_icon = lv_label_create(pass_row);
    lv_label_set_text(pass_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(pass_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(pass_icon, LV_ALIGN_LEFT_MID, 14, 0);

    dexcom_password_label = lv_label_create(pass_row);
    lv_obj_set_style_text_font(dexcom_password_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(dexcom_password_label, 195);
    lv_label_set_long_mode(dexcom_password_label, LV_LABEL_LONG_DOT);
    if (strlen(dexcom_password_buf) > 0) {
        int len = strlen(dexcom_password_buf);
        char display[65];
        for (int i = 0; i < len; i++) {
            display[i] = '*';
        }
        display[len] = '\0';
        lv_label_set_text(dexcom_password_label, display);
        lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_password_label, "Password");
        lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_password_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *pass_arrow = lv_label_create(pass_row);
    lv_label_set_text(pass_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(pass_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(pass_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(pass_row, dexcom_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Sign In button
    lv_obj_t *login_btn = lv_btn_create(screen_dexcom_login);
    lv_obj_set_size(login_btn, 260, 36);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_icon = lv_label_create(login_btn);
    lv_label_set_text(login_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(login_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(login_icon, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Sign In");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, dexcom_login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Hint text
    lv_obj_t *hint = lv_label_create(screen_dexcom_login);
    lv_label_set_text(hint, "Tap a field to enter credentials");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void create_dexcom_username_entry_screen(void) {
    if (screen_dexcom_username_entry != NULL) {
        if (screen_dexcom_username_entry == lv_scr_act()) {
            lv_obj_del_async(screen_dexcom_username_entry);
        } else {
            lv_obj_del(screen_dexcom_username_entry);
        }
        screen_dexcom_username_entry = NULL;
    }

    screen_dexcom_username_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_username_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_dexcom_username_entry, LV_OBJ_FLAG_SCROLLABLE);

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_dexcom_username_entry);
    lv_label_set_text(field_title, "USERNAME");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_dexcom_username_entry, dexcom_username_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // OK button (accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_username_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_username_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Input display label (centered between buttons)
    dexcom_username_entry_label = lv_label_create(screen_dexcom_username_entry);
    lv_obj_set_style_text_font(dexcom_username_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dexcom_username_entry_label, 210);
    lv_label_set_long_mode(dexcom_username_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(dexcom_username_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(dexcom_username_buf) > 0) {
        lv_label_set_text(dexcom_username_entry_label, dexcom_username_buf);
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_username_entry_label, "Username");
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_username_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Blue accent underline
    lv_obj_t *accent = lv_obj_create(screen_dexcom_username_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea for keyboard input
    dexcom_username_entry_ta = lv_textarea_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_username_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_username_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_username_entry_ta, true);
    lv_textarea_set_max_length(dexcom_username_entry_ta, 63);
    lv_textarea_set_text(dexcom_username_entry_ta, dexcom_username_buf);

    // Keyboard — glass theme
    dexcom_username_entry_keyboard = lv_keyboard_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_keyboard, 320, 175);
    lv_obj_align(dexcom_username_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_username_entry_keyboard, dexcom_username_entry_ta);
    lv_keyboard_set_mode(dexcom_username_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard background
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dexcom_username_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(dexcom_username_entry_keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(dexcom_username_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(dexcom_username_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(dexcom_username_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(dexcom_username_entry_keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(dexcom_username_entry_ta, dexcom_username_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_username_entry_label);
}

void create_dexcom_password_entry_screen(void) {
    if (screen_dexcom_password_entry != NULL) {
        if (screen_dexcom_password_entry == lv_scr_act()) {
            lv_obj_del_async(screen_dexcom_password_entry);
        } else {
            lv_obj_del(screen_dexcom_password_entry);
        }
        screen_dexcom_password_entry = NULL;
    }

    screen_dexcom_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_password_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_dexcom_password_entry, LV_OBJ_FLAG_SCROLLABLE);

    // Reset password visibility state
    dexcom_pwd_visible = false;

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_dexcom_password_entry);
    lv_label_set_text(field_title, "PASSWORD");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_dexcom_password_entry, dexcom_password_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // Eye toggle button (next to OK)
    lv_obj_t *eye_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(eye_btn, 30, 30);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, -42, 7);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COLOR_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(eye_btn, 0, 0);
    lv_obj_set_style_shadow_width(eye_btn, 0, 0);
    lv_obj_set_style_radius(eye_btn, 8, 0);

    dexcom_pwd_eye_label = lv_label_create(eye_btn);
    lv_label_set_text(dexcom_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(dexcom_pwd_eye_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(dexcom_pwd_eye_label);
    lv_obj_add_event_cb(eye_btn, dexcom_eye_btn_cb, LV_EVENT_CLICKED, NULL);

    // OK button (accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Input display label (centered between buttons)
    dexcom_password_entry_label = lv_label_create(screen_dexcom_password_entry);
    lv_obj_set_style_text_font(dexcom_password_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dexcom_password_entry_label, 210);
    lv_label_set_long_mode(dexcom_password_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(dexcom_password_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    int len = strlen(dexcom_password_buf);
    if (len > 0) {
        char display[65];
        for (int i = 0; i < len; i++) {
            display[i] = '*';
        }
        display[len] = '\0';
        lv_label_set_text(dexcom_password_entry_label, display);
        lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_password_entry_label, "Password");
        lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_password_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Blue accent underline
    lv_obj_t *accent = lv_obj_create(screen_dexcom_password_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea for keyboard input
    dexcom_password_entry_ta = lv_textarea_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_password_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_password_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_password_entry_ta, true);
    lv_textarea_set_password_mode(dexcom_password_entry_ta, false);
    lv_textarea_set_max_length(dexcom_password_entry_ta, 63);
    lv_textarea_set_text(dexcom_password_entry_ta, dexcom_password_buf);

    // Keyboard — glass theme
    dexcom_password_entry_keyboard = lv_keyboard_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_keyboard, 320, 175);
    lv_obj_align(dexcom_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_password_entry_keyboard, dexcom_password_entry_ta);
    lv_keyboard_set_mode(dexcom_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard background
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dexcom_password_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(dexcom_password_entry_keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(dexcom_password_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(dexcom_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(dexcom_password_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(dexcom_password_entry_keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(dexcom_password_entry_ta, dexcom_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_password_entry_label);
}

void create_dexcom_status_screen(void) {
    if (screen_dexcom_status != NULL) {
        if (screen_dexcom_status == lv_scr_act()) {
            lv_obj_del_async(screen_dexcom_status);
        } else {
            lv_obj_del(screen_dexcom_status);
        }
        screen_dexcom_status = NULL;
    }

    screen_dexcom_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_status, &style_bg, 0);
    lv_obj_clear_flag(screen_dexcom_status, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_status);
    lv_label_set_text(title, "Account");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_dexcom_status, dexcom_to_auth_event_cb, NULL);

    // Profile card with avatar, username, and status
    lv_obj_t *profile_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(profile_card, 290, 60);
    lv_obj_align(profile_card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(profile_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(profile_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(profile_card, 10, 0);
    lv_obj_set_style_border_width(profile_card, 0, 0);
    lv_obj_clear_flag(profile_card, LV_OBJ_FLAG_SCROLLABLE);

    // Green accent bar
    lv_obj_t *accent = lv_obj_create(profile_card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 3, 40);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 2, 0);

    // Avatar circle with initial letter
    lv_obj_t *avatar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);

    char initial[2] = {0};
    if (strlen(dexcom_username_buf) > 0) {
        initial[0] = dexcom_username_buf[0];
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    } else {
        initial[0] = 'D';
    }
    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(initial_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(initial_label);

    // Username (truncate with ellipsis if too long)
    lv_obj_t *name_label = lv_label_create(profile_card);
    lv_label_set_text(name_label, dexcom_username_buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 210);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, -8);

    // Connected status
    lv_obj_t *status_label = lv_label_create(profile_card);
    lv_label_set_text(status_label, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 62, 10);

    // Info card (CGM Type + Account)
    lv_obj_t *info_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(info_card, 290, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_set_style_pad_all(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "Dexcom Share");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 20);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *user_title = lv_label_create(info_card);
    lv_label_set_text(user_title, "ACCOUNT");
    lv_obj_set_style_text_font(user_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(user_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(user_title, 2, 0);
    lv_obj_align(user_title, LV_ALIGN_TOP_LEFT, 14, 46);

    lv_obj_t *user_value = lv_label_create(info_card);
    lv_label_set_text(user_value, dexcom_username_buf);
    lv_obj_set_style_text_font(user_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(user_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(user_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(user_value, 260);
    lv_obj_align(user_value, LV_ALIGN_TOP_LEFT, 14, 60);

    // Ghost red logout button
    lv_obj_t *logout_btn = lv_btn_create(screen_dexcom_status);
    lv_obj_set_size(logout_btn, 180, 38);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(logout_btn, 1, 0);
    lv_obj_set_style_border_opa(logout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Sign Out");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, dexcom_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
// LibreLinkUp Screens
// ============================================================================

static volatile bool libre_login_in_progress = false;

// Libre password visibility toggle
static bool libre_pwd_visible = false;
static lv_obj_t *libre_pwd_eye_label = NULL;

// Libre UI widget references (module-local)
static lv_obj_t *libre_email_label = NULL;
static lv_obj_t *libre_password_label = NULL;
static lv_obj_t *libre_email_entry_label = NULL;
static lv_obj_t *libre_email_entry_ta = NULL;
static lv_obj_t *libre_email_entry_keyboard = NULL;
static lv_obj_t *libre_password_entry_label = NULL;
static lv_obj_t *libre_password_entry_ta = NULL;
static lv_obj_t *libre_password_entry_keyboard = NULL;

static void libre_back_event_cb(lv_event_t *e);
static void libre_to_auth_event_cb(lv_event_t *e);
static void libre_open_login_event_cb(lv_event_t *e);
static void libre_view_status_btn_event_cb(lv_event_t *e);
static void libre_email_tap_event_cb(lv_event_t *e);
static void libre_password_tap_event_cb(lv_event_t *e);
static void libre_login_btn_event_cb(lv_event_t *e);
static void libre_email_entry_keyboard_event_cb(lv_event_t *e);
static void libre_email_entry_field_event_cb(lv_event_t *e);
static void libre_password_entry_keyboard_event_cb(lv_event_t *e);
static void libre_password_entry_field_event_cb(lv_event_t *e);
static void libre_logout_btn_event_cb(lv_event_t *e);

// Helper: delete all Libre screens
static void libre_cleanup_screens(void) {
    if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
    if (screen_libre_email_entry != NULL) { lv_obj_del(screen_libre_email_entry); screen_libre_email_entry = NULL; }
    if (screen_libre_password_entry != NULL) { lv_obj_del(screen_libre_password_entry); screen_libre_password_entry = NULL; }
    if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
    if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
}

// Libre login task (runs on separate FreeRTOS task to avoid blocking LVGL)
static void libre_login_task(void *pvParameters) {
    char email[64] = {0};
    char password[64] = {0};
    strncpy(email, libre_email_buf, sizeof(email) - 1);
    strncpy(password, libre_password_buf, sizeof(password) - 1);

    ESP_LOGI(TAG, "Libre login task started for email: %s", email);

    // Free heap for TLS handshake — same pattern as glucose_update_task re-auth
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Libre login: network mutex timeout");
        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
        libre_login_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    bool tasks_deleted = delete_background_tasks_for_ssl("libre-login");

    ESP_LOGI(TAG, "Libre login: heap free=%lu largest=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    if (libre_authenticate(email, password) == ESP_OK) {
        ESP_LOGI(TAG, "Libre login successful");
        sd_log(TAG, "Libre: login OK for %s", email);
        nvs_set_libre_credentials(email, password);
        nvs_save_cgm_type("libre");

        libre_close_persistent_client();  // Close auth connection; glucose task opens fresh

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after Libre login...");
            ensure_tasks_running();  // Canonical stack/prio/core
        }

        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lv_scr_load(screen_home);
            libre_cleanup_screens();
            if (screen_cgm_menu != NULL) { lv_obj_del(screen_cgm_menu); screen_cgm_menu = NULL; }
            home_screen_active = true;
            pause_background_tasks = false;
            show_login_success_overlay_ui();
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGE(TAG, "Libre login failed");
        sd_log(TAG, "Libre: login FAILED for %s", email);

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
    }

    libre_login_in_progress = false;
    vTaskDelete(NULL);
}

// Libre back → CGM menu
static void libre_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_cgm_menu_screen();
            lv_scr_load(screen_cgm_menu);
            libre_cleanup_screens();
            lvgl_port_unlock();
        }
    }
}

// Libre status/login → auth screen
static void libre_to_auth_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Open Libre login screen
static void libre_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// View Libre status
static void libre_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_libre_status_screen();
            lv_scr_load(screen_libre_status);
            if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap email field
static void libre_email_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_libre_email_entry_screen();
            lv_scr_load(screen_libre_email_entry);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap password field
static void libre_password_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_libre_password_entry_screen();
            lv_scr_load(screen_libre_password_entry);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Sign In button
static void libre_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!require_wifi_or_prompt()) return;  // push to WiFi setup if offline
        if (libre_login_in_progress) {
            ESP_LOGW(TAG, "Libre login already in progress");
            return;
        }
        if (strlen(libre_email_buf) == 0 || strlen(libre_password_buf) == 0) {
            ESP_LOGW(TAG, "Email or password is empty");
            return;
        }
        libre_login_in_progress = true;
        show_connecting_overlay("LibreLinkUp");

        // Stop existing glucose task — it reads provider type at start,
        // so it must be deleted and recreated after provider switch.
        if (glucose_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting glucose task for provider switch");
            vTaskDelete(glucose_task_handle);
            glucose_task_handle = NULL;
        }

        // Close any existing connections before switching
        if (dexcom_is_authenticated()) {
            ESP_LOGI(TAG, "Closing Dexcom connection for provider switch");
            dexcom_close_persistent_client();
            dexcom_logout();
        }
        nightscout_close_persistent_client();
        libre_close_persistent_client();
        delete_background_tasks_for_ssl("libre-login");

        if (xTaskCreate(libre_login_task, "libre_login", 6144, NULL, 5, NULL) != pdPASS) {
            libre_login_in_progress = false;
            hide_connecting_overlay();
            ESP_LOGE(TAG, "Failed to create Libre login task");
        }
    }
}

// Email entry keyboard events
static void libre_email_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            // OK — save text and go back to login
            const char *text = lv_textarea_get_text(libre_email_entry_ta);
            strncpy(libre_email_buf, text, sizeof(libre_email_buf) - 1);
            libre_email_buf[sizeof(libre_email_buf) - 1] = '\0';
        }
        // Both OK and Cancel go back to login
        if (lvgl_port_lock(1)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_email_entry != NULL) { lv_obj_del(screen_libre_email_entry); screen_libre_email_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Email entry field value changed
static void libre_email_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);
        const char *text = lv_textarea_get_text(libre_email_entry_ta);
        if (strlen(text) > 0) {
            lv_label_set_text(display_label, text);
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        } else {
            lv_label_set_text(display_label, "Email");
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }
}

// Password entry keyboard events
static void libre_password_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            const char *text = lv_textarea_get_text(libre_password_entry_ta);
            strncpy(libre_password_buf, text, sizeof(libre_password_buf) - 1);
            libre_password_buf[sizeof(libre_password_buf) - 1] = '\0';
        }
        if (lvgl_port_lock(1)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_password_entry != NULL) { lv_obj_del(screen_libre_password_entry); screen_libre_password_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Password entry field value changed (shows last character, masks rest)
static void libre_password_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                strncpy(libre_password_buf, text, sizeof(libre_password_buf) - 1);
                libre_password_buf[sizeof(libre_password_buf) - 1] = '\0';

                if (display_label != NULL) {
                    int len = strlen(text);
                    if (len > 0) {
                        if (libre_pwd_visible) {
                            lv_label_set_text(display_label, text);
                        } else {
                            char display[65];
                            // Masked except the last character typed
                            for (int i = 0; i < len - 1; i++) {
                                display[i] = '*';
                            }
                            display[len - 1] = text[len - 1];
                            display[len] = '\0';
                            lv_label_set_text(display_label, display);
                        }
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "Password");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

// Eye toggle callback for Libre password screen
static void libre_eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    libre_pwd_visible = !libre_pwd_visible;
    if (libre_pwd_eye_label) {
        lv_label_set_text(libre_pwd_eye_label,
                          libre_pwd_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
    if (libre_password_entry_label) {
        int len = strlen(libre_password_buf);
        if (len > 0) {
            if (libre_pwd_visible) {
                lv_label_set_text(libre_password_entry_label, libre_password_buf);
            } else {
                char display[65];
                for (int i = 0; i < len; i++) display[i] = '*';
                display[len] = '\0';
                lv_label_set_text(libre_password_entry_label, display);
            }
            lv_obj_set_style_text_color(libre_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        }
    }
}

// Logout
static void libre_logout_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Libre logout requested");
        libre_logout();
        nvs_clear_libre_credentials();

        // Clear UI buffers
        memset(libre_email_buf, 0, sizeof(libre_email_buf));
        memset(libre_password_buf, 0, sizeof(libre_password_buf));

        if (lvgl_port_lock(1)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// ==================== Libre Auth Screen ====================

void create_libre_auth_screen(void) {
    if (screen_libre_auth != NULL) {
        if (screen_libre_auth == lv_scr_act()) {
            lv_obj_del_async(screen_libre_auth);
        } else {
            lv_obj_del(screen_libre_auth);
        }
        screen_libre_auth = NULL;
    }

    screen_libre_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_auth, &style_bg, 0);
    lv_obj_clear_flag(screen_libre_auth, LV_OBJ_FLAG_SCROLLABLE);

    cgm_std_back_btn(screen_libre_auth, libre_back_event_cb, NULL);

    bool authenticated = libre_is_authenticated();

    if (authenticated) {
        // Title
        lv_obj_t *title = lv_label_create(screen_libre_auth);
        lv_label_set_text(title, "LibreLinkUp");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Green hero circle
        lv_obj_t *hero = lv_obj_create(screen_libre_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 52, 52);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_width(hero, 2, 0);
        lv_obj_set_style_border_opa(hero, LV_OPA_COVER, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(hero_icon);

        // "Connected" status
        lv_obj_t *connected = lv_label_create(screen_libre_auth);
        lv_label_set_text(connected, "Connected");
        lv_obj_set_style_text_font(connected, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(connected, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(connected, LV_ALIGN_TOP_MID, 0, 112);

        // Email
        lv_obj_t *email_lbl = lv_label_create(screen_libre_auth);
        lv_label_set_text(email_lbl, libre_email_buf);
        lv_obj_set_style_text_font(email_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(email_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(email_lbl, LV_ALIGN_TOP_MID, 0, 134);

        // Account Details card-button
        lv_obj_t *view_btn = lv_btn_create(screen_libre_auth);
        lv_obj_set_size(view_btn, 290, 44);
        lv_obj_align(view_btn, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(view_btn, 10, 0);
        lv_obj_set_style_border_width(view_btn, 0, 0);
        lv_obj_set_style_shadow_width(view_btn, 0, 0);
        lv_obj_set_style_pad_left(view_btn, 14, 0);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

        lv_obj_t *view_icon = lv_label_create(view_btn);
        lv_label_set_text(view_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(view_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(view_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *view_label = lv_label_create(view_btn);
        lv_label_set_text(view_label, "Account Details");
        lv_obj_set_style_text_font(view_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(view_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(view_label, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *view_arrow = lv_label_create(view_btn);
        lv_label_set_text(view_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(view_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(view_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(view_btn, libre_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Hero circle
        lv_obj_t *hero = lv_obj_create(screen_libre_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 56, 56);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(hero_icon);

        // Brand title
        lv_obj_t *title = lv_label_create(screen_libre_auth);
        lv_label_set_text(title, "LibreLinkUp");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 88);

        // Subtitle
        lv_obj_t *subtitle = lv_label_create(screen_libre_auth);
        lv_label_set_text(subtitle, "Connect your FreeStyle Libre\naccount for glucose readings");
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 112);

        // Sign In button
        lv_obj_t *login_btn = lv_btn_create(screen_libre_auth);
        lv_obj_set_size(login_btn, 260, 36);
        lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 156);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Sign In");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, libre_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        // Footer note
        lv_obj_t *note = lv_label_create(screen_libre_auth);
        lv_label_set_text(note, "Requires LibreLinkUp follower account");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

// ==================== Libre Login Screen ====================

void create_libre_login_screen(void) {
    if (screen_libre_login != NULL) {
        if (screen_libre_login == lv_scr_act()) {
            lv_obj_del_async(screen_libre_login);
        } else {
            lv_obj_del(screen_libre_login);
        }
        screen_libre_login = NULL;
    }

    screen_libre_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_login, &style_bg, 0);
    lv_obj_clear_flag(screen_libre_login, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_libre_login);
    lv_label_set_text(title, "Sign In");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_libre_login, libre_to_auth_event_cb, NULL);

    // Unified form card
    lv_obj_t *form_card = lv_obj_create(screen_libre_login);
    lv_obj_set_size(form_card, 290, 88);
    lv_obj_align(form_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(form_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(form_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(form_card, 10, 0);
    lv_obj_set_style_border_width(form_card, 0, 0);
    lv_obj_set_style_pad_all(form_card, 0, 0);
    lv_obj_clear_flag(form_card, LV_OBJ_FLAG_SCROLLABLE);

    // Email row (top half)
    lv_obj_t *email_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(email_row);
    lv_obj_set_size(email_row, 290, 43);
    lv_obj_align(email_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(email_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(email_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(email_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *email_icon = lv_label_create(email_row);
    lv_label_set_text(email_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(email_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(email_icon, LV_ALIGN_LEFT_MID, 14, 0);

    libre_email_label = lv_label_create(email_row);
    lv_obj_set_style_text_font(libre_email_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(libre_email_label, 195);
    lv_label_set_long_mode(libre_email_label, LV_LABEL_LONG_DOT);
    if (strlen(libre_email_buf) > 0) {
        lv_label_set_text(libre_email_label, libre_email_buf);
        lv_obj_set_style_text_color(libre_email_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_email_label, "Email");
        lv_obj_set_style_text_color(libre_email_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_email_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *email_arrow = lv_label_create(email_row);
    lv_label_set_text(email_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(email_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(email_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(email_row, libre_email_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Divider
    lv_obj_t *div = lv_obj_create(form_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 258, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Password row (bottom half)
    lv_obj_t *pass_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_size(pass_row, 290, 44);
    lv_obj_align(pass_row, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(pass_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pass_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pass_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *pass_icon = lv_label_create(pass_row);
    lv_label_set_text(pass_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(pass_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(pass_icon, LV_ALIGN_LEFT_MID, 14, 0);

    libre_password_label = lv_label_create(pass_row);
    lv_obj_set_style_text_font(libre_password_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(libre_password_label, 195);
    lv_label_set_long_mode(libre_password_label, LV_LABEL_LONG_DOT);
    if (strlen(libre_password_buf) > 0) {
        int len = strlen(libre_password_buf);
        char display[65];
        for (int i = 0; i < len; i++) display[i] = '*';
        display[len] = '\0';
        lv_label_set_text(libre_password_label, display);
        lv_obj_set_style_text_color(libre_password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_password_label, "Password");
        lv_obj_set_style_text_color(libre_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_password_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *pass_arrow = lv_label_create(pass_row);
    lv_label_set_text(pass_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(pass_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(pass_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(pass_row, libre_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Sign In button
    lv_obj_t *login_btn = lv_btn_create(screen_libre_login);
    lv_obj_set_size(login_btn, 260, 36);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_icon = lv_label_create(login_btn);
    lv_label_set_text(login_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(login_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(login_icon, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Sign In");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, libre_login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Hint text
    lv_obj_t *hint = lv_label_create(screen_libre_login);
    lv_label_set_text(hint, "Tap a field to enter credentials");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ==================== Libre Email Entry Screen ====================

void create_libre_email_entry_screen(void) {
    if (screen_libre_email_entry != NULL) {
        if (screen_libre_email_entry == lv_scr_act()) {
            lv_obj_del_async(screen_libre_email_entry);
        } else {
            lv_obj_del(screen_libre_email_entry);
        }
        screen_libre_email_entry = NULL;
    }

    screen_libre_email_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_email_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_libre_email_entry, LV_OBJ_FLAG_SCROLLABLE);

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_libre_email_entry);
    lv_label_set_text(field_title, "EMAIL");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_libre_email_entry, libre_email_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_libre_email_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, libre_email_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    libre_email_entry_label = lv_label_create(screen_libre_email_entry);
    lv_obj_set_style_text_font(libre_email_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(libre_email_entry_label, 210);
    lv_label_set_long_mode(libre_email_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(libre_email_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(libre_email_buf) > 0) {
        lv_label_set_text(libre_email_entry_label, libre_email_buf);
        lv_obj_set_style_text_color(libre_email_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_email_entry_label, "Email");
        lv_obj_set_style_text_color(libre_email_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_email_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_libre_email_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    libre_email_entry_ta = lv_textarea_create(screen_libre_email_entry);
    lv_obj_set_size(libre_email_entry_ta, 1, 1);
    lv_obj_set_pos(libre_email_entry_ta, -100, -100);
    lv_obj_add_flag(libre_email_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(libre_email_entry_ta, true);
    lv_textarea_set_max_length(libre_email_entry_ta, 63);
    lv_textarea_set_text(libre_email_entry_ta, libre_email_buf);

    // Keyboard
    libre_email_entry_keyboard = lv_keyboard_create(screen_libre_email_entry);
    lv_obj_set_size(libre_email_entry_keyboard, 320, 175);
    lv_obj_align(libre_email_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(libre_email_entry_keyboard, libre_email_entry_ta);
    lv_keyboard_set_mode(libre_email_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(libre_email_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(libre_email_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(libre_email_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(libre_email_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(libre_email_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(libre_email_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(libre_email_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(libre_email_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(libre_email_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(libre_email_entry_ta, libre_email_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, libre_email_entry_label);
}

// ==================== Libre Password Entry Screen ====================

void create_libre_password_entry_screen(void) {
    if (screen_libre_password_entry != NULL) {
        if (screen_libre_password_entry == lv_scr_act()) {
            lv_obj_del_async(screen_libre_password_entry);
        } else {
            lv_obj_del(screen_libre_password_entry);
        }
        screen_libre_password_entry = NULL;
    }

    screen_libre_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_password_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_libre_password_entry, LV_OBJ_FLAG_SCROLLABLE);

    libre_pwd_visible = false;

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_libre_password_entry);
    lv_label_set_text(field_title, "PASSWORD");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_libre_password_entry, libre_password_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // Eye toggle button (next to OK)
    lv_obj_t *eye_btn = lv_btn_create(screen_libre_password_entry);
    lv_obj_set_size(eye_btn, 30, 30);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, -42, 7);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COLOR_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(eye_btn, 0, 0);
    lv_obj_set_style_shadow_width(eye_btn, 0, 0);
    lv_obj_set_style_radius(eye_btn, 8, 0);

    libre_pwd_eye_label = lv_label_create(eye_btn);
    lv_label_set_text(libre_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(libre_pwd_eye_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(libre_pwd_eye_label);
    lv_obj_add_event_cb(eye_btn, libre_eye_btn_cb, LV_EVENT_CLICKED, NULL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_libre_password_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, libre_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    libre_password_entry_label = lv_label_create(screen_libre_password_entry);
    lv_obj_set_style_text_font(libre_password_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(libre_password_entry_label, 210);
    lv_label_set_long_mode(libre_password_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(libre_password_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(libre_password_buf) > 0) {
        int len = strlen(libre_password_buf);
        char masked[65];
        for (int i = 0; i < len && i < 64; i++) masked[i] = '*';
        masked[len < 64 ? len : 64] = '\0';
        lv_label_set_text(libre_password_entry_label, masked);
        lv_obj_set_style_text_color(libre_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_password_entry_label, "Password");
        lv_obj_set_style_text_color(libre_password_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_password_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_libre_password_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    libre_password_entry_ta = lv_textarea_create(screen_libre_password_entry);
    lv_obj_set_size(libre_password_entry_ta, 1, 1);
    lv_obj_set_pos(libre_password_entry_ta, -100, -100);
    lv_obj_add_flag(libre_password_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(libre_password_entry_ta, true);
    lv_textarea_set_max_length(libre_password_entry_ta, 63);
    lv_textarea_set_text(libre_password_entry_ta, libre_password_buf);

    // Keyboard
    libre_password_entry_keyboard = lv_keyboard_create(screen_libre_password_entry);
    lv_obj_set_size(libre_password_entry_keyboard, 320, 175);
    lv_obj_align(libre_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(libre_password_entry_keyboard, libre_password_entry_ta);
    lv_keyboard_set_mode(libre_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(libre_password_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(libre_password_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(libre_password_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(libre_password_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(libre_password_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(libre_password_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(libre_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(libre_password_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(libre_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(libre_password_entry_ta, libre_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, libre_password_entry_label);
}

// ==================== Libre Status Screen ====================

void create_libre_status_screen(void) {
    if (screen_libre_status != NULL) {
        if (screen_libre_status == lv_scr_act()) {
            lv_obj_del_async(screen_libre_status);
        } else {
            lv_obj_del(screen_libre_status);
        }
        screen_libre_status = NULL;
    }

    screen_libre_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_status, &style_bg, 0);
    lv_obj_clear_flag(screen_libre_status, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_libre_status);
    lv_label_set_text(title, "Account");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_libre_status, libre_to_auth_event_cb, NULL);

    // Profile card
    lv_obj_t *profile_card = lv_obj_create(screen_libre_status);
    lv_obj_set_size(profile_card, 290, 60);
    lv_obj_align(profile_card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(profile_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(profile_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(profile_card, 10, 0);
    lv_obj_set_style_border_width(profile_card, 0, 0);
    lv_obj_clear_flag(profile_card, LV_OBJ_FLAG_SCROLLABLE);

    // Green accent bar
    lv_obj_t *accent_bar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(accent_bar);
    lv_obj_set_size(accent_bar, 3, 40);
    lv_obj_align(accent_bar, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent_bar, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent_bar, 2, 0);

    // Avatar circle
    lv_obj_t *avatar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);

    char initial[2] = {0};
    if (strlen(libre_email_buf) > 0) {
        initial[0] = libre_email_buf[0];
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    } else {
        initial[0] = 'L';
    }
    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(initial_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(initial_label);

    // Email
    lv_obj_t *name_label = lv_label_create(profile_card);
    lv_label_set_text(name_label, libre_email_buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 210);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, -8);

    // Connected status
    lv_obj_t *status_label = lv_label_create(profile_card);
    lv_label_set_text(status_label, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 62, 10);

    // Info card (CGM Type + Account)
    lv_obj_t *info_card = lv_obj_create(screen_libre_status);
    lv_obj_set_size(info_card, 290, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_set_style_pad_all(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "LibreLinkUp");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 20);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *acct_title = lv_label_create(info_card);
    lv_label_set_text(acct_title, "ACCOUNT");
    lv_obj_set_style_text_font(acct_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(acct_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(acct_title, 2, 0);
    lv_obj_align(acct_title, LV_ALIGN_TOP_LEFT, 14, 46);

    lv_obj_t *acct_value = lv_label_create(info_card);
    lv_label_set_text(acct_value, libre_email_buf);
    lv_obj_set_style_text_font(acct_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(acct_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(acct_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(acct_value, 260);
    lv_obj_align(acct_value, LV_ALIGN_TOP_LEFT, 14, 60);

    // Ghost red logout button
    lv_obj_t *logout_btn = lv_btn_create(screen_libre_status);
    lv_obj_set_size(logout_btn, 180, 38);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(logout_btn, 1, 0);
    lv_obj_set_style_border_opa(logout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Sign Out");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, libre_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
// Nightscout Screens
// ============================================================================

static volatile bool nightscout_login_in_progress = false;

// Nightscout token visibility toggle
static bool nightscout_token_visible = false;
static lv_obj_t *nightscout_token_eye_label = NULL;

// Nightscout UI widget references (module-local)
static lv_obj_t *nightscout_url_label = NULL;
static lv_obj_t *nightscout_token_label = NULL;
static lv_obj_t *nightscout_url_entry_label = NULL;
static lv_obj_t *nightscout_url_entry_ta = NULL;
static lv_obj_t *nightscout_url_entry_keyboard = NULL;
static lv_obj_t *nightscout_token_entry_label = NULL;
static lv_obj_t *nightscout_token_entry_ta = NULL;
static lv_obj_t *nightscout_token_entry_keyboard = NULL;

static void nightscout_back_event_cb(lv_event_t *e);
static void nightscout_to_auth_event_cb(lv_event_t *e);
static void nightscout_open_login_event_cb(lv_event_t *e);
static void nightscout_view_status_btn_event_cb(lv_event_t *e);
static void nightscout_url_tap_event_cb(lv_event_t *e);
static void nightscout_token_tap_event_cb(lv_event_t *e);
static void nightscout_login_btn_event_cb(lv_event_t *e);
static void nightscout_url_entry_keyboard_event_cb(lv_event_t *e);
static void nightscout_url_entry_field_event_cb(lv_event_t *e);
static void nightscout_token_entry_keyboard_event_cb(lv_event_t *e);
static void nightscout_token_entry_field_event_cb(lv_event_t *e);
static void nightscout_logout_btn_event_cb(lv_event_t *e);

// Helper: delete all Nightscout screens
static void nightscout_cleanup_screens(void) {
    if (screen_nightscout_login != NULL) { lv_obj_del(screen_nightscout_login); screen_nightscout_login = NULL; }
    if (screen_nightscout_url_entry != NULL) { lv_obj_del(screen_nightscout_url_entry); screen_nightscout_url_entry = NULL; }
    if (screen_nightscout_token_entry != NULL) { lv_obj_del(screen_nightscout_token_entry); screen_nightscout_token_entry = NULL; }
    if (screen_nightscout_status != NULL) { lv_obj_del(screen_nightscout_status); screen_nightscout_status = NULL; }
    if (screen_nightscout_auth != NULL) { lv_obj_del(screen_nightscout_auth); screen_nightscout_auth = NULL; }
}

// Nightscout login task (runs on separate FreeRTOS task to avoid blocking LVGL)
static void nightscout_login_task(void *pvParameters) {
    char url[128] = {0};
    char token[64] = {0};
    strncpy(url, nightscout_url_buf, sizeof(url) - 1);
    strncpy(token, nightscout_token_buf, sizeof(token) - 1);

    ESP_LOGI(TAG, "Nightscout login task started for URL: %s", url);

    // Free heap for TLS handshake — same pattern as glucose_update_task re-auth
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Nightscout login: network mutex timeout");
        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
        nightscout_login_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // HTTPS needs the large TLS buffer, so free the background tasks first.
    // nightscout_authenticate() defaults a scheme-less URL to https://, so only
    // an explicit http:// URL skips the deletion.
    bool tasks_deleted = false;
    bool needs_tls = (strncmp(url, "http://", 7) != 0);  // no scheme or https = TLS
    if (needs_tls) {
        tasks_deleted = delete_background_tasks_for_ssl("nightscout-login");
    }

    ESP_LOGI(TAG, "Nightscout login: heap free=%lu largest=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    if (nightscout_authenticate(url, token) == ESP_OK) {
        ESP_LOGI(TAG, "Nightscout login successful");
        sd_log(TAG, "Nightscout: login OK for %s", url);
        nvs_set_nightscout_credentials(url, token);
        nvs_save_cgm_type("nightscout");

        nightscout_close_persistent_client();  // Close auth connection; glucose task opens fresh

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after Nightscout login...");
            ensure_tasks_running();  // Canonical stack/prio/core
        }

        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lv_scr_load(screen_home);
            nightscout_cleanup_screens();
            if (screen_cgm_menu != NULL) { lv_obj_del(screen_cgm_menu); screen_cgm_menu = NULL; }
            home_screen_active = true;
            pause_background_tasks = false;
            show_login_success_overlay_ui();
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGE(TAG, "Nightscout login failed");
        sd_log(TAG, "Nightscout: login FAILED for %s", url);

        if (tasks_deleted) {
            recreate_background_tasks();
        }
        xSemaphoreGive(network_mutex);

        if (lvgl_port_lock(1)) {
            hide_connecting_overlay();
            lvgl_port_unlock();
        }
    }

    nightscout_login_in_progress = false;
    vTaskDelete(NULL);
}

// Nightscout back -> CGM menu
static void nightscout_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_cgm_menu_screen();
            lv_scr_load(screen_cgm_menu);
            nightscout_cleanup_screens();
            lvgl_port_unlock();
        }
    }
}

// Nightscout status/login -> auth screen
static void nightscout_to_auth_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_nightscout_auth_screen();
            lv_scr_load(screen_nightscout_auth);
            if (screen_nightscout_login != NULL) { lv_obj_del(screen_nightscout_login); screen_nightscout_login = NULL; }
            if (screen_nightscout_status != NULL) { lv_obj_del(screen_nightscout_status); screen_nightscout_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Nightscout button event handler (from CGM menu)
static void nightscout_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Nightscout button pressed");
        if (lvgl_port_lock(1)) {
            create_nightscout_auth_screen();
            lv_scr_load(screen_nightscout_auth);
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Open Nightscout login screen
static void nightscout_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_nightscout_login_screen();
            lv_scr_load(screen_nightscout_login);
            if (screen_nightscout_auth != NULL) { lv_obj_del(screen_nightscout_auth); screen_nightscout_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// View Nightscout status
static void nightscout_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_nightscout_status_screen();
            lv_scr_load(screen_nightscout_status);
            if (screen_nightscout_auth != NULL) { lv_obj_del(screen_nightscout_auth); screen_nightscout_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap URL field
static void nightscout_url_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_nightscout_url_entry_screen();
            lv_scr_load(screen_nightscout_url_entry);
            if (screen_nightscout_login != NULL) { lv_obj_del(screen_nightscout_login); screen_nightscout_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap token field
static void nightscout_token_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(1)) {
            create_nightscout_token_entry_screen();
            lv_scr_load(screen_nightscout_token_entry);
            if (screen_nightscout_login != NULL) { lv_obj_del(screen_nightscout_login); screen_nightscout_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Sign In button
static void nightscout_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!require_wifi_or_prompt()) return;  // push to WiFi setup if offline
        if (nightscout_login_in_progress) {
            ESP_LOGW(TAG, "Nightscout login already in progress");
            return;
        }
        if (strlen(nightscout_url_buf) == 0) {
            ESP_LOGW(TAG, "Nightscout URL is empty");
            return;
        }
        nightscout_login_in_progress = true;
        show_connecting_overlay("Nightscout");

        // Stop existing glucose task — it reads provider type at start,
        // so it must be deleted and recreated after provider switch.
        if (glucose_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting glucose task for provider switch");
            vTaskDelete(glucose_task_handle);
            glucose_task_handle = NULL;
        }

        // Close all connections before switching
        if (dexcom_is_authenticated()) {
            dexcom_close_persistent_client();
            dexcom_logout();
        }
        if (libre_is_authenticated()) {
            libre_close_persistent_client();
            libre_logout();
        }
        nightscout_close_persistent_client();
        delete_background_tasks_for_ssl("nightscout-login");

        if (xTaskCreate(nightscout_login_task, "ns_login", 6144, NULL, 5, NULL) != pdPASS) {
            nightscout_login_in_progress = false;
            hide_connecting_overlay();
            ESP_LOGE(TAG, "Failed to create Nightscout login task");
        }
    }
}

// URL entry keyboard events
static void nightscout_url_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            // OK — save text and go back to login
            const char *text = lv_textarea_get_text(nightscout_url_entry_ta);
            strncpy(nightscout_url_buf, text, sizeof(nightscout_url_buf) - 1);
            nightscout_url_buf[sizeof(nightscout_url_buf) - 1] = '\0';
        }
        // Both OK and Cancel go back to login
        if (lvgl_port_lock(1)) {
            create_nightscout_login_screen();
            lv_scr_load(screen_nightscout_login);
            if (screen_nightscout_url_entry != NULL) { lv_obj_del(screen_nightscout_url_entry); screen_nightscout_url_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// URL entry field value changed
static void nightscout_url_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);
        const char *text = lv_textarea_get_text(nightscout_url_entry_ta);
        if (strlen(text) > 0) {
            lv_label_set_text(display_label, text);
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        } else {
            lv_label_set_text(display_label, "Server URL");
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }
}

// Token entry keyboard events
static void nightscout_token_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            const char *text = lv_textarea_get_text(nightscout_token_entry_ta);
            strncpy(nightscout_token_buf, text, sizeof(nightscout_token_buf) - 1);
            nightscout_token_buf[sizeof(nightscout_token_buf) - 1] = '\0';
        }
        if (lvgl_port_lock(1)) {
            create_nightscout_login_screen();
            lv_scr_load(screen_nightscout_login);
            if (screen_nightscout_token_entry != NULL) { lv_obj_del(screen_nightscout_token_entry); screen_nightscout_token_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Token entry field value changed (shows last character, masks rest)
static void nightscout_token_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                strncpy(nightscout_token_buf, text, sizeof(nightscout_token_buf) - 1);
                nightscout_token_buf[sizeof(nightscout_token_buf) - 1] = '\0';

                if (display_label != NULL) {
                    int len = strlen(text);
                    if (len > 0) {
                        if (nightscout_token_visible) {
                            lv_label_set_text(display_label, text);
                        } else {
                            char display[65];
                            // Masked except the last character typed
                            for (int i = 0; i < len - 1; i++) {
                                display[i] = '*';
                            }
                            display[len - 1] = text[len - 1];
                            display[len] = '\0';
                            lv_label_set_text(display_label, display);
                        }
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "API Token (optional)");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

// Eye toggle callback for Nightscout token screen
static void nightscout_eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nightscout_token_visible = !nightscout_token_visible;
    if (nightscout_token_eye_label) {
        lv_label_set_text(nightscout_token_eye_label,
                          nightscout_token_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
    if (nightscout_token_entry_label) {
        int len = strlen(nightscout_token_buf);
        if (len > 0) {
            if (nightscout_token_visible) {
                lv_label_set_text(nightscout_token_entry_label, nightscout_token_buf);
            } else {
                char display[65];
                for (int i = 0; i < len; i++) display[i] = '*';
                display[len] = '\0';
                lv_label_set_text(nightscout_token_entry_label, display);
            }
            lv_obj_set_style_text_color(nightscout_token_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        }
    }
}

// Logout
static void nightscout_logout_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Nightscout logout requested");
        nightscout_logout();
        nvs_clear_nightscout_credentials();

        // Clear UI buffers
        memset(nightscout_url_buf, 0, sizeof(nightscout_url_buf));
        memset(nightscout_token_buf, 0, sizeof(nightscout_token_buf));

        if (lvgl_port_lock(1)) {
            create_nightscout_auth_screen();
            lv_scr_load(screen_nightscout_auth);
            if (screen_nightscout_status != NULL) { lv_obj_del(screen_nightscout_status); screen_nightscout_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// ==================== Nightscout Auth Screen ====================

void create_nightscout_auth_screen(void) {
    if (screen_nightscout_auth != NULL) {
        if (screen_nightscout_auth == lv_scr_act()) {
            lv_obj_del_async(screen_nightscout_auth);
        } else {
            lv_obj_del(screen_nightscout_auth);
        }
        screen_nightscout_auth = NULL;
    }

    screen_nightscout_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_nightscout_auth, &style_bg, 0);
    lv_obj_clear_flag(screen_nightscout_auth, LV_OBJ_FLAG_SCROLLABLE);

    cgm_std_back_btn(screen_nightscout_auth, nightscout_back_event_cb, NULL);

    bool authenticated = nightscout_is_authenticated();

    if (authenticated) {
        // Title
        lv_obj_t *title = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(title, "Nightscout");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Green hero circle
        lv_obj_t *hero = lv_obj_create(screen_nightscout_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 52, 52);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_width(hero, 2, 0);
        lv_obj_set_style_border_opa(hero, LV_OPA_COVER, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(hero_icon);

        // "Connected" status
        lv_obj_t *connected = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(connected, "Connected");
        lv_obj_set_style_text_font(connected, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(connected, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(connected, LV_ALIGN_TOP_MID, 0, 112);

        // URL
        lv_obj_t *url_lbl = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(url_lbl, nightscout_url_buf);
        lv_obj_set_style_text_font(url_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(url_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_label_set_long_mode(url_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(url_lbl, 280);
        lv_obj_set_style_text_align(url_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(url_lbl, LV_ALIGN_TOP_MID, 0, 134);

        // Account Details card-button
        lv_obj_t *view_btn = lv_btn_create(screen_nightscout_auth);
        lv_obj_set_size(view_btn, 290, 44);
        lv_obj_align(view_btn, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(view_btn, 10, 0);
        lv_obj_set_style_border_width(view_btn, 0, 0);
        lv_obj_set_style_shadow_width(view_btn, 0, 0);
        lv_obj_set_style_pad_left(view_btn, 14, 0);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

        lv_obj_t *view_icon = lv_label_create(view_btn);
        lv_label_set_text(view_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(view_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(view_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *view_label = lv_label_create(view_btn);
        lv_label_set_text(view_label, "Account Details");
        lv_obj_set_style_text_font(view_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(view_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(view_label, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *view_arrow = lv_label_create(view_btn);
        lv_label_set_text(view_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(view_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(view_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(view_btn, nightscout_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Hero circle
        lv_obj_t *hero = lv_obj_create(screen_nightscout_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 56, 56);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(hero_icon);

        // Brand title
        lv_obj_t *title = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(title, "Nightscout");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 88);

        // Subtitle
        lv_obj_t *subtitle = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(subtitle, "Connect your Nightscout server\nfor glucose readings");
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 112);

        // Sign In button
        lv_obj_t *login_btn = lv_btn_create(screen_nightscout_auth);
        lv_obj_set_size(login_btn, 260, 36);
        lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 156);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Sign In");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, nightscout_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        // Footer note
        lv_obj_t *note = lv_label_create(screen_nightscout_auth);
        lv_label_set_text(note, "Requires Nightscout instance with API access");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

// ==================== Nightscout Login Screen ====================

void create_nightscout_login_screen(void) {
    if (screen_nightscout_login != NULL) {
        if (screen_nightscout_login == lv_scr_act()) {
            lv_obj_del_async(screen_nightscout_login);
        } else {
            lv_obj_del(screen_nightscout_login);
        }
        screen_nightscout_login = NULL;
    }

    screen_nightscout_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_nightscout_login, &style_bg, 0);
    lv_obj_clear_flag(screen_nightscout_login, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_nightscout_login);
    lv_label_set_text(title, "Sign In");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_nightscout_login, nightscout_to_auth_event_cb, NULL);

    // Unified form card
    lv_obj_t *form_card = lv_obj_create(screen_nightscout_login);
    lv_obj_set_size(form_card, 290, 88);
    lv_obj_align(form_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(form_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(form_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(form_card, 10, 0);
    lv_obj_set_style_border_width(form_card, 0, 0);
    lv_obj_set_style_pad_all(form_card, 0, 0);
    lv_obj_clear_flag(form_card, LV_OBJ_FLAG_SCROLLABLE);

    // URL row (top half)
    lv_obj_t *url_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(url_row);
    lv_obj_set_size(url_row, 290, 43);
    lv_obj_align(url_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(url_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(url_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(url_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *url_icon = lv_label_create(url_row);
    lv_label_set_text(url_icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_color(url_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(url_icon, LV_ALIGN_LEFT_MID, 14, 0);

    nightscout_url_label = lv_label_create(url_row);
    lv_obj_set_style_text_font(nightscout_url_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nightscout_url_label, 195);
    lv_label_set_long_mode(nightscout_url_label, LV_LABEL_LONG_DOT);
    if (strlen(nightscout_url_buf) > 0) {
        lv_label_set_text(nightscout_url_label, nightscout_url_buf);
        lv_obj_set_style_text_color(nightscout_url_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(nightscout_url_label, "Server URL");
        lv_obj_set_style_text_color(nightscout_url_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(nightscout_url_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *url_arrow = lv_label_create(url_row);
    lv_label_set_text(url_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(url_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(url_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(url_row, nightscout_url_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Divider
    lv_obj_t *div = lv_obj_create(form_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 258, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Token row (bottom half)
    lv_obj_t *token_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(token_row);
    lv_obj_set_size(token_row, 290, 44);
    lv_obj_align(token_row, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(token_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(token_row, lv_color_hex(COLOR_INPUT_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(token_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *token_icon = lv_label_create(token_row);
    lv_label_set_text(token_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(token_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(token_icon, LV_ALIGN_LEFT_MID, 14, 0);

    nightscout_token_label = lv_label_create(token_row);
    lv_obj_set_style_text_font(nightscout_token_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nightscout_token_label, 195);
    lv_label_set_long_mode(nightscout_token_label, LV_LABEL_LONG_DOT);
    if (strlen(nightscout_token_buf) > 0) {
        int len = strlen(nightscout_token_buf);
        char display[65];
        for (int i = 0; i < len; i++) display[i] = '*';
        display[len] = '\0';
        lv_label_set_text(nightscout_token_label, display);
        lv_obj_set_style_text_color(nightscout_token_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(nightscout_token_label, "API Token (optional)");
        lv_obj_set_style_text_color(nightscout_token_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(nightscout_token_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *token_arrow = lv_label_create(token_row);
    lv_label_set_text(token_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(token_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(token_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(token_row, nightscout_token_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Sign In button
    lv_obj_t *login_btn = lv_btn_create(screen_nightscout_login);
    lv_obj_set_size(login_btn, 260, 36);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(login_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_icon = lv_label_create(login_btn);
    lv_label_set_text(login_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(login_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(login_icon, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Sign In");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, nightscout_login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Hint text
    lv_obj_t *hint = lv_label_create(screen_nightscout_login);
    lv_label_set_text(hint, "Tap a field to enter credentials");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ==================== Nightscout URL Entry Screen ====================

void create_nightscout_url_entry_screen(void) {
    if (screen_nightscout_url_entry != NULL) {
        if (screen_nightscout_url_entry == lv_scr_act()) {
            lv_obj_del_async(screen_nightscout_url_entry);
        } else {
            lv_obj_del(screen_nightscout_url_entry);
        }
        screen_nightscout_url_entry = NULL;
    }

    screen_nightscout_url_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_nightscout_url_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_nightscout_url_entry, LV_OBJ_FLAG_SCROLLABLE);

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_nightscout_url_entry);
    lv_label_set_text(field_title, "SERVER URL");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_nightscout_url_entry, nightscout_url_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_nightscout_url_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, nightscout_url_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    nightscout_url_entry_label = lv_label_create(screen_nightscout_url_entry);
    lv_obj_set_style_text_font(nightscout_url_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(nightscout_url_entry_label, 210);
    lv_label_set_long_mode(nightscout_url_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(nightscout_url_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(nightscout_url_buf) > 0) {
        lv_label_set_text(nightscout_url_entry_label, nightscout_url_buf);
        lv_obj_set_style_text_color(nightscout_url_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(nightscout_url_entry_label, "Server URL");
        lv_obj_set_style_text_color(nightscout_url_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(nightscout_url_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_nightscout_url_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    nightscout_url_entry_ta = lv_textarea_create(screen_nightscout_url_entry);
    lv_obj_set_size(nightscout_url_entry_ta, 1, 1);
    lv_obj_set_pos(nightscout_url_entry_ta, -100, -100);
    lv_obj_add_flag(nightscout_url_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(nightscout_url_entry_ta, true);
    lv_textarea_set_max_length(nightscout_url_entry_ta, 127);
    lv_textarea_set_text(nightscout_url_entry_ta, nightscout_url_buf);

    // Keyboard
    nightscout_url_entry_keyboard = lv_keyboard_create(screen_nightscout_url_entry);
    lv_obj_set_size(nightscout_url_entry_keyboard, 320, 175);
    lv_obj_align(nightscout_url_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(nightscout_url_entry_keyboard, nightscout_url_entry_ta);
    lv_keyboard_set_mode(nightscout_url_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(nightscout_url_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(nightscout_url_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nightscout_url_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(nightscout_url_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(nightscout_url_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(nightscout_url_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(nightscout_url_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(nightscout_url_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(nightscout_url_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(nightscout_url_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(nightscout_url_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(nightscout_url_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(nightscout_url_entry_ta, nightscout_url_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, nightscout_url_entry_label);
}

// ==================== Nightscout Token Entry Screen ====================

void create_nightscout_token_entry_screen(void) {
    if (screen_nightscout_token_entry != NULL) {
        if (screen_nightscout_token_entry == lv_scr_act()) {
            lv_obj_del_async(screen_nightscout_token_entry);
        } else {
            lv_obj_del(screen_nightscout_token_entry);
        }
        screen_nightscout_token_entry = NULL;
    }

    screen_nightscout_token_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_nightscout_token_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_nightscout_token_entry, LV_OBJ_FLAG_SCROLLABLE);

    nightscout_token_visible = false;

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_nightscout_token_entry);
    lv_label_set_text(field_title, "API TOKEN");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    cgm_std_back_btn(screen_nightscout_token_entry, nightscout_token_entry_keyboard_event_cb, (void*)LV_EVENT_CANCEL);

    // Eye toggle button (next to OK)
    lv_obj_t *eye_btn = lv_btn_create(screen_nightscout_token_entry);
    lv_obj_set_size(eye_btn, 30, 30);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, -42, 7);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COLOR_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(eye_btn, 0, 0);
    lv_obj_set_style_shadow_width(eye_btn, 0, 0);
    lv_obj_set_style_radius(eye_btn, 8, 0);

    nightscout_token_eye_label = lv_label_create(eye_btn);
    lv_label_set_text(nightscout_token_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(nightscout_token_eye_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(nightscout_token_eye_label);
    lv_obj_add_event_cb(eye_btn, nightscout_eye_btn_cb, LV_EVENT_CLICKED, NULL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_nightscout_token_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, nightscout_token_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    nightscout_token_entry_label = lv_label_create(screen_nightscout_token_entry);
    lv_obj_set_style_text_font(nightscout_token_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(nightscout_token_entry_label, 210);
    lv_label_set_long_mode(nightscout_token_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(nightscout_token_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(nightscout_token_buf) > 0) {
        int len = strlen(nightscout_token_buf);
        char masked[65];
        for (int i = 0; i < len && i < 64; i++) masked[i] = '*';
        masked[len < 64 ? len : 64] = '\0';
        lv_label_set_text(nightscout_token_entry_label, masked);
        lv_obj_set_style_text_color(nightscout_token_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(nightscout_token_entry_label, "API Token (optional)");
        lv_obj_set_style_text_color(nightscout_token_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(nightscout_token_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_nightscout_token_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    nightscout_token_entry_ta = lv_textarea_create(screen_nightscout_token_entry);
    lv_obj_set_size(nightscout_token_entry_ta, 1, 1);
    lv_obj_set_pos(nightscout_token_entry_ta, -100, -100);
    lv_obj_add_flag(nightscout_token_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(nightscout_token_entry_ta, true);
    lv_textarea_set_max_length(nightscout_token_entry_ta, 63);
    lv_textarea_set_text(nightscout_token_entry_ta, nightscout_token_buf);

    // Keyboard
    nightscout_token_entry_keyboard = lv_keyboard_create(screen_nightscout_token_entry);
    lv_obj_set_size(nightscout_token_entry_keyboard, 320, 175);
    lv_obj_align(nightscout_token_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(nightscout_token_entry_keyboard, nightscout_token_entry_ta);
    lv_keyboard_set_mode(nightscout_token_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(nightscout_token_entry_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(nightscout_token_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nightscout_token_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(nightscout_token_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(nightscout_token_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(nightscout_token_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(nightscout_token_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(nightscout_token_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(nightscout_token_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(nightscout_token_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(nightscout_token_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(nightscout_token_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(nightscout_token_entry_ta, nightscout_token_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, nightscout_token_entry_label);
}

// ==================== Nightscout Status Screen ====================

void create_nightscout_status_screen(void) {
    if (screen_nightscout_status != NULL) {
        if (screen_nightscout_status == lv_scr_act()) {
            lv_obj_del_async(screen_nightscout_status);
        } else {
            lv_obj_del(screen_nightscout_status);
        }
        screen_nightscout_status = NULL;
    }

    screen_nightscout_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_nightscout_status, &style_bg, 0);
    lv_obj_clear_flag(screen_nightscout_status, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(screen_nightscout_status);
    lv_label_set_text(title, "Account");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    cgm_std_back_btn(screen_nightscout_status, nightscout_to_auth_event_cb, NULL);

    // Profile card
    lv_obj_t *profile_card = lv_obj_create(screen_nightscout_status);
    lv_obj_set_size(profile_card, 290, 60);
    lv_obj_align(profile_card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(profile_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(profile_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(profile_card, 10, 0);
    lv_obj_set_style_border_width(profile_card, 0, 0);
    lv_obj_clear_flag(profile_card, LV_OBJ_FLAG_SCROLLABLE);

    // Green accent bar
    lv_obj_t *accent_bar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(accent_bar);
    lv_obj_set_size(accent_bar, 3, 40);
    lv_obj_align(accent_bar, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent_bar, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent_bar, 2, 0);

    // Avatar circle
    lv_obj_t *avatar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, "N");
    lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(initial_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(initial_label);

    // URL
    lv_obj_t *name_label = lv_label_create(profile_card);
    lv_label_set_text(name_label, nightscout_url_buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 210);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, -8);

    // Connected status
    lv_obj_t *status_label = lv_label_create(profile_card);
    lv_label_set_text(status_label, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 62, 10);

    // Info card (CGM Type + URL)
    lv_obj_t *info_card = lv_obj_create(screen_nightscout_status);
    lv_obj_set_size(info_card, 290, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_set_style_pad_all(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "Nightscout");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 20);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *url_title = lv_label_create(info_card);
    lv_label_set_text(url_title, "URL");
    lv_obj_set_style_text_font(url_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(url_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(url_title, 2, 0);
    lv_obj_align(url_title, LV_ALIGN_TOP_LEFT, 14, 46);

    lv_obj_t *url_value = lv_label_create(info_card);
    lv_label_set_text(url_value, nightscout_url_buf);
    lv_obj_set_style_text_font(url_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(url_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(url_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(url_value, 260);
    lv_obj_align(url_value, LV_ALIGN_TOP_LEFT, 14, 60);

    // Ghost red logout button
    lv_obj_t *logout_btn = lv_btn_create(screen_nightscout_status);
    lv_obj_set_size(logout_btn, 180, 38);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(logout_btn, 1, 0);
    lv_obj_set_style_border_opa(logout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Sign Out");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, nightscout_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

