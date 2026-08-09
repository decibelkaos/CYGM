#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

// WiFi connection status
typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED
} wifi_status_t;

// Initialize WiFi manager
esp_err_t wifi_manager_init(void);

// Connect to WiFi using saved credentials or prompt for new ones
esp_err_t wifi_manager_connect(void);

// Connect to a specific WiFi network with given credentials
// Handles state reset, auth mode negotiation, and timeout
esp_err_t wifi_manager_connect_to(const char *ssid, const char *password);

// Get current WiFi status
wifi_status_t wifi_manager_get_status(void);

// Check if WiFi is connected
bool wifi_manager_is_connected(void);

// Disconnect from WiFi
void wifi_manager_disconnect(void);

// Prompt user for WiFi credentials via CardKB
// Returns ESP_OK if credentials were entered, ESP_FAIL if cancelled
esp_err_t wifi_manager_prompt_credentials(char *ssid_out, char *password_out);

// Scan for available WiFi networks
// Returns number of networks found
int wifi_manager_scan_networks(wifi_ap_record_t *ap_records, int max_records);

// Scan, match against the saved networks in NVS, connect to the strongest match.
// Returns ESP_OK if reconnected, ESP_FAIL or ESP_ERR_NOT_FOUND otherwise.
esp_err_t wifi_manager_try_reconnect(void);

// Apply the WiFi regulatory country code derived from the user's timezone.
// Call after esp_wifi_start(), and again whenever the timezone changes.
void wifi_manager_apply_country(void);

// No disconnect has been observed since the current attempt started.
#define WIFI_MANAGER_REASON_NONE 0

// wifi_err_reason_t from the most recent WIFI_EVENT_STA_DISCONNECTED. Cleared to
// WIFI_MANAGER_REASON_NONE on a new attempt and on IP acquisition, so the UI can
// tell "no reason recorded" from a real failure.
uint8_t wifi_manager_last_disconnect_reason(void);

#endif // WIFI_MANAGER_H
