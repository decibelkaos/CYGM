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

#endif // WIFI_MANAGER_H
