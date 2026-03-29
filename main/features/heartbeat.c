/*
 * heartbeat.c
 *
 * Sends a lightweight HTTP GET to cygm.me/api/heartbeat.php with the
 * device's WiFi MAC address as an anonymous identifier.  No TLS, no
 * response parsing — just fire and forget.
 */

#include "heartbeat.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include <stdio.h>

static const char *TAG = "HEARTBEAT";

esp_err_t heartbeat_send(void) {
    // Get WiFi station MAC address
    uint8_t mac[6];
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read MAC: %s", esp_err_to_name(mac_err));
        return mac_err;
    }

    char url[96];
    snprintf(url, sizeof(url),
             "http://cygm.me/api/heartbeat.php?id=%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_http_client_config_t config = {
        .url            = url,
        .timeout_ms     = 5000,
        .method         = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .buffer_size    = 128,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Heartbeat sent (MAC=%02X%02X%02X%02X%02X%02X)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Heartbeat failed: %s (status %d)", esp_err_to_name(err), status);
    return ESP_FAIL;
}
