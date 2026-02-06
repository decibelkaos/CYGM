/*
 * weather_system.c
 *
 * Weather data fetching and display implementation
 */

#include "weather_system.h"
#include "time_system.h"
#include "shared_state.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "dexcom_api.h"
#include "postal_code_database.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "WEATHER_SYSTEM";

// HTTP response buffer for weather/geocoding
#define WEATHER_HTTP_BUFFER_SIZE 2048
static char http_response_buffer[WEATHER_HTTP_BUFFER_SIZE];
static int http_response_len = 0;

// Geocoded zipcode tracker (to detect when user changes zipcode)
char geocoded_zipcode[MAX_ZIPCODE_LEN] = {0};

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < WEATHER_HTTP_BUFFER_SIZE - 1) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            } else {
                // Buffer overflow protection
                ESP_LOGE(TAG, "Weather HTTP response buffer overflow! Response too large (%d + %d >= %d)",
                         http_response_len, evt->data_len, WEATHER_HTTP_BUFFER_SIZE);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void load_weather_settings(void) {
    weather_settings_t settings;

    // Try to load settings from NVS
    if (nvs_load_weather_settings(&settings) == ESP_OK) {
        // Apply loaded settings
        strncpy(user_zipcode, settings.zipcode, sizeof(user_zipcode) - 1);
        user_zipcode[sizeof(user_zipcode) - 1] = '\0';
        user_temp_celsius = settings.temp_celsius;
        user_weather_interval_min = settings.update_interval_min;
        use_postal_code_entry = settings.use_postal_code_entry;

        // Load cached coordinates and location if available
        if (settings.coords_valid) {
            user_latitude = settings.latitude;
            user_longitude = settings.longitude;
            strncpy(geocoded_zipcode, settings.geocoded_zipcode, sizeof(geocoded_zipcode) - 1);
            geocoded_zipcode[sizeof(geocoded_zipcode) - 1] = '\0';

            // Load location name
            if (strlen(settings.location) > 0) {
                strncpy(user_location, settings.location, sizeof(user_location) - 1);
                user_location[sizeof(user_location) - 1] = '\0';
                ESP_LOGI(TAG, "Using cached weather location: %s (%.4f, %.4f, zipcode: %s)",
                         user_location, user_latitude, user_longitude, geocoded_zipcode);

                // Auto-detect timezone from location (in case it was changed manually before)
                // Extract state abbreviation from location (format: "City, ST")
                char *comma = strrchr(user_location, ',');
                if (comma != NULL && strlen(comma) > 2) {
                    char state_abbr[3];
                    strncpy(state_abbr, comma + 2, 2);  // Skip comma and space
                    state_abbr[2] = '\0';

                    // Map state abbreviation to full name
                    const char *state_full = NULL;
                    if (strcmp(state_abbr, "WA") == 0) state_full = "Washington";
                    else if (strcmp(state_abbr, "CA") == 0) state_full = "California";
                    else if (strcmp(state_abbr, "OR") == 0) state_full = "Oregon";
                    else if (strcmp(state_abbr, "TX") == 0) state_full = "Texas";
                    else if (strcmp(state_abbr, "NY") == 0) state_full = "New York";
                    else if (strcmp(state_abbr, "FL") == 0) state_full = "Florida";
                    else if (strcmp(state_abbr, "ME") == 0) state_full = "Maine";
                    else if (strcmp(state_abbr, "IL") == 0) state_full = "Illinois";
                    else if (strcmp(state_abbr, "PA") == 0) state_full = "Pennsylvania";
                    else if (strcmp(state_abbr, "OH") == 0) state_full = "Ohio";
                    else if (strcmp(state_abbr, "GA") == 0) state_full = "Georgia";
                    else if (strcmp(state_abbr, "NC") == 0) state_full = "North Carolina";
                    else if (strcmp(state_abbr, "MI") == 0) state_full = "Michigan";
                    else if (strcmp(state_abbr, "NJ") == 0) state_full = "New Jersey";
                    else if (strcmp(state_abbr, "VA") == 0) state_full = "Virginia";
                    else if (strcmp(state_abbr, "AZ") == 0) state_full = "Arizona";
                    else if (strcmp(state_abbr, "MA") == 0) state_full = "Massachusetts";
                    else if (strcmp(state_abbr, "IN") == 0) state_full = "Indiana";
                    else if (strcmp(state_abbr, "TN") == 0) state_full = "Tennessee";
                    else if (strcmp(state_abbr, "MO") == 0) state_full = "Missouri";
                    // Add more states as needed

                    if (state_full != NULL) {
                        const char *detected_tz = state_to_timezone(state_full);

                        // Check if detected timezone differs from loaded timezone
                        if (strcmp(detected_tz, user_timezone) != 0) {
                            ESP_LOGI(TAG, "Timezone mismatch detected! Location '%s' suggests %s, but loaded %s",
                                     user_location, detected_tz, user_timezone);
                            ESP_LOGI(TAG, "Updating timezone to match location...");

                            strncpy(user_timezone, detected_tz, sizeof(user_timezone) - 1);
                            user_timezone[sizeof(user_timezone) - 1] = '\0';

                            // Save and apply the corrected timezone
                            nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
                            const char *posix_tz = get_posix_timezone(user_timezone);
                            setenv("TZ", posix_tz, 1);
                            tzset();
                            ESP_LOGI(TAG, "Timezone corrected to: %s", user_timezone);
                        } else {
                            ESP_LOGI(TAG, "Timezone matches location: %s", user_timezone);
                        }
                    }
                }
            } else {
                ESP_LOGI(TAG, "Using cached weather coordinates: %.4f, %.4f (for zipcode: %s)",
                         user_latitude, user_longitude, geocoded_zipcode);
            }
        }

        ESP_LOGI(TAG, "Loaded weather settings from NVS:");
        ESP_LOGI(TAG, "  Zipcode: %s", user_zipcode);
        ESP_LOGI(TAG, "  Unit: %s", user_temp_celsius ? "Celsius" : "Fahrenheit");
        ESP_LOGI(TAG, "  Update interval: %d minutes", user_weather_interval_min);
    } else {
        ESP_LOGI(TAG, "No saved weather settings, using defaults");
    }
}
const char* state_to_timezone(const char *state) {
    // Eastern Time (UTC-5/-4)
    if (strcmp(state, "Connecticut") == 0) return "America/New_York";
    if (strcmp(state, "Delaware") == 0) return "America/New_York";
    if (strcmp(state, "District of Columbia") == 0) return "America/New_York";
    if (strcmp(state, "Georgia") == 0) return "America/New_York";
    if (strcmp(state, "Maine") == 0) return "America/New_York";
    if (strcmp(state, "Maryland") == 0) return "America/New_York";
    if (strcmp(state, "Massachusetts") == 0) return "America/New_York";
    if (strcmp(state, "New Hampshire") == 0) return "America/New_York";
    if (strcmp(state, "New Jersey") == 0) return "America/New_York";
    if (strcmp(state, "New York") == 0) return "America/New_York";
    if (strcmp(state, "North Carolina") == 0) return "America/New_York";
    if (strcmp(state, "Ohio") == 0) return "America/New_York";
    if (strcmp(state, "Pennsylvania") == 0) return "America/New_York";
    if (strcmp(state, "Rhode Island") == 0) return "America/New_York";
    if (strcmp(state, "South Carolina") == 0) return "America/New_York";
    if (strcmp(state, "Vermont") == 0) return "America/New_York";
    if (strcmp(state, "Virginia") == 0) return "America/New_York";
    if (strcmp(state, "West Virginia") == 0) return "America/New_York";

    // Central Time (UTC-6/-5)
    if (strcmp(state, "Alabama") == 0) return "America/Chicago";
    if (strcmp(state, "Arkansas") == 0) return "America/Chicago";
    if (strcmp(state, "Illinois") == 0) return "America/Chicago";
    if (strcmp(state, "Indiana") == 0) return "America/Indiana/Indianapolis";  // Most of Indiana
    if (strcmp(state, "Iowa") == 0) return "America/Chicago";
    if (strcmp(state, "Kansas") == 0) return "America/Chicago";  // Most of Kansas
    if (strcmp(state, "Kentucky") == 0) return "America/Chicago";  // Western Kentucky
    if (strcmp(state, "Louisiana") == 0) return "America/Chicago";
    if (strcmp(state, "Minnesota") == 0) return "America/Chicago";
    if (strcmp(state, "Mississippi") == 0) return "America/Chicago";
    if (strcmp(state, "Missouri") == 0) return "America/Chicago";
    if (strcmp(state, "Nebraska") == 0) return "America/Chicago";  // Eastern Nebraska
    if (strcmp(state, "North Dakota") == 0) return "America/Chicago";  // Most of ND
    if (strcmp(state, "Oklahoma") == 0) return "America/Chicago";
    if (strcmp(state, "South Dakota") == 0) return "America/Chicago";  // Eastern SD
    if (strcmp(state, "Tennessee") == 0) return "America/Chicago";  // Western TN
    if (strcmp(state, "Texas") == 0) return "America/Chicago";  // Most of Texas
    if (strcmp(state, "Wisconsin") == 0) return "America/Chicago";

    // Mountain Time (UTC-7/-6)
    if (strcmp(state, "Arizona") == 0) return "America/Phoenix";  // No DST
    if (strcmp(state, "Colorado") == 0) return "America/Denver";
    if (strcmp(state, "Idaho") == 0) return "America/Denver";  // Southern Idaho
    if (strcmp(state, "Montana") == 0) return "America/Denver";
    if (strcmp(state, "New Mexico") == 0) return "America/Denver";
    if (strcmp(state, "Utah") == 0) return "America/Denver";
    if (strcmp(state, "Wyoming") == 0) return "America/Denver";

    // Pacific Time (UTC-8/-7)
    if (strcmp(state, "California") == 0) return "America/Los_Angeles";
    if (strcmp(state, "Nevada") == 0) return "America/Los_Angeles";
    if (strcmp(state, "Oregon") == 0) return "America/Los_Angeles";  // Most of Oregon
    if (strcmp(state, "Washington") == 0) return "America/Los_Angeles";

    // Alaska Time (UTC-9/-8)
    if (strcmp(state, "Alaska") == 0) return "America/Anchorage";

    // Hawaii-Aleutian Time (UTC-10, no DST)
    if (strcmp(state, "Hawaii") == 0) return "Pacific/Honolulu";

    // Default fallback for unknown states
    return "America/New_York";  // Eastern Time as default
}
esp_err_t zipcode_to_latlon(const char *zipcode, float *lat, float *lon) {
    // LOCAL DATABASE LOOKUP - No SSL, No Internet Required, Instant!
    ESP_LOGI(TAG, "Looking up postal code '%s' in local database...", zipcode);

    const postal_code_entry_t *entry = find_postal_code(zipcode);
    if (entry == NULL) {
        // Try fuzzy match for partial codes
        entry = find_postal_code_fuzzy(zipcode);
    }

    if (entry != NULL) {
        *lat = entry->latitude;
        *lon = entry->longitude;

        // Format location as "City, ST" (max 64 chars)
        snprintf(user_location, sizeof(user_location), "%s, %s", entry->city, entry->state);

        // Auto-detect timezone from state
        const char *detected_tz = state_to_timezone(entry->state);
        strncpy(user_timezone, detected_tz, sizeof(user_timezone) - 1);
        user_timezone[sizeof(user_timezone) - 1] = '\0';

        ESP_LOGI(TAG, "Postal code %s -> %.4f, %.4f (%s, %s, %s) [Timezone: %s]",
                 zipcode, *lat, *lon, entry->city, entry->state, entry->country, user_timezone);

        return ESP_OK;
    }

    ESP_LOGW(TAG, "Postal code '%s' not found in local database", zipcode);
    return ESP_FAIL;
}

// ===== COLOR PALETTE - Dark Blue Theme =====
#define COLOR_BG            0x000000  // Pure black background
#define COLOR_CARD_BG       0x1a2332  // Dark blue-grey cards
#define COLOR_TEXT_WHITE    0xFFFFFF  // Pure white text
#define COLOR_TEXT_GRAY     0x9CA3AF  // Light gray for secondary
#define COLOR_TEXT_DIM      0x6B7280  // Dimmed gray
#define COLOR_ACCENT_BLUE   0x3B82F6  // Bright blue accent
#define COLOR_ACCENT_LIGHT  0x60A5FA  // Lighter blue
#define COLOR_GREEN         0x22C55E  // Green for good
#define COLOR_ORANGE        0xF59E0B  // Orange warning
#define COLOR_RED           0xEF4444  // Red alert
#define COLOR_BORDER        0xFFFFFF  // White border/outline

// Draw weather icon on a canvas
static void draw_weather_icon(lv_obj_t *canvas, int weather_code) {
    ESP_LOGI(TAG, "Drawing weather icon code: %d (%s)", weather_code, get_weather_condition_text(weather_code));

    // Initialize drawing descriptors
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);

    switch (weather_code) {
        case 0: { // SUN (day) / MOON (night) - Clear sky
            if (is_night_time()) {
                // Draw crescent moon
                // Draw large circle (full moon)
                arc_dsc.color = lv_color_hex(0xF0E68C); // Pale yellow
                arc_dsc.width = 24;
                lv_canvas_draw_arc(canvas, 30, 25, 12, 0, 360, &arc_dsc);

                // Draw smaller circle on right to create crescent
                arc_dsc.color = lv_color_hex(COLOR_CARD_BG);  // Same as background
                arc_dsc.width = 20;
                lv_canvas_draw_arc(canvas, 36, 22, 10, 0, 360, &arc_dsc);
            } else {
                // Draw filled sun circle
                rect_dsc.bg_color = lv_color_hex(0xFFAA00); // Orange
                rect_dsc.bg_opa = LV_OPA_COVER;
                rect_dsc.radius = LV_RADIUS_CIRCLE;

                // Draw filled circle by drawing a rounded rectangle
                // lv_canvas_draw_rect signature: (canvas, x, y, w, h, &dsc)
                lv_canvas_draw_rect(canvas, 30 - 12, 25 - 12, 24, 24, &rect_dsc);

                // Draw 8 rays around the sun
                line_dsc.color = lv_color_hex(0xFFAA00);
                line_dsc.width = 2;
                lv_point_t ray_points[2];

                for (int i = 0; i < 8; i++) {
                    float angle = i * 45.0 * 3.14159 / 180.0;
                    ray_points[0].x = 30 + (int)(18 * cos(angle));
                    ray_points[0].y = 25 + (int)(18 * sin(angle));
                    ray_points[1].x = 30 + (int)(26 * cos(angle));
                    ray_points[1].y = 25 + (int)(26 * sin(angle));
                    lv_canvas_draw_line(canvas, ray_points, 2, &line_dsc);
                }
            }
            break;
        }

        case 1: { // PARTLY CLOUDY - Sun/Moon with cloud
            if (is_night_time()) {
                // Draw crescent moon in upper left
                arc_dsc.color = lv_color_hex(0xF0E68C);
                arc_dsc.width = 14;
                lv_canvas_draw_arc(canvas, 18, 15, 7, 0, 360, &arc_dsc);

                // Create crescent effect
                arc_dsc.color = lv_color_hex(COLOR_CARD_BG);
                arc_dsc.width = 12;
                lv_canvas_draw_arc(canvas, 22, 13, 6, 0, 360, &arc_dsc);
            } else {
                // Draw sun in upper left
                arc_dsc.color = lv_color_hex(0xFFAA00); // Orange
                arc_dsc.width = 6;
                lv_canvas_draw_arc(canvas, 18, 15, 8, 0, 360, &arc_dsc);

                // Draw partial rays (only visible ones)
                line_dsc.color = lv_color_hex(0xFFAA00);
                line_dsc.width = 2;
                lv_point_t ray_points[2];

                // Top-left rays only (4 rays)
                for (int i = 2; i < 6; i++) {
                    float angle = i * 45.0 * 3.14159 / 180.0;
                    ray_points[0].x = 18 + (int)(12 * cos(angle));
                    ray_points[0].y = 15 + (int)(12 * sin(angle));
                    ray_points[1].x = 18 + (int)(18 * cos(angle));
                    ray_points[1].y = 15 + (int)(18 * sin(angle));
                    lv_canvas_draw_line(canvas, ray_points, 2, &line_dsc);
                }
            }

            // Draw cloud in lower right (partially covering sun/moon)
            arc_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
            arc_dsc.width = 14;
            lv_canvas_draw_arc(canvas, 28, 28, 7, 0, 360, &arc_dsc);   // Left bump
            lv_canvas_draw_arc(canvas, 38, 24, 9, 0, 360, &arc_dsc);   // Middle bump
            lv_canvas_draw_arc(canvas, 46, 28, 7, 0, 360, &arc_dsc);   // Right bump
            break;
        }

        case 2: { // CLOUD - Simple cloud shape
            // Draw cloud using overlapping filled circles (arcs with thick width)
            arc_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
            arc_dsc.width = 16;  // Thick arc creates filled circle effect

            lv_canvas_draw_arc(canvas, 20, 25, 8, 0, 360, &arc_dsc);   // Left bump
            lv_canvas_draw_arc(canvas, 30, 20, 10, 0, 360, &arc_dsc);  // Middle bump (larger)
            lv_canvas_draw_arc(canvas, 40, 25, 8, 0, 360, &arc_dsc);   // Right bump
            break;
        }

        case 3: { // RAIN - Cloud + rain drops
            // Draw cloud
            arc_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
            arc_dsc.width = 16;

            lv_canvas_draw_arc(canvas, 20, 18, 8, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 30, 13, 10, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 40, 18, 8, 0, 360, &arc_dsc);

            // Draw rain drops (3 vertical lines)
            line_dsc.color = lv_color_hex(COLOR_ACCENT_BLUE);
            line_dsc.width = 2;
            lv_point_t rain[2];

            for (int i = 0; i < 3; i++) {
                rain[0].x = 18 + i * 12;
                rain[0].y = 32;
                rain[1].x = 18 + i * 12;
                rain[1].y = 44;
                lv_canvas_draw_line(canvas, rain, 2, &line_dsc);
            }
            break;
        }

        case 4: { // SNOW - Cloud + snowflakes
            // Draw cloud
            arc_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
            arc_dsc.width = 16;

            lv_canvas_draw_arc(canvas, 20, 18, 8, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 30, 13, 10, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 40, 18, 8, 0, 360, &arc_dsc);

            // Draw snowflakes (small crosses)
            line_dsc.color = lv_color_hex(COLOR_TEXT_WHITE);
            line_dsc.width = 2;
            lv_point_t snow[2];
            int positions[] = {20, 30, 40};

            for (int i = 0; i < 3; i++) {
                int x = positions[i];
                int y = 38;
                // Vertical line
                snow[0].x = x;
                snow[0].y = y - 4;
                snow[1].x = x;
                snow[1].y = y + 4;
                lv_canvas_draw_line(canvas, snow, 2, &line_dsc);

                // Horizontal line
                snow[0].x = x - 4;
                snow[0].y = y;
                snow[1].x = x + 4;
                snow[1].y = y;
                lv_canvas_draw_line(canvas, snow, 2, &line_dsc);
            }
            break;
        }

        case 5: { // STORM - Dark cloud + lightning
            // Draw dark cloud
            arc_dsc.color = lv_color_hex(0x555555); // Dark gray
            arc_dsc.width = 16;

            lv_canvas_draw_arc(canvas, 20, 18, 8, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 30, 13, 10, 0, 360, &arc_dsc);
            lv_canvas_draw_arc(canvas, 40, 18, 8, 0, 360, &arc_dsc);

            // Draw lightning bolt (zigzag using connected lines)
            line_dsc.color = lv_color_hex(0xFFFF00); // Yellow
            line_dsc.width = 3;
            lv_point_t bolt[2];

            // First segment
            bolt[0].x = 30; bolt[0].y = 28;
            bolt[1].x = 28; bolt[1].y = 36;
            lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

            // Second segment
            bolt[0].x = 28; bolt[0].y = 36;
            bolt[1].x = 32; bolt[1].y = 36;
            lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

            // Third segment
            bolt[0].x = 32; bolt[0].y = 36;
            bolt[1].x = 30; bolt[1].y = 46;
            lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);
            break;
        }

        case 6: { // FOG - Horizontal wavy layers
            // Draw multiple horizontal fog layers (wavy lines)
            line_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
            line_dsc.width = 3;
            lv_point_t fog[2];

            // Top fog layer
            fog[0].x = 15; fog[0].y = 20;
            fog[1].x = 45; fog[1].y = 20;
            lv_canvas_draw_line(canvas, fog, 2, &line_dsc);

            // Middle fog layer (slightly offset)
            fog[0].x = 10; fog[0].y = 28;
            fog[1].x = 50; fog[1].y = 28;
            lv_canvas_draw_line(canvas, fog, 2, &line_dsc);

            // Lower-middle fog layer
            fog[0].x = 12; fog[0].y = 36;
            fog[1].x = 48; fog[1].y = 36;
            lv_canvas_draw_line(canvas, fog, 2, &line_dsc);

            // Bottom fog layer
            fog[0].x = 18; fog[0].y = 44;
            fog[1].x = 42; fog[1].y = 44;
            lv_canvas_draw_line(canvas, fog, 2, &line_dsc);
            break;
        }

        case 7: { // ICE - Falling ice crystals (diagonal lines + crosses)
            // Draw ice crystals as small diagonal crosses
            line_dsc.color = lv_color_hex(0xAADDFF); // Light cyan/ice blue
            line_dsc.width = 2;
            lv_point_t ice[2];

            // Three ice crystals falling at different positions
            int positions[][2] = {{20, 20}, {35, 32}, {25, 42}};

            for (int i = 0; i < 3; i++) {
                int cx = positions[i][0];
                int cy = positions[i][1];

                // Diagonal line 1 (top-left to bottom-right)
                ice[0].x = cx - 4; ice[0].y = cy - 4;
                ice[1].x = cx + 4; ice[1].y = cy + 4;
                lv_canvas_draw_line(canvas, ice, 2, &line_dsc);

                // Diagonal line 2 (top-right to bottom-left)
                ice[0].x = cx + 4; ice[0].y = cy - 4;
                ice[1].x = cx - 4; ice[1].y = cy + 4;
                lv_canvas_draw_line(canvas, ice, 2, &line_dsc);

                // Vertical line (shorter)
                ice[0].x = cx; ice[0].y = cy - 3;
                ice[1].x = cx; ice[1].y = cy + 3;
                lv_canvas_draw_line(canvas, ice, 2, &line_dsc);

                // Horizontal line (shorter)
                ice[0].x = cx - 3; ice[0].y = cy;
                ice[1].x = cx + 3; ice[1].y = cy;
                lv_canvas_draw_line(canvas, ice, 2, &line_dsc);
            }
            break;
        }

        default:
            break;
    }
}
bool is_night_time(void) {
    if (sunrise_time == 0 || sunset_time == 0) {
        return false;  // No sunrise/sunset data yet
    }

    time_t now;
    time(&now);

    // Check if current time is after sunset or before sunrise
    return (now >= sunset_time || now < sunrise_time);
}

