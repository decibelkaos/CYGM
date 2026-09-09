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

/**
 * Gate every optional location/weather TLS request on available heap.
 *
 * A TLS session needs roughly 17KB contiguous, which this device does not always
 * have. Returns false when the request must be skipped; closes an idle provider
 * client first, since the glucose task reopens one on its next fetch. Callers
 * must already hold network_mutex, and must treat false as "try next cycle" —
 * never as a glucose error. `what` is a fixed description, never user data.
 */
bool location_tls_heap_ready(const char *what);

/** Refresh temperature, condition, icon and sun times. Returns early if the UI is not up yet. */
void update_weather_display(void);

/** Refresh the home-screen location label. Returns early if the UI is not up yet. */
void update_location_display(void);

/** Refresh the home-screen sunrise/sunset labels. Returns early if the UI is not up yet. */
void update_sunrise_sunset_display(void);

// Re-runs any of the three label updates above that lost the LVGL lock.
// Called from the home screen's 1 Hz timer; a no-op when nothing was lost.
void weather_display_retry(void);

/** Human-readable text for a mapped weather code (0-7). */
const char* get_weather_condition_text(int weather_code);

// Stored temperatures are always Fahrenheit; convert with rounding for display.
int weather_f_to_c(int temp_f);

/** Weather task: fetches at the configured interval. */
void weather_update_task(void *pvParameters);

/**
 * Ask the weather task to stop.
 *
 * It must never be vTaskDelete'd from outside: it takes the LVGL lock while
 * drawing and the network mutex while fetching, and FreeRTOS releases neither
 * for a deleted task. The task notices this request at its next safe point,
 * clears weather_task_handle and deletes itself; poll that handle to know when
 * it is gone, and carry on without it if it does not stop in time.
 */
void weather_task_request_stop(void);

/**
 * Withdraw a stop request the task has not acted on yet.
 *
 * A latched request would park the task moments after the caller gave up
 * waiting, with nothing left to recreate it.
 */
void weather_task_cancel_stop(void);

/** True between sunset and sunrise — selects the night weather icons. */
bool is_night_time(void);

#ifdef __cplusplus
}
#endif

#endif // FEATURES_WEATHER_SYSTEM_H
