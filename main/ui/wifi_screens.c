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
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_SCREENS";

// Forward declarations for internal event callbacks
static void network_item_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void password_field_event_cb(lv_event_t *e);
static void wifi_password_cancel_btn_cb(lv_event_t *e);
static void wifi_password_ok_btn_cb(lv_event_t *e);

// WiFi scan task
void wifi_scan_task(void *pvParameters) {
    wifi_scan_and_show_networks();
    vTaskDelete(NULL);  // Delete task when done
}

// WiFi scan and show networks function
void wifi_scan_and_show_networks(void) {
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

// OK button callback for WiFi password screen
static void wifi_password_ok_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        // OK button pressed - connect to WiFi
        ESP_LOGI(TAG, "Connecting to WiFi: %s with password: %s", selected_ssid, wifi_password);

        // Save WiFi credentials
        nvs_save_wifi_credentials(selected_ssid, wifi_password);

        // Connect to WiFi in background
        led_set_breathing_color(0, 0, 127);  // Blue = connecting
        if (wifi_manager_connect() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connection successful");
            wifi_connected = true;
            led_set_breathing_color(0, 127, 0);  // Green = connected

            // Update WiFi status on home screen
            vTaskDelay(pdMS_TO_TICKS(100));
            if (lvgl_port_lock(0)) {
                ESP_LOGI(TAG, "Updating WiFi status label to Connected");
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " WiFi");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
                lvgl_port_unlock();
            } else {
                ESP_LOGW(TAG, "Failed to acquire LVGL lock for WiFi status update");
            }

            // Initialize SNTP
            initialize_sntp();

            // Note: Time and weather tasks are started in app_main(), not here

            // Return to home screen
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_home);
                lvgl_port_unlock();
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
        // OK button pressed - connect to WiFi
        ESP_LOGI(TAG, "Connecting to WiFi: %s with password: %s", selected_ssid, wifi_password);

        // Save WiFi credentials
        nvs_save_wifi_credentials(selected_ssid, wifi_password);

        // Connect to WiFi in background
        led_set_breathing_color(0, 0, 127);  // Blue = connecting
        if (wifi_manager_connect() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connection successful");
            wifi_connected = true;
            led_set_breathing_color(0, 127, 0);  // Green = connected

            // Update WiFi status on home screen
            vTaskDelay(pdMS_TO_TICKS(100));
            if (lvgl_port_lock(0)) {
                ESP_LOGI(TAG, "Updating WiFi status label to Connected");
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " WiFi");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
                lvgl_port_unlock();
            } else {
                ESP_LOGW(TAG, "Failed to acquire LVGL lock for WiFi status update");
            }

            // Initialize SNTP
            initialize_sntp();

            // Note: Time and weather tasks are started in app_main(), not here

            // Return to home screen
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_home);
                lvgl_port_unlock();
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
