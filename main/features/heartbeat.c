/*
 * heartbeat.c
 *
 * Sends a lightweight HTTP GET to cygm.me/api/heartbeat.php with the
 * device's WiFi MAC address as an anonymous identifier.  No TLS, no
 * response parsing — just fire and forget.
 *
 * When the user has set a weather location, the city and its coordinates go
 * along too, so the site's device map can place the device where the user
 * actually is.  Without it the server falls back to the network address, which
 * only ever resolves to the provider's serving area.  Coordinates are sent to
 * two decimals — the postal district, not the street.
 */

#include "heartbeat.h"
#include "shared_state.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "HEARTBEAT";

/* Percent-encode everything outside the unreserved set, so city names with
   spaces, commas or accents survive the query string. */
static void url_encode(const char *in, char *out, size_t out_size) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (safe) {
            if (o + 2 > out_size) break;
            out[o++] = (char)c;
        } else {
            if (o + 4 > out_size) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

esp_err_t heartbeat_send(void) {
    // Get WiFi station MAC address
    uint8_t mac[6];
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read MAC: %s", esp_err_to_name(mac_err));
        return mac_err;
    }

    char url[384];
    int n = snprintf(url, sizeof(url),
                     "http://cygm.me/api/heartbeat.php?id=%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Zero coordinates are the "no location set yet" state, not a real place.
    if (n > 0 && n < (int)sizeof(url) &&
        (user_latitude != 0.0f || user_longitude != 0.0f) &&
        user_location[0] != '\0') {
        char loc_enc[3 * sizeof(user_location)];
        url_encode(user_location, loc_enc, sizeof(loc_enc));
        snprintf(url + n, sizeof(url) - (size_t)n, "&lat=%.2f&lon=%.2f&loc=%s",
                 user_latitude, user_longitude, loc_enc);
    }

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
