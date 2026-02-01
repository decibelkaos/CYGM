/*
 * weather_system.h
 *
 * Weather data fetching and display module
 * Geocoding (zipcode to lat/lon), reverse geocoding, Open-Meteo API integration
 */

#ifndef FEATURES_WEATHER_SYSTEM_H
#define FEATURES_WEATHER_SYSTEM_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load weather settings from NVS
 * Loads zipcode, temperature unit, update interval, cached coordinates
 */
void load_weather_settings(void);

/**
 * Convert US zipcode to latitude/longitude coordinates
 * Uses Nominatim (OpenStreetMap) geocoding API
 * @param zipcode US zipcode (5 or 9 digits)
 * @param lat Output latitude
 * @param lon Output longitude
 * @return ESP_OK on success
 */
esp_err_t zipcode_to_latlon(const char *zipcode, float *lat, float *lon);

/**
 * Reverse geocode coordinates to city/state name
 * Uses Nominatim reverse geocoding API
 * @param lat Latitude
 * @param lon Longitude
 * @return ESP_OK on success (sets user_location global)
 */
esp_err_t reverse_geocode(float lat, float lon);

/**
 * Fetch current weather and forecast from Open-Meteo API
 * @param lat Latitude
 * @param lon Longitude
 * @return ESP_OK on success
 */
esp_err_t fetch_weather(float lat, float lon);

/**
 * Update weather display widgets (temperature, condition, icon, sunrise/sunset)
 * Safe to call before UI is initialized (will return early)
 */
void update_weather_display(void);

/**
 * Update location label on home screen
 * Safe to call before UI is initialized (will return early)
 */
void update_location_display(void);

/**
 * Update sunrise/sunset time labels on home screen
 * Safe to call before UI is initialized (will return early)
 */
void update_sunrise_sunset_display(void);

/**
 * Get text description for weather code
 * @param weather_code Mapped weather code (0-7)
 * @return Human-readable weather condition string
 */
const char* get_weather_condition_text(int weather_code);

/**
 * Weather update task - fetches weather at configured interval
 * @param pvParameters Task parameter (unused)
 */
void weather_update_task(void *pvParameters);

/**
 * Check if current time is nighttime (for weather icons)
 * @return true if between sunset and sunrise
 */
bool is_night_time(void);

#ifdef __cplusplus
}
#endif

#endif // FEATURES_WEATHER_SYSTEM_H
