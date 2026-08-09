/*
 * libre_api.h
 *
 * LibreLinkUp API client for FreeStyle Libre CGM data.
 * Mirrors the dexcom_api.h interface for consistent dispatch.
 */

#ifndef LIBRE_API_H
#define LIBRE_API_H

#include "esp_err.h"
#include "cgm_types.h"
#include <stdbool.h>

// Authenticate with LibreLinkUp email and password.
// Handles regional redirect and patient connection discovery.
esp_err_t libre_authenticate(const char *email, const char *password);

// Restore a saved session from NVS, verified with a test fetch. Fails silently,
// so the caller should fall through to libre_authenticate().
esp_err_t libre_restore_session(void);

// Fetch latest glucose reading from LibreLinkUp.
// Returns ESP_OK on success, fills glucose struct.
esp_err_t libre_fetch_glucose(cgm_glucose_t *glucose);

// Check if we have a valid authenticated session.
bool libre_is_authenticated(void);

// Check if JWT token is within 24h of expiry and needs refresh.
bool libre_session_needs_refresh(void);

// Clear stored credentials and session state.
void libre_logout(void);

// Memory management — close/reopen persistent HTTP client.
// Same lifecycle pattern as dexcom_close_persistent_client().
void libre_close_persistent_client(void);
esp_err_t libre_reopen_persistent_client(void);
bool libre_persistent_client_is_open(void);

#endif // LIBRE_API_H
