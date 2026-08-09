/*
 * glucose_history.h
 *
 * Glucose reading history management (circular buffer + NVS persistence)
 * Stores last 24 hours of readings (288 points at 5-min intervals)
 */

#ifndef GLUCOSE_HISTORY_H
#define GLUCOSE_HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum history: 24 hours at 5-minute intervals = 288 readings
#define GLUCOSE_HISTORY_MAX_POINTS 288

// Single glucose reading with timestamp
typedef struct {
    int64_t timestamp;  // Unix timestamp (milliseconds)
    int glucose;        // mg/dL value
    bool valid;         // True if this slot contains valid data
} glucose_history_point_t;

// Circular buffer containing glucose history
typedef struct {
    glucose_history_point_t readings[GLUCOSE_HISTORY_MAX_POINTS];
    int count;  // Number of valid readings (0-24)
    int head;   // Next write position (circular buffer index)
} glucose_history_t;

// Initialize glucose history system (load from NVS if exists)
esp_err_t glucose_history_init(void);

// Add a new glucose reading to the history
void glucose_history_add(int glucose_value, int64_t timestamp);

// Get the current history buffer (read-only access)
const glucose_history_t* glucose_history_get(void);

// Get reading at specific index (0 = oldest, count-1 = newest)
const glucose_history_point_t* glucose_history_get_at(int index);

// Get number of valid readings in history
int glucose_history_get_count(void);

// Timestamp (ms) of the newest reading, or 0 when the history is empty.
// Used to size the gap that a provider backfill needs to close.
int64_t glucose_history_newest_timestamp(void);

// Insert a reading at its chronological position (backfill path). Deduplicates
// against any reading within ~2 minutes and keeps the buffer ordered. Does not
// write NVS itself — it flags the next glucose_history_add() to flush.
bool glucose_history_insert(int glucose_value, int64_t timestamp);

// Find the closest reading to a given timestamp
const glucose_history_point_t* glucose_history_find_closest(int64_t timestamp);

// Get readings within a time window (returns count of points filled)
// points_out must have at least max_points capacity
// minutes_back: how many minutes of history to retrieve
int glucose_history_get_range(int minutes_back, glucose_history_point_t *points_out, int max_points);

// Clear all history (rarely needed)
void glucose_history_clear(void);

// Save current history to NVS
esp_err_t glucose_history_save_to_nvs(void);

// Load history from NVS
esp_err_t glucose_history_load_from_nvs(void);

// Register shutdown handler to save history on esp_restart()
void glucose_history_register_shutdown_handler(void);

#ifdef __cplusplus
}
#endif

#endif // GLUCOSE_HISTORY_H
