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
#include "features/postal_code_database.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "TIME_WEATHER";

// Reference to interval value label
static lv_obj_t *weather_interval_value_label = NULL;

// Code lookup screen references
static lv_obj_t *code_lookup_result_label = NULL;
static char temp_search_code[12] = {0};

// City lookup screen references
typedef enum {
    CITY_LOOKUP_STAGE_COUNTRY = 0,
    CITY_LOOKUP_STAGE_STATE,
    CITY_LOOKUP_STAGE_CITY
} city_lookup_stage_t;

static city_lookup_stage_t city_lookup_stage = CITY_LOOKUP_STAGE_COUNTRY;
static char selected_country[64] = {0};
static char selected_state[64] = {0};
static const postal_code_entry_t *selected_city_entry = NULL;
static lv_obj_t *city_lookup_title_label = NULL;
static lv_obj_t *city_lookup_result_label = NULL;
static lv_obj_t *city_lookup_dropdown = NULL;

// Forward declarations
static void time_weather_back_event_cb(lv_event_t *e);
static void city_lookup_event_cb(lv_event_t *e);
static void city_lookup_cancel_event_cb(lv_event_t *e);
static void city_lookup_ok_event_cb(lv_event_t *e);
static void city_lookup_dropdown_event_cb(lv_event_t *e);
static void code_lookup_event_cb(lv_event_t *e);
static void weather_interval_slider_event_cb(lv_event_t *e);
static void zipcode_field_update_event_cb(lv_event_t *e);
static void zipcode_keyboard_event_cb(lv_event_t *e);
static void create_city_lookup_screen(void);
static void update_city_lookup_dropdown(void);

// Event handlers
static void city_lookup_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "City Lookup clicked");
        create_city_lookup_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_zipcode_entry);  // Reuse screen pointer
            lvgl_port_unlock();
        }
    }
}

static void city_lookup_cancel_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "City lookup cancelled");

        // Clear selections
        memset(selected_country, 0, sizeof(selected_country));
        memset(selected_state, 0, sizeof(selected_state));
        selected_city_entry = NULL;
        city_lookup_stage = CITY_LOOKUP_STAGE_COUNTRY;

        // Return to settings screen
        create_time_weather_settings_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_time_weather_settings);
            lvgl_port_unlock();
        }
    }
}

static void city_lookup_ok_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (selected_city_entry != NULL) {
            ESP_LOGI(TAG, "City lookup confirmed: %s, %s, %s",
                     selected_city_entry->city, selected_city_entry->state, selected_city_entry->country);

            // Save location data
            strncpy(user_zipcode, selected_city_entry->code, sizeof(user_zipcode) - 1);
            user_zipcode[sizeof(user_zipcode) - 1] = '\0';

            user_latitude = selected_city_entry->latitude;
            user_longitude = selected_city_entry->longitude;

            snprintf(user_location, sizeof(user_location), "%s, %s",
                     selected_city_entry->city, selected_city_entry->state);

            // Auto-detect timezone
            const char *detected_tz = state_to_timezone(selected_city_entry->state);
            strncpy(user_timezone, detected_tz, sizeof(user_timezone) - 1);
            user_timezone[sizeof(user_timezone) - 1] = '\0';

            ESP_LOGI(TAG, "Location saved: %s (%.4f, %.4f)", user_location, user_latitude, user_longitude);

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

            // Clear selections
            memset(selected_country, 0, sizeof(selected_country));
            memset(selected_state, 0, sizeof(selected_state));
            selected_city_entry = NULL;
            city_lookup_stage = CITY_LOOKUP_STAGE_COUNTRY;

            // Return to settings screen
            create_time_weather_settings_screen();
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_time_weather_settings);
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGW(TAG, "No city selected");
        }
    }
}

