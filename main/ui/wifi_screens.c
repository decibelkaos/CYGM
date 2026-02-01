/*
 * wifi_screens.c
 *
 * WiFi configuration screens implementation
 */

#include "wifi_screens.h"
#include "shared_state.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_SCREENS";

// Forward declarations for event callbacks
static void network_item_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);

void wifi_scan_task(void *pvParameters);

static void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}
static void wifi_scan_task(void *pvParameters) {
    wifi_scan_and_show_networks();
    vTaskDelete(NULL);  // Delete task when done
}

void wifi_scan_and_show_networks(void);
static void create_wifi_password_screen(void);
static void create_time_weather_settings_screen(void);
static void create_zipcode_entry_screen(void);

// Forward declaration for event handlers
static void temp_toggle_event_cb(lv_event_t *e);
static void create_cgm_menu_screen(void);
static void create_dexcom_auth_screen(void);
static void create_dexcom_login_screen(void);
static void create_dexcom_username_entry_screen(void);
static void create_dexcom_password_entry_screen(void);
static void create_dexcom_status_screen(void);
static void create_alarm_settings_screen(void);
static void create_tone_picker_screen(alarm_config_t *alarm);
#if 0
static void create_alarm_preview_screen(void);
#endif
static void create_alarm_detail_editor_screen(alarm_config_t *alarm);

// Alarm event handlers
static void alarm_settings_back_event_cb(lv_event_t *e);
static void tone_picker_back_event_cb(lv_event_t *e);
#if 0
static void alarm_preview_back_event_cb(lv_event_t *e);
#endif
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_edit_btn_event_cb(lv_event_t *e);
// static void alarm_preview_open_event_cb(lv_event_t *e);  // Unused - preview button removed
static void tone_select_btn_event_cb(lv_event_t *e);
static void tone_preview_btn_event_cb(lv_event_t *e);
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_threshold_slider_event_cb(lv_event_t *e);
static void alarm_volume_slider_event_cb(lv_event_t *e);
static void alarm_audio_toggle_event_cb(lv_event_t *e);
static void alarm_visual_toggle_event_cb(lv_event_t *e);
static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);

// Alarm helper functions
static void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm,
                              int y_pos, int height);
#if 0
static void create_alarm_preview_card(lv_obj_t *parent, const char *title, int glucose_value,
                                      uint32_t text_color, int y_pos);
#endif
static void wifi_scan_task(void *pvParameters);

static void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}
static void wifi_scan_and_show_networks(void) {
    ESP_LOGI(TAG, "Scanning for WiFi networks...");

    // Scan for networks
    wifi_network_count = wifi_manager_scan_networks(wifi_networks, 10);
    ESP_LOGI(TAG, "Found %d networks", wifi_network_count);

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

        lv_obj_t *back_btn = lv_btn_create(screen_wifi_list);
        lv_obj_set_size(back_btn, 50, 30);
        lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_radius(back_btn, 8, 0);
        lv_obj_set_style_border_width(back_btn, 0, 0);
        lv_obj_set_style_shadow_width(back_btn, 0, 0);

        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
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

            // Arrow
            lv_obj_t *arrow = lv_label_create(row);
            lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -5, 0);

            // Add click handler with SSID as user data
            lv_obj_add_event_cb(row, network_item_event_cb, LV_EVENT_CLICKED, (void*)wifi_networks[i].ssid);
        }

        // Load the WiFi list screen
        lv_scr_load(screen_wifi_list);
        lvgl_port_unlock();
    }
}

void create_wifi_list_screen(void) {
    screen_wifi_list = lv_obj_create(NULL);
    lv_obj_add_style(screen_wifi_list, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_wifi_list);
    lv_label_set_text(header, "WiFi Networks");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_wifi_list);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Network list will be populated dynamically by wifi_scan_and_show_networks()
}

void create_wifi_password_screen(void);
static void create_time_weather_settings_screen(void);
static void create_zipcode_entry_screen(void);