const char* get_weather_condition_text(int weather_code) {
    switch (weather_code) {
        case 0: return "Clear";           // Works for both day and night
        case 1: return "Partly Cloudy";
        case 2: return "Cloudy";
        case 3: return "Rain";
        case 4: return "Snow";
        case 5: return "Thunderstorm";
        case 6: return "Fog";
        case 7: return "Freezing Rain";
        default: return "Unknown";
    }
}

void update_location_display(void) {
    if (home_location_label == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_label_set_text(home_location_label, strlen(user_location) > 0 ? user_location : "");
        lvgl_port_unlock();
    }
}

void update_sunrise_sunset_display(void) {
    if (home_sunrise_label == NULL || home_sunset_label == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        if (sunrise_time != 0 && sunset_time != 0) {
            struct tm tm_sunrise;
            struct tm tm_sunset;
            localtime_r(&sunrise_time, &tm_sunrise);
            localtime_r(&sunset_time, &tm_sunset);

            char sunrise_buf[8];
            char sunset_buf[8];

            // Format based on user preference (12hr vs 24hr)
            if (user_24hr_format) {
                strftime(sunrise_buf, sizeof(sunrise_buf), "%H:%M", &tm_sunrise);
                strftime(sunset_buf, sizeof(sunset_buf), "%H:%M", &tm_sunset);
            } else {
                strftime(sunrise_buf, sizeof(sunrise_buf), "%I:%M", &tm_sunrise);
                strftime(sunset_buf, sizeof(sunset_buf), "%I:%M", &tm_sunset);
                // Remove leading zero from hour
                if (sunrise_buf[0] == '0') {
                    memmove(sunrise_buf, sunrise_buf + 1, strlen(sunrise_buf));
                }
                if (sunset_buf[0] == '0') {
                    memmove(sunset_buf, sunset_buf + 1, strlen(sunset_buf));
                }
            }

            lv_label_set_text(home_sunrise_label, sunrise_buf);
            lv_label_set_text(home_sunset_label, sunset_buf);
        } else {
            lv_label_set_text(home_sunrise_label, "--:--");
            lv_label_set_text(home_sunset_label, "--:--");
        }
        lvgl_port_unlock();
    }
}

