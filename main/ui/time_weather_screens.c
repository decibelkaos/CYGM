/*
 * time_weather_screens.c
 *
 * Time/Date/Weather settings screen implementation
 */

#include "time_weather_screens.h"
#include "shared_state.h"
#include "main.h"
#include "nvs_config.h"
#include "features/weather_system.h"
#include "features/time_system.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "TIME_WEATHER";

// Reference to interval value label
static lv_obj_t *weather_interval_value_label = NULL;

// Forward declarations
static void time_weather_back_event_cb(lv_event_t *e);
static void zipcode_edit_event_cb(lv_event_t *e);
static void weather_interval_slider_event_cb(lv_event_t *e);
static void zipcode_field_update_event_cb(lv_event_t *e);
static void zipcode_keyboard_event_cb(lv_event_t *e);

// Event handlers
static void zipcode_edit_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Zipcode edit clicked");
        create_zipcode_entry_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_zipcode_entry);
            lvgl_port_unlock();
        }
    }
}

static void weather_interval_slider_event_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    user_weather_interval_min = (uint8_t)lv_slider_get_value(slider);

    if (weather_interval_value_label != NULL) {
        lv_label_set_text_fmt(weather_interval_value_label, "%d min", user_weather_interval_min);
    }
}

static void time_weather_back_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Saving time & weather settings to NVS...");

        // Save time settings to NVS
        esp_err_t time_ret = nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
        if (time_ret == ESP_OK) {
            ESP_LOGI(TAG, "Time settings saved successfully");

            // Apply timezone to system immediately using POSIX format
            const char *posix_tz = get_posix_timezone(user_timezone);
            setenv("TZ", posix_tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone applied: %s -> %s", user_timezone, posix_tz);

            // Force immediate time display update
            update_time_display();
        } else {
            ESP_LOGE(TAG, "Failed to save time settings");
        }

        // Save weather settings to NVS
        esp_err_t weather_ret = nvs_save_weather_settings(user_zipcode, user_temp_celsius, user_weather_interval_min);
        if (weather_ret == ESP_OK) {
            ESP_LOGI(TAG, "Weather settings saved successfully");

            // Trigger immediate weather update
            if (weather_task_handle != NULL) {
                xTaskNotifyGive(weather_task_handle);
                ESP_LOGI(TAG, "Triggered immediate weather update");
            }
        } else {
            ESP_LOGE(TAG, "Failed to save weather settings");
        }

        // Return to menu screen
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}

static void zipcode_field_update_event_cb(lv_event_t *e) {
    lv_obj_t *display = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    const char *txt = lv_textarea_get_text(ta);
    lv_label_set_text(display, strlen(txt) > 0 ? txt : "00000");
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

                // Reset geocoding to trigger re-fetch
                memset(geocoded_zipcode, 0, sizeof(geocoded_zipcode));

                // Trigger immediate weather update
                if (weather_task_handle != NULL) {
                    xTaskNotifyGive(weather_task_handle);
                    ESP_LOGI(TAG, "Triggered immediate weather update after zipcode entry");
                }

                // Return to settings screen
                create_time_weather_settings_screen();
                if (lvgl_port_lock(0)) {
                    lv_scr_load(screen_time_weather_settings);
                    lvgl_port_unlock();
                }
            } else {
                ESP_LOGW(TAG, "Invalid zipcode length: %d (expected 5 or 9 digits)", len);
            }
        } else {
            // CANCEL pressed
            ESP_LOGI(TAG, "Zipcode entry cancelled");
            create_time_weather_settings_screen();
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_time_weather_settings);
                lvgl_port_unlock();
            }
        }
    }
}