// Forward declaration for event handlers
static void temp_toggle_event_cb(lv_event_t *e);
static void create_cgm_menu_screen(void);
static void create_dexcom_auth_screen(void);
static void create_dexcom_login_screen(void);
static void create_dexcom_username_entry_screen(void);
static void create_dexcom_password_entry_screen(void);
static void create_dexcom_status_screen(void);
static void create_alarm_settings_screen(void);
static void create_tone_picker_screen(alarm_config_t *alarm);
#if 0
static void create_alarm_preview_screen(void);
#endif
static void create_alarm_detail_editor_screen(alarm_config_t *alarm);

// Alarm event handlers
static void alarm_settings_back_event_cb(lv_event_t *e);
static void tone_picker_back_event_cb(lv_event_t *e);
#if 0
static void alarm_preview_back_event_cb(lv_event_t *e);
#endif
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_edit_btn_event_cb(lv_event_t *e);
// static void alarm_preview_open_event_cb(lv_event_t *e);  // Unused - preview button removed
static void tone_select_btn_event_cb(lv_event_t *e);
static void tone_preview_btn_event_cb(lv_event_t *e);
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_threshold_slider_event_cb(lv_event_t *e);
static void alarm_volume_slider_event_cb(lv_event_t *e);
static void alarm_audio_toggle_event_cb(lv_event_t *e);
static void alarm_visual_toggle_event_cb(lv_event_t *e);
static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);

// Alarm helper functions
static void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm,
                              int y_pos, int height);
#if 0
static void create_alarm_preview_card(lv_obj_t *parent, const char *title, int glucose_value,
                                      uint32_t text_color, int y_pos);
#endif
static void wifi_scan_task(void *pvParameters);