static int map_weather_code(int wmo_code) {
    // WMO Weather interpretation codes
    if (wmo_code == 0) return 0;  // Clear sky
    if (wmo_code == 1 || wmo_code == 2) return 1;  // Partly cloudy
    if (wmo_code == 3) return 2;  // Cloudy
    if (wmo_code >= 45 && wmo_code <= 48) return 6;  // Fog
    if ((wmo_code >= 56 && wmo_code <= 57) || (wmo_code >= 66 && wmo_code <= 67)) return 7;  // Freezing rain/drizzle (ice)
    if (wmo_code >= 51 && wmo_code <= 65) return 3;  // Rain (excluding freezing rain)
    if (wmo_code >= 71 && wmo_code <= 86) return 4;  // Snow (including snow showers)
    if (wmo_code >= 95 && wmo_code <= 99) return 5;  // Thunderstorm
    return 2;  // Default to cloudy
}
esp_err_t fetch_weather(float lat, float lon) {
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, cannot fetch weather");
        return ESP_FAIL;
    }

    char url[512];
    // Use HTTP for Open-Meteo (free API, no SSL needed, saves ~20KB memory)
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset&temperature_unit=%s&timezone=auto&forecast_days=1",
             lat, lon, user_temp_celsius ? "celsius" : "fahrenheit");

    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,  // Increase timeout to 15 seconds
        .user_agent = "CYGM/1.0",
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,  // HTTP instead of HTTPS
        .skip_cert_common_name_check = true,
        .disable_auto_redirect = false
    };

    ESP_LOGI(TAG, "Weather URL: %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for weather");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Weather HTTP result: %s (status: %d)", esp_err_to_name(err), status_code);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Weather response: %s", http_response_buffer);

        // Parse JSON response
        cJSON *json = cJSON_Parse(http_response_buffer);
        if (json != NULL) {
            cJSON *current = cJSON_GetObjectItem(json, "current");
            cJSON *daily = cJSON_GetObjectItem(json, "daily");

            if (current && daily) {
                cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
                cJSON *weather_code = cJSON_GetObjectItem(current, "weather_code");
                cJSON *temp_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
                cJSON *temp_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
                cJSON *sunrise = cJSON_GetObjectItem(daily, "sunrise");
                cJSON *sunset = cJSON_GetObjectItem(daily, "sunset");

                if (temp && weather_code && temp_max && temp_min) {
                    current_temp_f = (int)temp->valuedouble;

                    // Get first element of daily arrays
                    high_temp_f = (int)cJSON_GetArrayItem(temp_max, 0)->valuedouble;
                    low_temp_f = (int)cJSON_GetArrayItem(temp_min, 0)->valuedouble;

                    // Parse sunrise/sunset times (ISO 8601 format)
                    if (sunrise && sunset) {
                        cJSON *sunrise_str = cJSON_GetArrayItem(sunrise, 0);
                        cJSON *sunset_str = cJSON_GetArrayItem(sunset, 0);
                        if (sunrise_str && cJSON_IsString(sunrise_str)) {
                            struct tm tm_sunrise = {0};
                            strptime(sunrise_str->valuestring, "%Y-%m-%dT%H:%M", &tm_sunrise);
                            sunrise_time = mktime(&tm_sunrise);
                        }
                        if (sunset_str && cJSON_IsString(sunset_str)) {
                            struct tm tm_sunset = {0};
                            strptime(sunset_str->valuestring, "%Y-%m-%dT%H:%M", &tm_sunset);
                            sunset_time = mktime(&tm_sunset);
                        }
                    }

                    current_weather_code = map_weather_code(weather_code->valueint);
                    current_condition = get_weather_condition_text(current_weather_code);

                    ESP_LOGI(TAG, "Weather updated: %d F (Hi: %d F, Lo: %d F), Code: %d",
                             current_temp_f, high_temp_f, low_temp_f, current_weather_code);

                    cJSON_Delete(json);
                    esp_http_client_cleanup(client);
                    return ESP_OK;
                }
            }
            cJSON_Delete(json);
        }
    }

    ESP_LOGE(TAG, "Failed to fetch weather: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}
void update_weather_display(void) {
    // Check if widgets exist (home screen is loaded)
    if (home_temp_label == NULL || home_hilo_label == NULL || home_weather_canvas == NULL ||
        home_condition_label == NULL || home_location_label == NULL ||
        home_sunrise_label == NULL || home_sunset_label == NULL) {
        ESP_LOGW(TAG, "Weather widgets not available (home screen not loaded)");
        return;
    }

    if (lvgl_port_lock(0)) {
        // Update temperature label
        char temp_buf[16];
        if (user_temp_celsius) {
            int temp_c = (current_temp_f - 32) * 5 / 9;
            snprintf(temp_buf, sizeof(temp_buf), "%d C", temp_c);
        } else {
            snprintf(temp_buf, sizeof(temp_buf), "%d F", current_temp_f);
        }
        lv_label_set_text(home_temp_label, temp_buf);

        // Update Hi/Lo label (stacked vertically)
        char hilo_buf[32];
        if (user_temp_celsius) {
            int high_c = (high_temp_f - 32) * 5 / 9;
            int low_c = (low_temp_f - 32) * 5 / 9;
            snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_c, low_c);
        } else {
            snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_temp_f, low_temp_f);
        }
        lv_label_set_text(home_hilo_label, hilo_buf);

        // Update weather condition text
        lv_label_set_text(home_condition_label, get_weather_condition_text(current_weather_code));

        // Update location label
        lv_label_set_text(home_location_label, strlen(user_location) > 0 ? user_location : "");

        // Update weather icon (clear canvas with fully transparent background first)
        lv_canvas_fill_bg(home_weather_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Fully transparent
        draw_weather_icon(home_weather_canvas, current_weather_code);

        ESP_LOGD(TAG, "Weather display updated: %d F (Hi: %d F, Lo: %d F) - %s",
                 current_temp_f, high_temp_f, low_temp_f, get_weather_condition_text(current_weather_code));
        lvgl_port_unlock();
    }

    // Update sunrise/sunset times (separate function since it has its own lock)
    update_sunrise_sunset_display();
}
void weather_update_task(void *pvParameters) {
    ESP_LOGI(TAG, "Weather update task started");

    // Wait for initial WiFi connection and Dexcom auth to complete
    // Short delay since we're using HTTP (no SSL overhead)
    ESP_LOGI(TAG, "Waiting 15 seconds before first weather attempt...");
    vTaskDelay(pdMS_TO_TICKS(15000));  // 15 seconds - gives Dexcom time to authenticate
    ESP_LOGI(TAG, "Free heap before weather geocoding: %lu bytes", esp_get_free_heap_size());

    while (1) {
        // Check if background tasks are paused (not on home screen)
        if (pause_background_tasks) {
            ESP_LOGI(TAG, "[Weather Task] Paused - not on home screen (waiting 10s)");
            vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
            continue;
        }

        // Local database lookup - no need to manage Dexcom client memory

        if (wifi_manager_is_connected() && strlen(user_zipcode) > 0) {
            // Only geocode if:
            // 1. No cached coordinates exist, OR
            // 2. Current zipcode != geocoded zipcode (user changed it)
            bool need_geocode = (user_latitude == 0.0f && user_longitude == 0.0f) ||
                                (strcmp(user_zipcode, geocoded_zipcode) != 0);

            if (need_geocode) {
                if (strcmp(user_zipcode, geocoded_zipcode) != 0) {
                    ESP_LOGI(TAG, "Zipcode changed (%s -> %s), re-geocoding...", geocoded_zipcode, user_zipcode);
                } else {
                    ESP_LOGI(TAG, "No cached coordinates - need to geocode zipcode: %s", user_zipcode);
                }

                // No time sync needed for local database lookup!

                // LOCAL DATABASE LOOKUP - No SSL, instant, no memory issues!
                ESP_LOGI(TAG, "Looking up postal code in local database (instant, no SSL)...");
                esp_err_t geocode_result = zipcode_to_latlon(user_zipcode, &user_latitude, &user_longitude);

                if (geocode_result == ESP_OK) {
                    // Save auto-detected timezone to NVS (timezone was set during geocoding)
                    nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
                    ESP_LOGI(TAG, "Auto-detected timezone saved to NVS: %s", user_timezone);

                    // Apply timezone to system immediately
                    const char *posix_tz = get_posix_timezone(user_timezone);
                    setenv("TZ", posix_tz, 1);
                    tzset();

                    // Force immediate time display update to reflect new timezone
                    update_time_display();
                    ESP_LOGI(TAG, "Time display updated with new timezone: %s", user_timezone);

                    // Update geocoded zipcode tracker
                    strncpy(geocoded_zipcode, user_zipcode, sizeof(geocoded_zipcode) - 1);
                    geocoded_zipcode[sizeof(geocoded_zipcode) - 1] = '\0';

                    ESP_LOGI(TAG, "Coordinates and location for zipcode: %s", geocoded_zipcode);
                    ESP_LOGI(TAG, "Location: %s (%.4f, %.4f)", user_location, user_latitude, user_longitude);

                    // Save location and coordinates to NVS (location already set from local database)
                    nvs_save_weather_coords(user_zipcode, user_latitude, user_longitude, user_location);

                    // Update location label immediately
                    update_location_display();
                } else {
                    ESP_LOGW(TAG, "Geocoding failed after 3 attempts - skipping weather update this cycle");
                }
            } else {
                // Using cached coordinates - no geocoding needed
                ESP_LOGI(TAG, "Using cached coordinates for zipcode %s (no geocoding needed)", geocoded_zipcode);

                // Location name comes from cached NVS or from database lookup - no reverse geocoding needed!
                ESP_LOGI(TAG, "Location: %s (%.4f, %.4f)", user_location, user_latitude, user_longitude);
            }

            // Fetch weather if we have coordinates
            if (user_latitude != 0.0f && user_longitude != 0.0f) {
                // CRITICAL: Take network mutex to ensure exclusive access
                ESP_LOGI(TAG, "Waiting for network mutex...");
                if (xSemaphoreTake(network_mutex, portMAX_DELAY) == pdTRUE) {
                    ESP_LOGI(TAG, "Network mutex acquired for weather fetch");

                    // Weather uses HTTP (not HTTPS), so we don't need to close Dexcom SSL client
                    // This keeps the Dexcom persistent connection alive for next glucose fetch
                    ESP_LOGI(TAG, "Fetching weather for %.4f, %.4f", user_latitude, user_longitude);
                    ESP_LOGI(TAG, "Free heap before weather fetch: %lu bytes", esp_get_free_heap_size());

                    // Retry logic for weather fetch (may fail due to HTTP connection pool exhaustion)
                    for (int attempt = 1; attempt <= 3; attempt++) {
                        if (fetch_weather(user_latitude, user_longitude) == ESP_OK) {
                            ESP_LOGI(TAG, "Free heap after weather fetch: %lu bytes", esp_get_free_heap_size());
                            update_weather_display();
                            break;
                        } else {
                            ESP_LOGW(TAG, "Weather fetch attempt %d/3 failed", attempt);
                            ESP_LOGI(TAG, "Free heap after failed weather fetch: %lu bytes", esp_get_free_heap_size());
                            if (attempt < 3) {
                                ESP_LOGI(TAG, "Retrying in 5 seconds...");
                                vTaskDelay(pdMS_TO_TICKS(5000));  // Longer delay
                            }
                        }
                    }

                    // Release network mutex
                    xSemaphoreGive(network_mutex);
                    ESP_LOGI(TAG, "Network mutex released after weather fetch");
                } else {
                    ESP_LOGE(TAG, "Failed to acquire network mutex - this should never happen!");
                }
            }

            // Dexcom client will reopen automatically on next glucose fetch
        } else {
            ESP_LOGD(TAG, "Skipping weather update (WiFi: %d, Zipcode: %s)",
                    wifi_manager_is_connected(), user_zipcode);
        }

        // Wait for configured interval or until notified to update immediately
        // ulTaskNotifyTake clears notification and returns immediately if notified
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(user_weather_interval_min * 60 * 1000));
    }
}
