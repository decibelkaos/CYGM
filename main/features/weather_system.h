/*
 * weather_system.h
 *
 * Weather fetching and display, plus worldwide location search.
 * Both use the Open-Meteo APIs (free, no API key).
 */

#ifndef FEATURES_WEATHER_SYSTEM_H
#define FEATURES_WEATHER_SYSTEM_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load zipcode, temperature unit, interval and cached coordinates from NVS. */
void load_weather_settings(void);

/**
 * Resolve a search query — city name, postal code or address — to coordinates.
 * Also sets user_location and user_timezone from the result.
 * Returns ESP_FAIL when the search yields nothing.
 */
esp_err_t zipcode_to_latlon(const char *zipcode, float *lat, float *lon);

/** Fetch current conditions and the forecast for a coordinate pair. */
esp_err_t fetch_weather(float lat, float lon);

/** Refresh temperature, condition, icon and sun times. Returns early if the UI is not up yet. */
void update_weather_display(void);

/** Refresh the home-screen location label. Returns early if the UI is not up yet. */
void update_location_display(void);

/** Refresh the home-screen sunrise/sunset labels. Returns early if the UI is not up yet. */
void update_sunrise_sunset_display(void);

/** Human-readable text for a mapped weather code (0-7). */
const char* get_weather_condition_text(int weather_code);

/** Weather task: fetches at the configured interval. */
void weather_update_task(void *pvParameters);

/** True between sunset and sunrise — selects the night weather icons. */
bool is_night_time(void);

#ifdef __cplusplus
}
#endif

#endif // FEATURES_WEATHER_SYSTEM_H
