/*
 * nightscout_api.h
 *
 * Nightscout CGM API client for ESP32.
 * Supports both HTTP and HTTPS connections.
 * Token-based auth (stateless, never expires).
 */

#ifndef NIGHTSCOUT_API_H
#define NIGHTSCOUT_API_H

#include "esp_err.h"
#include "cgm_types.h"
#include <stdbool.h>

// Validate URL + token by fetching /api/v1/status.json.
// Stores URL and token internally on success.
esp_err_t nightscout_authenticate(const char *base_url, const char *token);

// Fetch latest glucose from /api/v1/entries/sgv.json?count=1
esp_err_t nightscout_fetch_glucose(cgm_glucose_t *glucose);

// Check if validated (URL + token were accepted)
bool nightscout_is_authenticated(void);

// Tokens never expire — always returns false
bool nightscout_session_needs_refresh(void);

// Clear stored URL/token
void nightscout_logout(void);

// Persistent client management
void nightscout_close_persistent_client(void);
bool nightscout_persistent_client_is_open(void);

// Whether this Nightscout instance uses HTTPS (determined at authenticate time)
bool nightscout_uses_https(void);

#endif // NIGHTSCOUT_API_H
