/*
 * heartbeat.c
 *
 * Sends a lightweight HTTPS GET to cygm.me/api/heartbeat.php carrying a
 * pseudonymous device identifier.  No response parsing — just fire and forget.
 *
 * The identifier is 16 random bytes kept in NVS, not the WiFi MAC: the online
 * counter only needs to tell devices apart, and a MAC is a hardware identity
 * that follows the user everywhere else on the network.
 *
 * When the user has set a weather location, the city and its coordinates go
 * along too, so the site's device map can place the device where the user
 * actually is.  Without it the server falls back to the network address, which
 * only ever resolves to the provider's serving area.  Coordinates are sent to
 * two decimals — the postal district, not the street.
 *
 * Nothing here is worth a missed glucose fetch: the heartbeat gives up on a
 * busy network mutex or a fragmented heap instead of competing for either.
 */

#include "heartbeat.h"
#include "shared_state.h"
#include "nvs_config.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "nightscout_api.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "HEARTBEAT";

// A TLS handshake with the 8KB IN record buffer needs a single contiguous block
// of roughly 9KB, plus the output buffer and session state. Below this the
// allocation would either fail with -0x7F00 or succeed by taking the block the
// next glucose fetch needs.
#define HEARTBEAT_MIN_LARGEST_BLOCK 16384

// Long enough to outlast a glucose fetch that is already in flight, short
// enough that a wedged holder costs one cycle rather than the task.
#define HEARTBEAT_MUTEX_WAIT_MS 6000

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

const char *cygm_device_id(void) {
    static char id_hex[2 * CYGM_DEVICE_ID_BYTES + 1];
    static bool id_ready = false;

    if (id_ready) {
        return id_hex;
    }

    uint8_t id[CYGM_DEVICE_ID_BYTES];
    if (nvs_load_device_id(id) != ESP_OK) {
        esp_fill_random(id, sizeof(id));
        // A write failure is survivable: the counter sees a new device after
        // the next reboot, which is better than blocking the heartbeat.
        if (nvs_save_device_id(id) != ESP_OK) {
            ESP_LOGW(TAG, "Device id could not be stored; using a boot-only id");
        }
    }

    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(id); i++) {
        id_hex[2 * i]     = hex[id[i] >> 4];
        id_hex[2 * i + 1] = hex[id[i] & 0x0F];
    }
    id_hex[2 * sizeof(id)] = '\0';
    id_ready = true;

    return id_hex;
}

// Providers hold a persistent TLS client between fetches; closing it is the
// only lever this path has on fragmentation. The glucose task reopens on its
// next fetch, so this costs a reconnect, not a reading.
static void heartbeat_release_provider_clients(void) {
    dexcom_close_persistent_client();
    libre_close_persistent_client();
    nightscout_close_persistent_client();
}

esp_err_t heartbeat_send(void) {
    // The 15-minute caller already holds network_mutex; the boot caller does
    // not. Taking a non-recursive mutex twice from one task would block until
    // the timeout and skip every scheduled heartbeat, so only take it when
    // this task is not already the holder, and only give back what was taken.
    bool took_mutex = false;
    if (network_mutex != NULL &&
        xSemaphoreGetMutexHolder(network_mutex) != xTaskGetCurrentTaskHandle()) {
        if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(HEARTBEAT_MUTEX_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Heartbeat skipped: network busy");
            return ESP_ERR_TIMEOUT;
        }
        took_mutex = true;
    }

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largest < HEARTBEAT_MIN_LARGEST_BLOCK) {
        heartbeat_release_provider_clients();
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    }
    if (largest < HEARTBEAT_MIN_LARGEST_BLOCK) {
        ESP_LOGW(TAG, "Heartbeat skipped: low heap (largest_block=%u)", (unsigned)largest);
        if (took_mutex) xSemaphoreGive(network_mutex);
        return ESP_ERR_NO_MEM;
    }

    char url[384];
    int n = snprintf(url, sizeof(url),
                     "https://cygm.me/api/heartbeat.php?id=%s", cygm_device_id());

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
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size    = 512,
        .buffer_size_tx = 512,
        // Authenticate the server rather than merely encrypting: the query
        // string carries the device's city and coordinates.
        .crt_bundle_attach = esp_crt_bundle_attach,
        // A followed redirect would hand that query string to whatever host the
        // Location header names.
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "HTTP client init failed");
        if (took_mutex) xSemaphoreGive(network_mutex);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (took_mutex) xSemaphoreGive(network_mutex);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Heartbeat sent");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Heartbeat failed: %s (status %d)", esp_err_to_name(err), status);
    return ESP_FAIL;
}
