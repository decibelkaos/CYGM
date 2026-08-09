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

// Full login flow: username/password -> account ID -> session ID.
// Returns ESP_OK on success and stores the session internally.
esp_err_t dexcom_authenticate(const char *username, const char *password);

// Fetch the latest glucose reading, refreshing the session if it has expired.
esp_err_t dexcom_fetch_glucose(dexcom_glucose_t *glucose);

// Check if we have a valid session
bool dexcom_is_authenticated(void);

// Session older than 2 hours? Read-only — never re-auths or mutates state.
bool dexcom_session_needs_refresh(void);

// Clear stored credentials and session
void dexcom_logout(void);

// Get trend arrow character for display
const char* dexcom_trend_arrow(dexcom_trend_t trend);

// Get trend description string
const char* dexcom_trend_description(dexcom_trend_t trend);

// Memory management - close persistent HTTP client to free ~15-20KB
void dexcom_close_persistent_client(void);

// True when the client is open, so a fetch reuses the connection and needs only
// ~15-20KB. False means a fresh SSL handshake, which needs ~35-40KB contiguous.
bool dexcom_persistent_client_is_open(void);

#endif // DEXCOM_API_H
