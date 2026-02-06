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
#include "features/geocoding_api.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "TIME_WEATHER";

// Reference to interval value label
static lv_obj_t *weather_interval_value_label = NULL;

// Location search screen references
static lv_obj_t *location_search_result_label = NULL;
static lv_obj_t *location_search_keyboard = NULL;
static lv_obj_t *location_search_prev_btn = NULL;
static lv_obj_t *location_search_next_btn = NULL;
static char temp_search_query[128] = {0};
static geocode_result_t search_results[MAX_GEOCODE_RESULTS];
static uint8_t search_result_count = 0;
static int selected_result_idx = -1;

// Forward declarations
static void time_weather_back_event_cb(lv_event_t *e);
static void location_search_event_cb(lv_event_t *e);
static void location_search_cancel_event_cb(lv_event_t *e);
static void location_search_ok_event_cb(lv_event_t *e);
static void location_search_prev_event_cb(lv_event_t *e);
static void location_search_next_event_cb(lv_event_t *e);
static void location_field_update_event_cb(lv_event_t *e);
static void location_keyboard_event_cb(lv_event_t *e);
static void weather_interval_slider_event_cb(lv_event_t *e);
static void create_location_search_screen(void);
static void update_location_search_result_display(void);

// Event handlers
static void location_search_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Location Search clicked");
        create_location_search_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_zipcode_entry);  // Reuse screen pointer
            lvgl_port_unlock();
        }
    }
}

static void location_search_cancel_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Location search cancelled");

        // Clear search state
        memset(temp_search_query, 0, sizeof(temp_search_query));
        search_result_count = 0;
        selected_result_idx = -1;
        location_search_keyboard = NULL;
        location_search_prev_btn = NULL;
        location_search_next_btn = NULL;

        // Return to settings screen
        create_time_weather_settings_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_time_weather_settings);
            lvgl_port_unlock();
        }
    }
}

static void location_search_prev_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (search_result_count > 0) {
            // Move to previous result (wrap around)
            selected_result_idx--;
            if (selected_result_idx < 0) {
                selected_result_idx = search_result_count - 1;
            }
            ESP_LOGI(TAG, "Previous result: %d/%d", selected_result_idx + 1, search_result_count);
            update_location_search_result_display();
        }
    }
}

static void location_search_next_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (search_result_count > 0) {
            // Move to next result (wrap around)
            selected_result_idx++;
            if (selected_result_idx >= search_result_count) {
                selected_result_idx = 0;
            }
            ESP_LOGI(TAG, "Next result: %d/%d", selected_result_idx + 1, search_result_count);
            update_location_search_result_display();
        }
    }
}

static void location_search_ok_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (selected_result_idx >= 0 && selected_result_idx < search_result_count) {
            geocode_result_t *result = &search_results[selected_result_idx];

            ESP_LOGI(TAG, "Location confirmed: %s, %s (%s)",
                     result->name, result->country, result->timezone);

            // Save search query as user_zipcode (will be used for re-geocoding if needed)
            strncpy(user_zipcode, temp_search_query, sizeof(user_zipcode) - 1);
            user_zipcode[sizeof(user_zipcode) - 1] = '\0';

            // Save coordinates
            user_latitude = result->latitude;
            user_longitude = result->longitude;

            // Format location name (safely truncate to fit in 64-byte buffer)
            if (result->admin1[0] != '\0') {
                snprintf(user_location, sizeof(user_location), "%.30s, %.30s",
                         result->name, result->admin1);
            } else {
                snprintf(user_location, sizeof(user_location), "%.30s, %.30s",
                         result->name, result->country);
            }

            // Use timezone from geocoding result
            if (result->timezone[0] != '\0') {
                strncpy(user_timezone, result->timezone, sizeof(user_timezone) - 1);
                user_timezone[sizeof(user_timezone) - 1] = '\0';
            } else {
                strncpy(user_timezone, "America/New_York", sizeof(user_timezone) - 1);
                ESP_LOGW(TAG, "No timezone in result, using default");
            }

            ESP_LOGI(TAG, "Location saved: %s (%.4f, %.4f) [%s]",
                     user_location, user_latitude, user_longitude, user_timezone);

            // Save to NVS
            nvs_save_weather_coords(user_zipcode, user_latitude, user_longitude, user_location);
            nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);

            // Apply timezone
            const char *posix_tz = get_posix_timezone(user_timezone);
            setenv("TZ", posix_tz, 1);
            tzset();

            // Reset geocoding tracker
            memset(geocoded_zipcode, 0, sizeof(geocoded_zipcode));

            // Trigger immediate weather update
            if (weather_task_handle != NULL) {
                xTaskNotifyGive(weather_task_handle);
                ESP_LOGI(TAG, "Triggered immediate weather update");
            }

            // Update display
            update_time_display();

            // Clear search state
            memset(temp_search_query, 0, sizeof(temp_search_query));
            search_result_count = 0;
            selected_result_idx = -1;
            location_search_keyboard = NULL;
            location_search_prev_btn = NULL;
            location_search_next_btn = NULL;

            // Return to settings screen
            create_time_weather_settings_screen();
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_time_weather_settings);
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGW(TAG, "No location selected");
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

