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
// Raised from 16384 on 2026-09-09. A bench unit sat at exactly 16384, passed
// this gate, and then failed the handshake with ESP_ERR_HTTP_CONNECT; the same
// unit rebooted to 18432 and the very next check succeeded. The old value was
// the floor rather than a budget, so the one case it was meant to prevent, a
// doomed handshake that closes the provider's client to make room and shows
// the user "Check WiFi and try again", was exactly what it let through.
// Skipping a cycle is the graceful outcome this exists to choose.
//
// 18432 and not more. It is the lowest value measured to actually work,
// and 20 KB was tried first purely because a comment elsewhere claimed
// that figure. Two hundred samples per unit said otherwise: a 20 KB gate
// let the no-card unit through on 6% of cycles against 36% at 18 KB,
// which stops being a guard and starts being an off switch. The gate is
// also measured a second time after the provider client is closed, so
// the real pass rate is better than an idle sample suggests.
// A handshake needs room for several allocations, not one big one, so both
// numbers matter and total free is the one that actually tracks the outcome.
// Measured across both bench units: a check failed with a largest block of
// 22528 and succeeded at 20480, while the unit with an SD card failed every
// time at ~28 KB free and the unit without one succeeded every time at ~33 KB.
// Three rounds of tuning the block threshold changed nothing, because it was
// never the deciding quantity.
//
// The block figure that remains is the biggest single allocation a handshake
// makes, the 8 KB TLS input buffer plus headroom; 12288 is what
// weather_system.c already documents as measured for the location search.
// The floor is the one allocation that must be served whole: the 8192-byte
// TLS input buffer, plus slack for its header. 12288 was borrowed from the
// location search in weather_system.c, which was measured for a different
// path, and on a card-fitted unit it refused checks that had ample total
// free heap. Total free is the quantity that was shown to decide the outcome;
// this one only has to stop a request that cannot possibly fit.
#define HEARTBEAT_MIN_LARGEST_BLOCK 9216
#define HEARTBEAT_MIN_FREE_HEAP     30720

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
    size_t freeheap = esp_get_free_heap_size();
    if (largest < HEARTBEAT_MIN_LARGEST_BLOCK || freeheap < HEARTBEAT_MIN_FREE_HEAP) {
        heartbeat_release_provider_clients();
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        freeheap = esp_get_free_heap_size();
    }
    if (largest < HEARTBEAT_MIN_LARGEST_BLOCK || freeheap < HEARTBEAT_MIN_FREE_HEAP) {
        ESP_LOGW(TAG, "Heartbeat skipped: free %u (need %d), largest %u (need %d)",
                 (unsigned)freeheap, HEARTBEAT_MIN_FREE_HEAP,
                 (unsigned)largest, HEARTBEAT_MIN_LARGEST_BLOCK);
        if (took_mutex) xSemaphoreGive(network_mutex);
        return ESP_ERR_NO_MEM;
    }

    char url[384];
    // The version goes on every beat, not just the ones that carry a location:
    // the fleet panel groups by it, and a device with no location set would
    // otherwise never report one. MAJOR.MINOR.PATCH only — the date and stage
    // would fragment the distribution into one bucket per build for no gain,
    // and this is the same triple the update checker compares.
    int n = snprintf(url, sizeof(url),
                     "https://cygm.me/api/heartbeat.php?id=%s&v=%d.%d.%d",
                     cygm_device_id(),
                     CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);

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