void create_zipcode_entry_screen(void) {
    if (screen_zipcode_entry != NULL) {
        lv_obj_del(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }

    screen_zipcode_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_zipcode_entry, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(header, "Enter Zipcode");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Hidden textarea for keyboard input
    lv_obj_t *zipcode_field = lv_textarea_create(screen_zipcode_entry);
    lv_obj_set_size(zipcode_field, 1, 1);
    lv_obj_set_pos(zipcode_field, -100, -100);
    lv_textarea_set_one_line(zipcode_field, true);
    lv_textarea_set_max_length(zipcode_field, 9);
    lv_textarea_set_text(zipcode_field, user_zipcode);
    lv_obj_add_flag(zipcode_field, LV_OBJ_FLAG_HIDDEN);

    // Display label showing current input
    lv_obj_t *display_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(display_label, strlen(user_zipcode) > 0 ? user_zipcode : "00000");
    lv_obj_set_style_text_font(display_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(display_label, LV_ALIGN_TOP_MID, 0, 35);

    // LVGL keyboard (numeric mode)
    lv_obj_t *kbd = lv_keyboard_create(screen_zipcode_entry);
    lv_obj_set_size(kbd, 320, 165);
    lv_obj_align(kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kbd, zipcode_field);
    lv_keyboard_set_mode(kbd, LV_KEYBOARD_MODE_NUMBER);

    // Style keyboard
    lv_obj_set_style_bg_color(kbd, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, 0);

    // Update display label when typing
    lv_obj_add_event_cb(zipcode_field, zipcode_field_update_event_cb, LV_EVENT_VALUE_CHANGED, display_label);

    // Add keyboard event handler
    lv_obj_add_event_cb(kbd, zipcode_keyboard_event_cb, LV_EVENT_ALL, zipcode_field);
}

void create_time_weather_settings_screen(void) {
    screen_time_weather_settings = lv_obj_create(NULL);
    lv_obj_add_style(screen_time_weather_settings, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(header, "Time & Weather");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, time_weather_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Timezone (auto-detected from zipcode)
    lv_obj_t *tz_label = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(tz_label, "Timezone (auto):");
    lv_obj_set_style_text_font(tz_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tz_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(tz_label, LV_ALIGN_TOP_LEFT, 20, 55);

    lv_obj_t *tz_value = lv_label_create(screen_time_weather_settings);
    if (strlen(user_timezone) > 0) {
        lv_label_set_text(tz_value, user_timezone);
    } else {
        lv_label_set_text(tz_value, "Set zipcode below");
    }
    lv_obj_set_style_text_font(tz_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tz_value, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(tz_value, LV_ALIGN_TOP_LEFT, 20, 73);

    // Zipcode field
    lv_obj_t *zip_label = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(zip_label, "Zipcode:");
    lv_obj_set_style_text_font(zip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(zip_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(zip_label, LV_ALIGN_TOP_LEFT, 20, 105);

    lv_obj_t *zip_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(zip_btn, 280, 35);
    lv_obj_align(zip_btn, LV_ALIGN_TOP_LEFT, 20, 125);
    lv_obj_set_style_bg_color(zip_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(zip_btn, 8, 0);
    lv_obj_set_style_border_width(zip_btn, 0, 0);
    lv_obj_set_style_shadow_width(zip_btn, 0, 0);

    lv_obj_t *zip_value = lv_label_create(zip_btn);
    if (strlen(user_zipcode) > 0) {
        lv_label_set_text(zip_value, user_zipcode);
    } else {
        lv_label_set_text(zip_value, "Tap to enter zipcode");
    }
    lv_obj_set_style_text_font(zip_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(zip_value, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(zip_value);
    lv_obj_add_event_cb(zip_btn, zipcode_edit_event_cb, LV_EVENT_CLICKED, NULL);

    // Warning
    lv_obj_t *zip_warning = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(zip_warning, "May take up to 3 minutes to update");
    lv_obj_set_style_text_font(zip_warning, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(zip_warning, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(zip_warning, LV_ALIGN_TOP_LEFT, 20, 163);

    // Update interval
    lv_obj_t *interval_label = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(interval_label, "Update Interval:");
    lv_obj_set_style_text_font(interval_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(interval_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(interval_label, LV_ALIGN_TOP_LEFT, 20, 185);

    weather_interval_value_label = lv_label_create(screen_time_weather_settings);
    lv_label_set_text_fmt(weather_interval_value_label, "%d min", user_weather_interval_min);
    lv_obj_set_style_text_font(weather_interval_value_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_interval_value_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(weather_interval_value_label, LV_ALIGN_TOP_RIGHT, -20, 185);

    // Interval slider (5-90 minutes)
    lv_obj_t *interval_slider = lv_slider_create(screen_time_weather_settings);
    lv_obj_set_size(interval_slider, 280, 10);
    lv_obj_align(interval_slider, LV_ALIGN_TOP_LEFT, 20, 210);
    lv_slider_set_range(interval_slider, 5, 90);
    lv_slider_set_value(interval_slider, user_weather_interval_min, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(interval_slider, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(interval_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(interval_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_KNOB);
    lv_obj_add_event_cb(interval_slider, weather_interval_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
