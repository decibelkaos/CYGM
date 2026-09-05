/*
 * weather_system.c
 *
 * Weather data fetching and display implementation
 */

#include "weather_system.h"
#include "time_system.h"
#include "shared_state.h"
#include "main.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "geocoding_api.h"
#include "sd_logger.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// Track last successful weather fetch (persists across task delete/recreate)
static int64_t last_weather_fetch_ms = 0;

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

    if (nvs_load_weather_settings(&settings) == ESP_OK) {
        strncpy(user_zipcode, settings.zipcode, sizeof(user_zipcode) - 1);
        user_zipcode[sizeof(user_zipcode) - 1] = '\0';
        user_temp_celsius = settings.temp_celsius;
        user_weather_interval_min = settings.update_interval_min;

        // Load cached coordinates and location if available
        if (settings.coords_valid) {
            user_latitude = settings.latitude;
            user_longitude = settings.longitude;
            strncpy(geocoded_zipcode, settings.geocoded_zipcode, sizeof(geocoded_zipcode) - 1);
            geocoded_zipcode[sizeof(geocoded_zipcode) - 1] = '\0';

            if (strlen(settings.location) > 0) {
                strncpy(user_location, settings.location, sizeof(user_location) - 1);
                user_location[sizeof(user_location) - 1] = '\0';
                ESP_LOGI(TAG, "Using cached location: %s (%.4f, %.4f)",
                         user_location, user_latitude, user_longitude);
            } else {
                ESP_LOGI(TAG, "Using cached coordinates: %.4f, %.4f",
                         user_latitude, user_longitude);
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
esp_err_t zipcode_to_latlon(const char *zipcode, float *lat, float *lon) {
    // ONLINE GEOCODING - Uses Open-Meteo Geocoding API (free, worldwide)
    ESP_LOGI(TAG, "Searching for location '%s' using online geocoding...", zipcode);

    geocode_result_t results[MAX_GEOCODE_RESULTS];
    uint8_t result_count = 0;

    esp_err_t err = geocoding_search(zipcode, results, &result_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Geocoding API request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (result_count == 0) {
        ESP_LOGW(TAG, "No results found for: %s", zipcode);
        return ESP_FAIL;
    }

    // Use first result (most relevant)
    geocode_result_t *result = &results[0];
    *lat = result->latitude;
    *lon = result->longitude;

    // Format as "City, State" or "City, Country", truncated to fit user_location.
    if (result->admin1[0] != '\0') {
        // Has state/province - limit to ~30 chars each
        snprintf(user_location, sizeof(user_location), "%.30s, %.30s", result->name, result->admin1);
    } else {
        // No state, use country - limit to ~30 chars each
        snprintf(user_location, sizeof(user_location), "%.30s, %.30s", result->name, result->country);
    }

    if (result->timezone[0] != '\0') {
        strncpy(user_timezone, result->timezone, sizeof(user_timezone) - 1);
        user_timezone[sizeof(user_timezone) - 1] = '\0';
    } else {
        // Fallback to Eastern Time if no timezone provided
        strncpy(user_timezone, "America/New_York", sizeof(user_timezone) - 1);
        ESP_LOGW(TAG, "No timezone in result, using default: America/New_York");
    }

    ESP_LOGI(TAG, "Location '%s' -> %.4f, %.4f (%s, %s) [Timezone: %s]",
             zipcode, *lat, *lon, result->name, result->country, user_timezone);

    // Re-apply the system timezone and WiFi regulatory domain now that we know
    // the user's actual location — otherwise the clock and channel set keep the
    // boot defaults until the next reboot.
    setenv("TZ", get_posix_timezone(user_timezone), 1);
    tzset();
    wifi_manager_apply_country();

    return ESP_OK;
}

// The COLOR_* palette lives in shared_state.h — never keep a second copy here.

// draw_weather_icon() lives in main.c; its animation timer redraws continuously,
// so this file only calls it when new data arrives.
bool is_night_time(void) {
    if (sunrise_time == 0 || sunset_time == 0) {
        return false;  // No sunrise/sunset data yet
    }

    time_t now;
    time(&now);

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

    if (lvgl_port_lock(1)) {
        lv_label_set_text(home_location_label, strlen(user_location) > 0 ? user_location : "");
        lvgl_port_unlock();
    }
}

void update_sunrise_sunset_display(void) {
    if (home_sunrise_label == NULL || home_sunset_label == NULL) {
        return;
    }

    // Retry lock (5 attempts) — weather is lower priority than glucose
    bool locked = false;
    for (int i = 0; i < 5 && !locked; i++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (locked) {
        if (sunrise_time != 0 && sunset_time != 0) {
            struct tm tm_sunrise;
            struct tm tm_sunset;
            localtime_r(&sunrise_time, &tm_sunrise);
            localtime_r(&sunset_time, &tm_sunset);

            char sunrise_buf[8];
            char sunset_buf[8];

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

int weather_f_to_c(int temp_f) {
    return (int)lroundf((temp_f - 32) * 5.0f / 9.0f);
}

static int map_weather_code(int wmo_code) {
    // WMO 4677 weather interpretation codes as used by Open-Meteo
    if (wmo_code == 0) return 0;  // Clear sky
    if (wmo_code == 1 || wmo_code == 2) return 1;  // Mainly clear / partly cloudy
    if (wmo_code == 3) return 2;  // Overcast
    if (wmo_code >= 45 && wmo_code <= 48) return 6;  // Fog
    if ((wmo_code >= 56 && wmo_code <= 57) || (wmo_code >= 66 && wmo_code <= 67)) return 7;  // Freezing drizzle / rain
    if (wmo_code >= 51 && wmo_code <= 65) return 3;  // Drizzle and rain
    if (wmo_code >= 71 && wmo_code <= 77) return 4;  // Snow fall, snow grains
    if (wmo_code >= 80 && wmo_code <= 82) return 3;  // Rain showers (NOT snow)
    if (wmo_code >= 85 && wmo_code <= 86) return 4;  // Snow showers
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
    // Always fetch Fahrenheit: current_temp_f/high/low are stored in F and the
    // display converts. Fetching in the user's unit made Celsius get converted twice.
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset&temperature_unit=fahrenheit&timezone=auto&forecast_days=1",
             lat, lon);

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

                cJSON *temp_max0 = temp_max ? cJSON_GetArrayItem(temp_max, 0) : NULL;
                cJSON *temp_min0 = temp_min ? cJSON_GetArrayItem(temp_min, 0) : NULL;

                if (cJSON_IsNumber(temp) && cJSON_IsNumber(weather_code) &&
                    cJSON_IsNumber(temp_max0) && cJSON_IsNumber(temp_min0)) {
                    current_temp_f = (int)lround(temp->valuedouble);
                    high_temp_f = (int)lround(temp_max0->valuedouble);
                    low_temp_f = (int)lround(temp_min0->valuedouble);

                    // Parse sunrise/sunset times (ISO 8601 format)
                    if (sunrise && sunset) {
                        cJSON *sunrise_str = cJSON_GetArrayItem(sunrise, 0);
                        cJSON *sunset_str = cJSON_GetArrayItem(sunset, 0);
                        if (sunrise_str && cJSON_IsString(sunrise_str)) {
                            struct tm tm_sunrise = {0};
                            strptime(sunrise_str->valuestring, "%Y-%m-%dT%H:%M", &tm_sunrise);
                            tm_sunrise.tm_isdst = -1;  // Auto-determine DST (API returns local time)
                            sunrise_time = mktime(&tm_sunrise);
                        }
                        if (sunset_str && cJSON_IsString(sunset_str)) {
                            struct tm tm_sunset = {0};
                            strptime(sunset_str->valuestring, "%Y-%m-%dT%H:%M", &tm_sunset);
                            tm_sunset.tm_isdst = -1;  // Auto-determine DST (API returns local time)
                            sunset_time = mktime(&tm_sunset);
                        }
                    }

                    current_weather_code = map_weather_code(weather_code->valueint);
                    current_condition = get_weather_condition_text(current_weather_code);

                    ESP_LOGI(TAG, "Weather updated: %d F (Hi: %d F, Lo: %d F), Code: %d",
                             current_temp_f, high_temp_f, low_temp_f, current_weather_code);
                    sd_log(TAG, "Weather: %dF (Hi:%d Lo:%d) code=%d",
                           current_temp_f, high_temp_f, low_temp_f, current_weather_code);

                    cJSON_Delete(json);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    return ESP_OK;
                }
            }
            cJSON_Delete(json);
        }
    }

    ESP_LOGE(TAG, "Failed to fetch weather: %s", esp_err_to_name(err));
    sd_log(TAG, "Weather: FAILED %s", esp_err_to_name(err));
    esp_http_client_close(client);
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

    // Prepare text buffers outside lock (minimize lock hold time)
    char temp_buf[16];
    if (user_temp_celsius) {
        snprintf(temp_buf, sizeof(temp_buf), "%d C", weather_f_to_c(current_temp_f));
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "%d F", current_temp_f);
    }

    char hilo_buf[32];
    if (user_temp_celsius) {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d",
                 weather_f_to_c(high_temp_f), weather_f_to_c(low_temp_f));
    } else {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_temp_f, low_temp_f);
    }

    const char *condition_text = get_weather_condition_text(current_weather_code);
    const char *location_text = strlen(user_location) > 0 ? user_location : "";

    // Lock 1: Update text labels (~2-3ms)
    if (lvgl_port_lock(1)) {
        lv_label_set_text(home_temp_label, temp_buf);
        if (expanded_temp_label != NULL) {
            lv_label_set_text(expanded_temp_label, temp_buf);
        }
        lv_label_set_text(home_hilo_label, hilo_buf);
        lv_label_set_text(home_condition_label, condition_text);
        lv_label_set_text(home_location_label, location_text);
        lvgl_port_unlock();
    }

    // Lock 2: Draw weather icon (~5-10ms, separate to reduce contention)
    if (lvgl_port_lock(1)) {
        lv_canvas_fill_bg(home_weather_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
        draw_weather_icon(home_weather_canvas, current_weather_code);
        lvgl_port_unlock();
    }

    ESP_LOGD(TAG, "Weather display updated: %d F (Hi: %d F, Lo: %d F) - %s",
             current_temp_f, high_temp_f, low_temp_f, condition_text);

    // Update sunrise/sunset times (separate function with its own lock)
    update_sunrise_sunset_display();
}
void weather_update_task(void *pvParameters) {
    ESP_LOGI(TAG, "Weather update task started");

    // Check if we were recently recreated (glucose task deletes/recreates us for SSL).
    // Skip the initial delay and fetch if last fetch was recent enough.
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t interval_ms = (int64_t)user_weather_interval_min * 60 * 1000;
    int64_t elapsed_ms = now_ms - last_weather_fetch_ms;

    if (last_weather_fetch_ms > 0 && elapsed_ms < interval_ms) {
        // Recently fetched — sleep for the remaining interval
        int64_t remaining_ms = interval_ms - elapsed_ms;
        ESP_LOGI(TAG, "Recent fetch %ds ago — sleeping %ds until next",
                 (int)(elapsed_ms / 1000), (int)(remaining_ms / 1000));
        vTaskDelay(pdMS_TO_TICKS(remaining_ms > 0 ? (uint32_t)remaining_ms : 1000));
    } else {
        // First boot or interval elapsed — short delay for network stability
        ESP_LOGI(TAG, "Waiting 15 seconds before first weather attempt...");
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    ESP_LOGI(TAG, "Free heap before weather geocoding: %lu bytes", esp_get_free_heap_size());

    while (1) {
        // STRICT runtime policy: never fetch off-home
        if (pause_background_tasks || !home_screen_active) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        extern bool wifi_connected;
        if (!wifi_connected) {
            ESP_LOGI(TAG, "[Weather Task] WiFi not connected - pausing (waiting 10s)");
            vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
            continue;
        }

        if (wifi_manager_is_connected() && strlen(user_zipcode) > 0) {
            // Geocode only when there are no cached coordinates, or the user has
            // changed the zipcode since the cached ones were resolved.
            bool need_geocode = (user_latitude == 0.0f && user_longitude == 0.0f) ||
                                (strcmp(user_zipcode, geocoded_zipcode) != 0);

            if (need_geocode) {
                if (strcmp(user_zipcode, geocoded_zipcode) != 0) {
                    ESP_LOGI(TAG, "Location changed (%s -> %s), re-geocoding...", geocoded_zipcode, user_zipcode);
                } else {
                    ESP_LOGI(TAG, "No cached coordinates - need to geocode location: %s", user_zipcode);
                }

                ESP_LOGI(TAG, "Searching for location using Open-Meteo Geocoding API...");
                esp_err_t geocode_result = zipcode_to_latlon(user_zipcode, &user_latitude, &user_longitude);

                if (geocode_result == ESP_OK) {
                    // Save auto-detected timezone to NVS (timezone was set during geocoding)
                    nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
                    ESP_LOGI(TAG, "Auto-detected timezone saved to NVS: %s", user_timezone);

                    // TZ + WiFi country were already applied inside zipcode_to_latlon();
                    // no need to setenv/tzset again here. Just refresh the display.

                    update_time_display();
                    ESP_LOGI(TAG, "Time display updated with new timezone: %s", user_timezone);

                    strncpy(geocoded_zipcode, user_zipcode, sizeof(geocoded_zipcode) - 1);
                    geocoded_zipcode[sizeof(geocoded_zipcode) - 1] = '\0';

                    ESP_LOGI(TAG, "Geocoded location: %s", geocoded_zipcode);
                    ESP_LOGI(TAG, "Result: %s (%.4f, %.4f) [%s]", user_location, user_latitude, user_longitude, user_timezone);

                    // Save location, coordinates, and timezone to NVS (from geocoding API)
                    nvs_save_weather_coords(user_zipcode, user_latitude, user_longitude, user_location);

                    update_location_display();
                } else {
                    ESP_LOGW(TAG, "Online geocoding failed - skipping weather update this cycle");
                }
            } else {
                // Using cached coordinates - no geocoding needed
                ESP_LOGI(TAG, "Using cached coordinates for %s (no geocoding needed)", geocoded_zipcode);
                ESP_LOGI(TAG, "Location: %s (%.4f, %.4f)", user_location, user_latitude, user_longitude);
            }

            // Fetch weather if we have coordinates
            if (user_latitude != 0.0f && user_longitude != 0.0f) {
                // Retry with short critical sections: never sleep while holding network mutex
                for (int attempt = 1; attempt <= 3; attempt++) {
                    ESP_LOGI(TAG, "Waiting for network mutex (attempt %d/3)...", attempt);
                    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
                        ESP_LOGW(TAG, "Weather mutex timeout (attempt %d/3)", attempt);
                        continue;
                    }

                    ESP_LOGI(TAG, "Network mutex acquired for weather fetch");

                    // Weather uses plain HTTP — no need to close CGM SSL client.
                    // ~13KB free with CGM alive is enough for HTTP client + cJSON.
                    size_t weather_heap = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
                    ESP_LOGI(TAG, "Weather fetch (largest_block=%lu)", (unsigned long)weather_heap);

                    // Safety fallback: close the CGM client only when the heap is
                    // desperately low. Plain HTTP plus cJSON needs only ~2KB, and
                    // the response buffer is static rather than heap.
                    if (weather_heap < 2048) {
                        ESP_LOGW(TAG, "Low heap for weather (%lu) — closing CGM client", (unsigned long)weather_heap);
                        if (dexcom_persistent_client_is_open()) {
                            dexcom_close_persistent_client();
                        }
                        if (libre_persistent_client_is_open()) {
                            libre_close_persistent_client();
                        }
                    }

                    ESP_LOGI(TAG, "Fetching weather for %.4f, %.4f", user_latitude, user_longitude);
                    ESP_LOGI(TAG, "Free heap before weather fetch: %lu bytes", esp_get_free_heap_size());

                    esp_err_t fetch_ret = fetch_weather(user_latitude, user_longitude);

                    // Release immediately after network op
                    xSemaphoreGive(network_mutex);
                    ESP_LOGI(TAG, "Network mutex released after weather fetch attempt");

                    if (fetch_ret == ESP_OK) {
                        last_weather_fetch_ms = esp_timer_get_time() / 1000;
                        ESP_LOGI(TAG, "Free heap after weather fetch: %lu bytes", esp_get_free_heap_size());
                        update_weather_display();
                        break;
                    }

                    ESP_LOGW(TAG, "Weather fetch attempt %d/3 failed", attempt);
                    ESP_LOGI(TAG, "Free heap after failed weather fetch: %lu bytes", esp_get_free_heap_size());
                    if (attempt < 3) {
                        ESP_LOGI(TAG, "Retrying in 5 seconds...");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                }
            }

            // Dexcom client will reopen automatically on next glucose fetch
        } else {
            ESP_LOGD(TAG, "Skipping weather update (WiFi: %d, Zipcode: %s)",
                    wifi_manager_is_connected(), user_zipcode);
        }

        // Wait out the configured interval, or return at once if notified to update.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(user_weather_interval_min * 60 * 1000));
    }
}