static void update_location_search_result_display(void) {
    if (location_search_result_label == NULL || search_result_count == 0 || selected_result_idx < 0) {
        return;
    }

    geocode_result_t *result = &search_results[selected_result_idx];
    char result_text[350];

    if (search_result_count == 1) {
        // Single result
        snprintf(result_text, sizeof(result_text),
                 "%s, %s\n%s\n(%.4f, %.4f)\n\nPress OK to confirm",
                 result->name,
                 result->admin1[0] ? result->admin1 : result->country,
                 result->timezone,
                 result->latitude, result->longitude);
    } else {
        // Multiple results - show current result number
        snprintf(result_text, sizeof(result_text),
                 "Result %d of %d\n\n%s, %s\n%s\n(%.4f, %.4f)\n\nUse < > to browse\nPress OK to select",
                 selected_result_idx + 1, search_result_count,
                 result->name,
                 result->admin1[0] ? result->admin1 : result->country,
                 result->timezone,
                 result->latitude, result->longitude);
    }

    lv_label_set_text(location_search_result_label, result_text);

    // Show/hide navigation buttons
    if (search_result_count > 1) {
        if (location_search_prev_btn != NULL) {
            lv_obj_clear_flag(location_search_prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (location_search_next_btn != NULL) {
            lv_obj_clear_flag(location_search_next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (location_search_prev_btn != NULL) {
            lv_obj_add_flag(location_search_prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (location_search_next_btn != NULL) {
            lv_obj_add_flag(location_search_next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void location_field_update_event_cb(lv_event_t *e) {
    lv_obj_t *display = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    const char *txt = lv_textarea_get_text(ta);

    // Update display label
    lv_label_set_text(display, strlen(txt) > 0 ? txt : "Type city or postal code...");

    // Save to temp query for searching
    if (strlen(txt) > 0 && strlen(txt) < sizeof(temp_search_query)) {
        strncpy(temp_search_query, txt, sizeof(temp_search_query) - 1);
        temp_search_query[sizeof(temp_search_query) - 1] = '\0';

        // Show instructions
        if (location_search_result_label != NULL) {
            lv_label_set_text(location_search_result_label,
                              "Press OK to search\n(e.g., \"Paris\", \"Tokyo\", \"98092\")");
        }
    } else if (location_search_result_label != NULL) {
        lv_label_set_text(location_search_result_label, "Type a city name or postal code");
    }
}

static void location_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    void *user_data = lv_event_get_user_data(e);

    // Handle button clicks (user_data contains the synthetic event code)
    if (code == LV_EVENT_CLICKED) {
        code = (lv_event_code_t)(intptr_t)user_data;
    }

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (code == LV_EVENT_READY) {
            // Check if we already have search results to confirm
            if (selected_result_idx >= 0 && selected_result_idx < search_result_count) {
                // Confirmation mode - call the OK handler
                location_search_ok_event_cb(e);
                return;
            }

            // Search mode - perform online geocoding search
            if (strlen(temp_search_query) >= 2) {
                ESP_LOGI(TAG, "Searching for location: %s", temp_search_query);

                if (location_search_result_label != NULL) {
                    lv_label_set_text(location_search_result_label, "Searching...");
                }

                // Perform geocoding search
                esp_err_t err = geocoding_search(temp_search_query, search_results, &search_result_count);

                if (err == ESP_OK && search_result_count > 0) {
                    ESP_LOGI(TAG, "Found %d location(s)", search_result_count);

                    // Auto-select first result
                    selected_result_idx = 0;

                    // Display results with navigation
                    update_location_search_result_display();

                    // Hide keyboard to show results clearly
                    if (location_search_keyboard != NULL) {
                        lv_obj_add_flag(location_search_keyboard, LV_OBJ_FLAG_HIDDEN);
                    }

                    return;  // Stay on search screen for confirmation
                } else {
                    ESP_LOGW(TAG, "No locations found for: %s", temp_search_query);
                    if (location_search_result_label != NULL) {
                        lv_label_set_text(location_search_result_label,
                                          "No locations found\nTry different search terms");
                    }
                    return;  // Stay on search screen
                }
            } else {
                ESP_LOGW(TAG, "Search query too short");
                if (location_search_result_label != NULL) {
                    lv_label_set_text(location_search_result_label, "Type at least 2 characters");
                }
                return;
            }
        } else {
            // CANCEL pressed - call cancel handler
            location_search_cancel_event_cb(e);
            return;
        }
    }
}

static void create_location_search_screen(void) {
    if (screen_zipcode_entry != NULL) {
        lv_obj_del(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }

    // Clear search state
    memset(temp_search_query, 0, sizeof(temp_search_query));
    search_result_count = 0;
    selected_result_idx = -1;
    location_search_keyboard = NULL;
    location_search_prev_btn = NULL;
    location_search_next_btn = NULL;

    screen_zipcode_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_zipcode_entry, &style_bg, 0);

    // Cancel button (red, top-left)
    lv_obj_t *cancel_btn = lv_btn_create(screen_zipcode_entry);
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

    // OK button (green, top-right)
    lv_obj_t *ok_btn = lv_btn_create(screen_zipcode_entry);
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

    // Header
    lv_obj_t *header = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(header, "Location Search");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 8);

    // Hidden textarea for keyboard input
    lv_obj_t *location_field = lv_textarea_create(screen_zipcode_entry);
    lv_obj_set_size(location_field, 1, 1);
    lv_obj_set_pos(location_field, -100, -100);
    lv_textarea_set_one_line(location_field, true);
    lv_textarea_set_max_length(location_field, 100);
    lv_textarea_set_text(location_field, "");
    lv_obj_add_flag(location_field, LV_OBJ_FLAG_HIDDEN);

    // Display label showing query being typed
    lv_obj_t *display_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(display_label, "Type city or postal code...");
    lv_obj_set_style_text_font(display_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_width(display_label, 280);
    lv_obj_set_style_text_align(display_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(display_label, LV_ALIGN_TOP_MID, 0, 40);

    // Result label (search results)
    location_search_result_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(location_search_result_label, "Examples:\n\"Paris\", \"Tokyo\", \"London\"\n\"New York\", \"98092\"");
    lv_obj_set_style_text_font(location_search_result_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(location_search_result_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(location_search_result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(location_search_result_label, 300);
    lv_obj_align(location_search_result_label, LV_ALIGN_TOP_MID, 0, 75);

    // Previous button (< - left side, initially hidden)
    location_search_prev_btn = lv_btn_create(screen_zipcode_entry);
    lv_obj_set_size(location_search_prev_btn, 50, 40);
    lv_obj_align(location_search_prev_btn, LV_ALIGN_BOTTOM_LEFT, 10, -160);
    lv_obj_set_style_bg_color(location_search_prev_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(location_search_prev_btn, 8, 0);
    lv_obj_set_style_border_width(location_search_prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(location_search_prev_btn, 0, 0);
    lv_obj_add_flag(location_search_prev_btn, LV_OBJ_FLAG_HIDDEN);  // Hidden initially

    lv_obj_t *prev_lbl = lv_label_create(location_search_prev_btn);
    lv_label_set_text(prev_lbl, "<");
    lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(location_search_prev_btn, location_search_prev_event_cb, LV_EVENT_CLICKED, NULL);

    // Next button (> - right side, initially hidden)
    location_search_next_btn = lv_btn_create(screen_zipcode_entry);
    lv_obj_set_size(location_search_next_btn, 50, 40);
    lv_obj_align(location_search_next_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -160);
    lv_obj_set_style_bg_color(location_search_next_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(location_search_next_btn, 8, 0);
    lv_obj_set_style_border_width(location_search_next_btn, 0, 0);
    lv_obj_set_style_shadow_width(location_search_next_btn, 0, 0);
    lv_obj_add_flag(location_search_next_btn, LV_OBJ_FLAG_HIDDEN);  // Hidden initially

    lv_obj_t *next_lbl = lv_label_create(location_search_next_btn);
    lv_label_set_text(next_lbl, ">");
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(location_search_next_btn, location_search_next_event_cb, LV_EVENT_CLICKED, NULL);

    // LVGL keyboard (text mode)
    location_search_keyboard = lv_keyboard_create(screen_zipcode_entry);
    lv_obj_set_size(location_search_keyboard, 320, 150);
    lv_obj_align(location_search_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(location_search_keyboard, location_field);
    lv_keyboard_set_mode(location_search_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);

    // Style keyboard
    lv_obj_set_style_bg_color(location_search_keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(location_search_keyboard, LV_OPA_COVER, 0);

    // Update display as user types
    lv_obj_add_event_cb(location_field, location_field_update_event_cb, LV_EVENT_VALUE_CHANGED, display_label);

    // Add keyboard event handler
    lv_obj_add_event_cb(location_search_keyboard, location_keyboard_event_cb, LV_EVENT_ALL, location_field);

    // Add cancel/OK button handlers
    lv_obj_add_event_cb(cancel_btn, location_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);
    lv_obj_add_event_cb(ok_btn, location_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);
}

void create_time_weather_settings_screen(void) {
    // Delete old screen if it exists
    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }

    screen_time_weather_settings = lv_obj_create(NULL);
    lv_obj_add_style(screen_time_weather_settings, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(header, "Time & Weather");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button (matching Settings screen style)
    lv_obj_t *back_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

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

    // Location section header
    lv_obj_t *loc_label = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(loc_label, "Location:");
    lv_obj_set_style_text_font(loc_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(loc_label, LV_ALIGN_TOP_LEFT, 20, 105);

    // Current location display
    lv_obj_t *loc_value = lv_label_create(screen_time_weather_settings);
    if (strlen(user_location) > 0) {
        lv_label_set_text(loc_value, user_location);
    } else {
        lv_label_set_text(loc_value, "Not set - use lookup below");
    }
    lv_obj_set_style_text_font(loc_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(loc_value, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(loc_value, LV_ALIGN_TOP_LEFT, 20, 123);

    // Location Search button (centered)
    lv_obj_t *search_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(search_btn, 280, 40);
    lv_obj_align(search_btn, LV_ALIGN_TOP_LEFT, 20, 145);
    lv_obj_set_style_bg_color(search_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(search_btn, 8, 0);
    lv_obj_set_style_border_width(search_btn, 0, 0);
    lv_obj_set_style_shadow_width(search_btn, 0, 0);

    lv_obj_t *search_lbl = lv_label_create(search_btn);
    lv_label_set_text(search_lbl, "Search Location");
    lv_obj_set_style_text_font(search_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(search_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(search_lbl);
    lv_obj_add_event_cb(search_btn, location_search_event_cb, LV_EVENT_CLICKED, NULL);

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

