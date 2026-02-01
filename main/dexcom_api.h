#ifndef DEXCOM_API_H
#define DEXCOM_API_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// ============================================================================
// DEXCOM SHARE API - Username/Password Authentication
// ============================================================================
// This uses the unofficial Dexcom Share API which allows direct login
// with username and password - no OAuth flow required.

// Region selection
typedef enum {
    DEXCOM_REGION_US = 0,   // United States
    DEXCOM_REGION_OUS,      // Outside US (Europe, etc.)
    DEXCOM_REGION_JP        // Japan
} dexcom_region_t;

// Glucose trend values (from Dexcom API)
typedef enum {
    TREND_NONE = 0,
    TREND_DOUBLE_UP,        // Rising quickly (3+ mg/dL/min)
    TREND_SINGLE_UP,        // Rising (2 mg/dL/min)
    TREND_FORTY_FIVE_UP,    // Rising slowly (1 mg/dL/min)
    TREND_FLAT,             // Steady
    TREND_FORTY_FIVE_DOWN,  // Falling slowly (-1 mg/dL/min)
    TREND_SINGLE_DOWN,      // Falling (-2 mg/dL/min)
    TREND_DOUBLE_DOWN,      // Falling quickly (-3+ mg/dL/min)
    TREND_NOT_COMPUTABLE,   // Cannot determine
    TREND_RATE_OUT_OF_RANGE // Rate too high to compute
} dexcom_trend_t;

// Glucose status codes
typedef enum {
    GLUCOSE_STATUS_OK = 0,           // Valid data
    GLUCOSE_STATUS_NO_DATA,          // No readings (empty array from API)
    GLUCOSE_STATUS_WARMUP,           // Likely sensor warmup (no data + recent auth)
    GLUCOSE_STATUS_SIGNAL_LOSS,      // Likely signal loss (data too old)
    GLUCOSE_STATUS_NOT_AUTHENTICATED // Not logged in
} dexcom_status_t;

// Glucose data structure
typedef struct {
    int value;              // mg/dL
    dexcom_trend_t trend;   // Trend direction
    time_t timestamp;       // Unix timestamp (from WT field)
    bool valid;             // Data validity flag
    dexcom_status_t status; // Status code (why data might be invalid)
} dexcom_glucose_t;

// Initialize Dexcom Share API client
esp_err_t dexcom_api_init(void);

// Set the region (call before authenticate if not US)
void dexcom_set_region(dexcom_region_t region);

// Authenticate with username and password
// This performs the full login flow:
// 1. Get account ID from username/password
// 2. Get session ID from account ID/password
// Returns ESP_OK on success, stores session internally
esp_err_t dexcom_authenticate(const char *username, const char *password);

// Fetch latest glucose reading
// Returns ESP_OK on success, fills glucose struct
// Will automatically refresh session if expired
esp_err_t dexcom_fetch_glucose(dexcom_glucose_t *glucose);

// Check if we have a valid session
bool dexcom_is_authenticated(void);

// Clear stored credentials and session
void dexcom_logout(void);

// Get trend arrow character for display
const char* dexcom_trend_arrow(dexcom_trend_t trend);

// Get trend description string
const char* dexcom_trend_description(dexcom_trend_t trend);

// Memory management - close/reopen persistent HTTP client
// Use this to free ~30KB memory when needed (e.g., for weather operations)
void dexcom_close_persistent_client(void);
esp_err_t dexcom_reopen_persistent_client(void);

#endif // DEXCOM_API_H