static void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}
static void create_wifi_password_screen(void) {
    screen_wifi_password = lv_obj_create(NULL);
    lv_obj_add_style(screen_wifi_password, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_wifi_password);
    lv_label_set_text(header, "Enter Password");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Password display (masked)
    lv_obj_t *display_container = lv_obj_create(screen_wifi_password);
    lv_obj_set_size(display_container, 300, 35);
    lv_obj_align(display_container, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_style(display_container, &style_card, 0);

    password_label = lv_label_create(display_container);
    lv_label_set_text(password_label, "");
    lv_obj_set_style_text_font(password_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(password_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Hidden textarea for keyboard input
    password_field = lv_textarea_create(screen_wifi_password);
    lv_obj_set_size(password_field, 1, 1);  // Hidden
    lv_obj_set_pos(password_field, -100, -100);  // Off screen
    lv_textarea_set_one_line(password_field, true);
    lv_textarea_set_password_mode(password_field, false);  // We handle masking ourselves
    lv_textarea_set_max_length(password_field, 63);

    // LVGL keyboard
    keyboard = lv_keyboard_create(screen_wifi_password);
    lv_obj_set_size(keyboard, 320, 165);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, password_field);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Style keyboard
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);

    // Add event handlers
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(password_field, password_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void network_item_event_cb(lv_event_t *e) {
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

void keyboard_event_cb(lv_event_t *e) {
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
        // OK button pressed - connect to WiFi
        ESP_LOGI(TAG, "Connecting to WiFi: %s with password: %s", selected_ssid, wifi_password);

        // Save credentials to NVS
        esp_err_t ret = nvs_save_wifi_credentials(selected_ssid, wifi_password);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");

        // Show connecting message
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_home);
            lvgl_port_unlock();
        }

        // Update LED to yellow (connecting)
        led_set_breathing_color(127, 127, 0);

        // Attempt connection in background (this blocks until connected or failed)
        ret = wifi_manager_connect();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected successfully!");
            wifi_connected = true;
            led_set_breathing_color(0, 127, 0);  // Green = connected

            // Initialize SNTP for time sync
            initialize_sntp();

            // Start time update task (check if not already running)
            static TaskHandle_t time_task_handle = NULL;
            if (time_task_handle == NULL) {
                ESP_LOGI(TAG, "Starting time update task");
                xTaskCreate(time_update_task, "time_update", 4096, NULL, 1, &time_task_handle);
            }

            // Start weather update task (uses HTTP - no SSL overhead!)
            if (weather_task_handle == NULL) {
                ESP_LOGI(TAG, "Starting weather update task (HTTP mode)");
                xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
            }

            // Update WiFi status icon color (with delay to ensure screen is loaded)
            vTaskDelay(pdMS_TO_TICKS(100));
            if (lvgl_port_lock(0)) {
                ESP_LOGI(TAG, "Updating WiFi status icon to Connected");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
                lvgl_port_unlock();
            } else {
                ESP_LOGW(TAG, "Failed to acquire LVGL lock for WiFi status update");
            }
        } else {
            ESP_LOGE(TAG, "WiFi connection failed");
            led_set_breathing_color(127, 0, 0);  // Red = failed

            // Update WiFi status on home screen
            vTaskDelay(pdMS_TO_TICKS(100));
            if (lvgl_port_lock(0)) {
                ESP_LOGI(TAG, "Updating WiFi status label to Failed");
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " Failed");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
                lvgl_port_unlock();
            } else {
                ESP_LOGW(TAG, "Failed to acquire LVGL lock for WiFi status update");
            }
        }
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
static void zipcode_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_user_data(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        const char *zip = lv_textarea_get_text(ta);
        int len = strlen(zip);

        if (code == LV_EVENT_READY) {
            // ENTER pressed - validate and save
            if (len == 5 || len == 9) {
                strncpy(user_zipcode, zip, sizeof(user_zipcode) - 1);
                user_zipcode[sizeof(user_zipcode) - 1] = '\0';
                ESP_LOGI(TAG, "Zipcode entered: %s", user_zipcode);

                // Reset coordinates to trigger re-geocoding
                user_latitude = 0.0f;
                user_longitude = 0.0f;

                // Trigger immediate weather update
                if (weather_task_handle != NULL) {
                    xTaskNotifyGive(weather_task_handle);
                    ESP_LOGI(TAG, "Triggered immediate weather update after zipcode entry");
                } else {
                    ESP_LOGW(TAG, "Cannot trigger weather update - task handle is NULL");
                }

                // Return to combined time/weather settings screen
                create_time_weather_settings_screen();
                lv_scr_load(screen_time_weather_menu);
                screen_zipcode_entry = NULL;
            } else {
                ESP_LOGW(TAG, "Invalid zipcode length: %d (expected 5 or 9 digits)", len);
            }
        } else {
            // CANCEL/Close pressed - return without saving
            ESP_LOGI(TAG, "Zipcode entry cancelled");

            // Return to combined time/weather settings screen
            create_time_weather_settings_screen();
            lv_scr_load(screen_time_weather_menu);
            screen_zipcode_entry = NULL;
        }
    }
}
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Handle cancel button - return to login screen without saving
    if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Username entry cancelled");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
        return;
    }

    // Handle OK/Ready button - save and return to login screen
    if (code == LV_EVENT_READY) {
        ESP_LOGI(TAG, "Username entry completed: %s", dexcom_username_buf);

        // Update the main login screen label
        if (dexcom_username_label != NULL) {
            lv_label_set_text(dexcom_username_label, dexcom_username_buf);
        }

        // Return to login screen
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    }
}
static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Handle cancel button - return to login screen without saving
    if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Password entry cancelled");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
        return;
    }

    // Handle OK/Ready button - save and return to login screen
    if (code == LV_EVENT_READY) {
        ESP_LOGI(TAG, "Password entry completed (masked)");

        // Update the main login screen label with masked password
        if (dexcom_password_label != NULL) {
            int len = strlen(dexcom_password_buf);
            if (len == 0) {
                lv_label_set_text(dexcom_password_label, "");
            } else {
                char display[65];
                for (int i = 0; i < len; i++) {
                    display[i] = '*';
                }
                display[len] = '\0';
                lv_label_set_text(dexcom_password_label, display);
            }
        }

        // Return to login screen
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    }
}