static void city_lookup_dropdown_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *dropdown = lv_event_get_target(e);
        uint16_t selected_idx = lv_dropdown_get_selected(dropdown);

        if (city_lookup_stage == CITY_LOOKUP_STAGE_COUNTRY) {
            // Country selected - move to state
            const char *country = get_unique_country(selected_idx);
            if (country != NULL) {
                strncpy(selected_country, country, sizeof(selected_country) - 1);
                selected_country[sizeof(selected_country) - 1] = '\0';
                ESP_LOGI(TAG, "Country selected: %s", selected_country);

                // Check if this country has states
                int state_count = get_unique_state_count(selected_country);
                if (state_count > 0) {
                    city_lookup_stage = CITY_LOOKUP_STAGE_STATE;
                    memset(selected_state, 0, sizeof(selected_state));
                } else {
                    // No states, go directly to cities
                    city_lookup_stage = CITY_LOOKUP_STAGE_CITY;
                    memset(selected_state, 0, sizeof(selected_state));
                }

                update_city_lookup_dropdown();
            }
        } else if (city_lookup_stage == CITY_LOOKUP_STAGE_STATE) {
            // State selected - move to city
            const char *state = get_unique_state(selected_country, selected_idx);
            if (state != NULL) {
                strncpy(selected_state, state, sizeof(selected_state) - 1);
                selected_state[sizeof(selected_state) - 1] = '\0';
                ESP_LOGI(TAG, "State selected: %s", selected_state);

                city_lookup_stage = CITY_LOOKUP_STAGE_CITY;
                update_city_lookup_dropdown();
            }
        } else if (city_lookup_stage == CITY_LOOKUP_STAGE_CITY) {
            // City selected - show result
            selected_city_entry = get_unique_city(selected_country,
                                                   selected_state[0] ? selected_state : NULL,
                                                   selected_idx);
            if (selected_city_entry != NULL) {
                ESP_LOGI(TAG, "City selected: %s", selected_city_entry->city);

                // Update result label
                if (city_lookup_result_label != NULL) {
                    char result[128];
                    snprintf(result, sizeof(result), "%s, %s, %s\n(%.4f, %.4f)",
                             selected_city_entry->city, selected_city_entry->state,
                             selected_city_entry->country,
                             selected_city_entry->latitude, selected_city_entry->longitude);
                    lv_label_set_text(city_lookup_result_label, result);
                }
            }
        }
    }
}

static void code_lookup_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Code Lookup clicked");
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

    // Update display label
    lv_label_set_text(display, strlen(txt) > 0 ? txt : "Type postal code...");

    // Real-time search in postal code database
    if (strlen(txt) >= 3 && code_lookup_result_label != NULL) {
        const postal_code_entry_t *entry = find_location_unified(txt);
        if (entry != NULL) {
            // Format: "City, State, Country\n(Lat: XX.XX, Lon: XX.XX)"
            char result[128];
            snprintf(result, sizeof(result), "%s, %s, %s\n(%.4f, %.4f)",
                     entry->city, entry->state, entry->country,
                     entry->latitude, entry->longitude);
            lv_label_set_text(code_lookup_result_label, result);

            // Save to temp variables for OK button
            strncpy(temp_search_code, txt, sizeof(temp_search_code) - 1);
            temp_search_code[sizeof(temp_search_code) - 1] = '\0';
        } else {
            lv_label_set_text(code_lookup_result_label, "No match found");
        }
    } else if (code_lookup_result_label != NULL) {
        lv_label_set_text(code_lookup_result_label, "Type at least 3 characters");
    }
}

static void zipcode_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    void *user_data = lv_event_get_user_data(e);

    // Handle button clicks (user_data contains the synthetic event code)
    if (code == LV_EVENT_CLICKED) {
        code = (lv_event_code_t)(intptr_t)user_data;
    }

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (code == LV_EVENT_READY) {
            // OK pressed - use the last searched postal code
            if (strlen(temp_search_code) >= 3) {
                const postal_code_entry_t *entry = find_location_unified(temp_search_code);
                if (entry != NULL) {
                    // Save location data
                    strncpy(user_zipcode, temp_search_code, sizeof(user_zipcode) - 1);
                    user_zipcode[sizeof(user_zipcode) - 1] = '\0';

                    user_latitude = entry->latitude;
                    user_longitude = entry->longitude;

                    snprintf(user_location, sizeof(user_location), "%s, %s", entry->city, entry->state);

                    // Auto-detect timezone
                    const char *detected_tz = state_to_timezone(entry->state);
                    strncpy(user_timezone, detected_tz, sizeof(user_timezone) - 1);
                    user_timezone[sizeof(user_timezone) - 1] = '\0';

                    ESP_LOGI(TAG, "Location saved: %s (%.4f, %.4f)", user_location, user_latitude, user_longitude);

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
                } else {
                    ESP_LOGW(TAG, "No valid location found for: %s", temp_search_code);
                }
            } else {
                ESP_LOGW(TAG, "No postal code entered");
            }
        } else {
            // CANCEL pressed
            ESP_LOGI(TAG, "Code lookup cancelled");
        }

        // Clear temp data
        memset(temp_search_code, 0, sizeof(temp_search_code));
        code_lookup_result_label = NULL;

        // Return to settings screen
        create_time_weather_settings_screen();
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_time_weather_settings);
            lvgl_port_unlock();
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

    // Header (centered between buttons)
    lv_obj_t *header = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(header, "Code Lookup");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 8);

    // Hidden textarea for keyboard input
    lv_obj_t *zipcode_field = lv_textarea_create(screen_zipcode_entry);
    lv_obj_set_size(zipcode_field, 1, 1);
    lv_obj_set_pos(zipcode_field, -100, -100);
    lv_textarea_set_one_line(zipcode_field, true);
    lv_textarea_set_max_length(zipcode_field, 10);
    lv_textarea_set_text(zipcode_field, user_zipcode);
    lv_obj_add_flag(zipcode_field, LV_OBJ_FLAG_HIDDEN);

    // Display label showing postal code being typed
    lv_obj_t *display_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(display_label, strlen(user_zipcode) > 0 ? user_zipcode : "Type postal code...");
    lv_obj_set_style_text_font(display_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(display_label, LV_ALIGN_TOP_MID, 0, 40);

    // Result label (location with lat/lon)
    code_lookup_result_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(code_lookup_result_label, "Type at least 3 characters");
    lv_obj_set_style_text_font(code_lookup_result_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(code_lookup_result_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(code_lookup_result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(code_lookup_result_label, 300);
    lv_obj_align(code_lookup_result_label, LV_ALIGN_TOP_MID, 0, 65);

    // LVGL keyboard (text mode for alphanumeric postal codes)
    lv_obj_t *kbd = lv_keyboard_create(screen_zipcode_entry);
    lv_obj_set_size(kbd, 320, 150);
    lv_obj_align(kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kbd, zipcode_field);
    lv_keyboard_set_mode(kbd, LV_KEYBOARD_MODE_TEXT_UPPER);

    // Style keyboard
    lv_obj_set_style_bg_color(kbd, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, 0);

    // Update display and search as user types
    lv_obj_add_event_cb(zipcode_field, zipcode_field_update_event_cb, LV_EVENT_VALUE_CHANGED, display_label);

    // Add keyboard event handler (for enter/cancel)
    lv_obj_add_event_cb(kbd, zipcode_keyboard_event_cb, LV_EVENT_ALL, zipcode_field);

    // Add cancel/OK button handlers
    lv_obj_add_event_cb(cancel_btn, zipcode_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);
    lv_obj_add_event_cb(ok_btn, zipcode_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);
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

    // City Lookup button
    lv_obj_t *city_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(city_btn, 135, 35);
    lv_obj_align(city_btn, LV_ALIGN_TOP_LEFT, 20, 145);
    lv_obj_set_style_bg_color(city_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(city_btn, 8, 0);
    lv_obj_set_style_border_width(city_btn, 0, 0);
    lv_obj_set_style_shadow_width(city_btn, 0, 0);

    lv_obj_t *city_lbl = lv_label_create(city_btn);
    lv_label_set_text(city_lbl, "City Lookup");
    lv_obj_set_style_text_font(city_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(city_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(city_lbl);
    lv_obj_add_event_cb(city_btn, city_lookup_event_cb, LV_EVENT_CLICKED, NULL);

    // Code Lookup button
    lv_obj_t *code_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(code_btn, 135, 35);
    lv_obj_align(code_btn, LV_ALIGN_TOP_LEFT, 165, 145);
    lv_obj_set_style_bg_color(code_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(code_btn, 8, 0);
    lv_obj_set_style_border_width(code_btn, 0, 0);
    lv_obj_set_style_shadow_width(code_btn, 0, 0);

    lv_obj_t *code_lbl = lv_label_create(code_btn);
    lv_label_set_text(code_lbl, "Code Lookup");
    lv_obj_set_style_text_font(code_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(code_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(code_lbl);
    lv_obj_add_event_cb(code_btn, code_lookup_event_cb, LV_EVENT_CLICKED, NULL);

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

static void create_city_lookup_screen(void) {
    // Reset state
    memset(selected_country, 0, sizeof(selected_country));
    memset(selected_state, 0, sizeof(selected_state));
    selected_city_entry = NULL;
    city_lookup_stage = CITY_LOOKUP_STAGE_COUNTRY;

    // Reuse screen_zipcode_entry pointer
    if (screen_zipcode_entry != NULL) {
        lv_obj_del(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }

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
    lv_obj_add_event_cb(cancel_btn, city_lookup_cancel_event_cb, LV_EVENT_CLICKED, NULL);

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
    lv_obj_add_event_cb(ok_btn, city_lookup_ok_event_cb, LV_EVENT_CLICKED, NULL);

    // Title label (shows current stage)
    city_lookup_title_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(city_lookup_title_label, "Country");
    lv_obj_set_style_text_font(city_lookup_title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(city_lookup_title_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(city_lookup_title_label, LV_ALIGN_TOP_MID, 0, 8);

    // Result label (shows selected location with lat/lon)
    city_lookup_result_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(city_lookup_result_label, "Select country from dropdown");
    lv_obj_set_style_text_font(city_lookup_result_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(city_lookup_result_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(city_lookup_result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(city_lookup_result_label, 300);
    lv_obj_align(city_lookup_result_label, LV_ALIGN_TOP_MID, 0, 40);

    // Dropdown for selection
    city_lookup_dropdown = lv_dropdown_create(screen_zipcode_entry);
    lv_obj_set_size(city_lookup_dropdown, 300, 40);
    lv_obj_align(city_lookup_dropdown, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(city_lookup_dropdown, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_text_color(city_lookup_dropdown, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_add_event_cb(city_lookup_dropdown, city_lookup_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Populate initial dropdown (countries)
    update_city_lookup_dropdown();
}

static void update_city_lookup_dropdown(void) {
    if (city_lookup_dropdown == NULL) return;

    // Clear existing options
    lv_dropdown_clear_options(city_lookup_dropdown);

    if (city_lookup_stage == CITY_LOOKUP_STAGE_COUNTRY) {
        // Populate countries
        lv_label_set_text(city_lookup_title_label, "Country");
        lv_label_set_text(city_lookup_result_label, "Select country from dropdown");

        int country_count = get_unique_country_count();
        for (int i = 0; i < country_count && i < 100; i++) {  // Limit to 100 for performance
            const char *country = get_unique_country(i);
            if (country != NULL) {
                lv_dropdown_add_option(city_lookup_dropdown, country, LV_DROPDOWN_POS_LAST);
            }
        }
    } else if (city_lookup_stage == CITY_LOOKUP_STAGE_STATE) {
        // Populate states for selected country
        lv_label_set_text(city_lookup_title_label, "State/Province");

        char result[128];
        snprintf(result, sizeof(result), "Country: %s\nSelect state/province", selected_country);
        lv_label_set_text(city_lookup_result_label, result);

        int state_count = get_unique_state_count(selected_country);
        for (int i = 0; i < state_count && i < 100; i++) {
            const char *state = get_unique_state(selected_country, i);
            if (state != NULL) {
                lv_dropdown_add_option(city_lookup_dropdown, state, LV_DROPDOWN_POS_LAST);
            }
        }
    } else if (city_lookup_stage == CITY_LOOKUP_STAGE_CITY) {
        // Populate cities for selected country/state
        lv_label_set_text(city_lookup_title_label, "City");

        char result[128];
        if (selected_state[0] != '\0') {
            snprintf(result, sizeof(result), "%s, %s\nSelect city", selected_state, selected_country);
        } else {
            snprintf(result, sizeof(result), "%s\nSelect city", selected_country);
        }
        lv_label_set_text(city_lookup_result_label, result);

        int city_count = get_unique_city_count(selected_country,
                                                selected_state[0] ? selected_state : NULL);
        for (int i = 0; i < city_count && i < 100; i++) {
            const postal_code_entry_t *entry = get_unique_city(selected_country,
                                                                selected_state[0] ? selected_state : NULL,
                                                                i);
            if (entry != NULL) {
                lv_dropdown_add_option(city_lookup_dropdown, entry->city, LV_DROPDOWN_POS_LAST);
            }
        }
    }
}
