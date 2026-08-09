/*
 * Nightscout API Implementation
 *
 * Fetches glucose data from a Nightscout server via REST API.
 * Supports both HTTP and HTTPS. Token-based auth (stateless).
 * API docs: https://nightscout.github.io/nightscout/setup_variables/
 */

#include "nightscout_api.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "NIGHTSCOUT";

// ============================================================================
// Configuration
// ============================================================================

#define HTTP_BUFFER_SIZE 2048
#define MAX_CONSECUTIVE_FAILURES 3

// ============================================================================
// State Variables
// ============================================================================

static char stored_url[128] = {0};      // Base URL (e.g., "http://my.nightscout.site")
static char stored_token[64] = {0};     // API token (optional — empty for public)
static bool is_authenticated = false;
static bool is_https = false;

// HTTP response buffer
static char http_response[HTTP_BUFFER_SIZE];
static int http_response_len = 0;

// Persistent HTTP client for connection reuse
static esp_http_client_handle_t persistent_client = NULL;
static int consecutive_failures = 0;

// Duplicate detection
static time_t last_successful_timestamp = 0;
static int duplicate_reading_count = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(http_response + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response[http_response_len] = '\0';
            }
            // Silently truncate if buffer full — status endpoint can exceed 2KB
            // but glucose endpoint is only ~230 bytes
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Map Nightscout direction string to cgm_trend_t
static cgm_trend_t map_direction_string(const char *dir) {
    if (!dir) return TREND_NONE;
    if (strcmp(dir, "DoubleUp") == 0)         return TREND_DOUBLE_UP;
    if (strcmp(dir, "SingleUp") == 0)         return TREND_SINGLE_UP;
    if (strcmp(dir, "FortyFiveUp") == 0)      return TREND_FORTY_FIVE_UP;
    if (strcmp(dir, "Flat") == 0)             return TREND_FLAT;
    if (strcmp(dir, "FortyFiveDown") == 0)    return TREND_FORTY_FIVE_DOWN;
    if (strcmp(dir, "SingleDown") == 0)       return TREND_SINGLE_DOWN;
    if (strcmp(dir, "DoubleDown") == 0)       return TREND_DOUBLE_DOWN;
    if (strcmp(dir, "NOT COMPUTABLE") == 0)   return TREND_NOT_COMPUTABLE;
    if (strcmp(dir, "RATE OUT OF RANGE") == 0) return TREND_RATE_OUT_OF_RANGE;
    return TREND_NONE;
}

// Build URL with optional token parameter
// Returns length written, or -1 on overflow
static int build_url(char *buf, size_t buf_size, const char *endpoint) {
    if (stored_token[0] != '\0') {
        return snprintf(buf, buf_size, "%s%s?token=%s", stored_url, endpoint, stored_token);
    }
    return snprintf(buf, buf_size, "%s%s", stored_url, endpoint);
}

// Strip trailing slash from URL
static void normalize_url(char *url) {
    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/') {
        url[--len] = '\0';
    }
}

// ============================================================================
// Persistent Client Management
// ============================================================================

bool nightscout_persistent_client_is_open(void) {
    return (persistent_client != NULL);
}

void nightscout_close_persistent_client(void) {
    if (persistent_client != NULL) {
        ESP_LOGI(TAG, "Closing persistent HTTP client");
        esp_http_client_close(persistent_client);
        esp_http_client_cleanup(persistent_client);
        persistent_client = NULL;
        ESP_LOGI(TAG, "Free heap after closing Nightscout client: %lu bytes",
                 esp_get_free_heap_size());
    }
}

bool nightscout_uses_https(void) {
    return is_https;
}

// ============================================================================
// Authentication (URL + Token Validation)
// ============================================================================

esp_err_t nightscout_authenticate(const char *base_url, const char *token) {
    if (!base_url || base_url[0] == '\0') {
        ESP_LOGE(TAG, "No Nightscout URL provided");
        return ESP_ERR_INVALID_ARG;
    }

    // Store and normalize URL — auto-prefix https:// if no scheme provided
    // Most Nightscout instances use HTTPS. Users with local HTTP can type http:// explicitly.
    if (strncmp(base_url, "http://", 7) != 0 && strncmp(base_url, "https://", 8) != 0) {
        snprintf(stored_url, sizeof(stored_url), "https://%s", base_url);
        ESP_LOGI(TAG, "No protocol specified — defaulting to HTTPS");
    } else {
        strncpy(stored_url, base_url, sizeof(stored_url) - 1);
        stored_url[sizeof(stored_url) - 1] = '\0';
    }
    normalize_url(stored_url);

    // Store token (may be empty for public instances)
    if (token) {
        strncpy(stored_token, token, sizeof(stored_token) - 1);
        stored_token[sizeof(stored_token) - 1] = '\0';
    } else {
        stored_token[0] = '\0';
    }

    is_https = (strncmp(stored_url, "https", 5) == 0);
    ESP_LOGI(TAG, "Authenticating with Nightscout (%s): %s", is_https ? "HTTPS" : "HTTP", stored_url);

    // Build validation URL: /api/v1/status.json
    char url[256];
    int url_len = build_url(url, sizeof(url), "/api/v1/status.json");
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Validating: %s", url);

    // Close any existing client
    nightscout_close_persistent_client();

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        // Note: do NOT set skip_cert_common_name_check — it disables SNI, which
        // Cloudflare-fronted instances require.
        .keep_alive_enable = true,
    };

    if (!is_https) {
        config.transport_type = HTTP_TRANSPORT_OVER_TCP;
    }

    persistent_client = esp_http_client_init(&config);
    if (!persistent_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_ERR_NO_MEM;
    }

    http_response_len = 0;
    http_response[0] = '\0';

    esp_err_t err = esp_http_client_perform(persistent_client);
    int status = esp_http_client_get_status_code(persistent_client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Status request failed: %s", esp_err_to_name(err));
        nightscout_close_persistent_client();
        return err;
    }

    if (status == 401) {
        ESP_LOGE(TAG, "Authentication failed (401 Unauthorized) — check API token");
        nightscout_close_persistent_client();
        return ESP_ERR_INVALID_STATE;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected status %d from Nightscout", status);
        nightscout_close_persistent_client();
        return ESP_FAIL;
    }

    // HTTP 200 plus a "status" substring is enough to confirm a Nightscout server.
    // Don't parse the full JSON — the status endpoint can exceed the 2KB buffer.
    if (http_response_len == 0 || strstr(http_response, "\"status\"") == NULL) {
        ESP_LOGE(TAG, "Not a Nightscout server (missing 'status' in response)");
        nightscout_close_persistent_client();
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Nightscout server validated (HTTP 200, status field present)");

    is_authenticated = true;
    consecutive_failures = 0;
    sd_log(TAG, "Nightscout auth OK: %s (%s)", stored_url, is_https ? "HTTPS" : "HTTP");
    ESP_LOGI(TAG, "Nightscout authentication successful!");

    return ESP_OK;
}

// ============================================================================
// Glucose Fetch
// ============================================================================

esp_err_t nightscout_fetch_glucose(cgm_glucose_t *glucose) {
    if (!glucose) return ESP_ERR_INVALID_ARG;

    memset(glucose, 0, sizeof(cgm_glucose_t));
    glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;

    if (!is_authenticated) {
        ESP_LOGW(TAG, "Not authenticated");
        return ESP_ERR_INVALID_STATE;
    }

    // Build glucose URL: /api/v1/entries/sgv.json?count=1
    char url[256];
    if (stored_token[0] != '\0') {
        snprintf(url, sizeof(url), "%s/api/v1/entries/sgv.json?count=1&token=%s",
                 stored_url, stored_token);
    } else {
        snprintf(url, sizeof(url), "%s/api/v1/entries/sgv.json?count=1", stored_url);
    }

    ESP_LOGI(TAG, "Fetching glucose data...");
    ESP_LOGI(TAG, "Free heap before glucose fetch: %lu bytes", esp_get_free_heap_size());

    // Create or reuse persistent client
    if (persistent_client == NULL) {
        ESP_LOGI(TAG, "Creating new persistent client");
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .timeout_ms = 15000,
            .buffer_size = 1024,
            .buffer_size_tx = 512,
            // Note: do NOT set skip_cert_common_name_check — it disables SNI, which
            // Cloudflare-fronted instances require.
            .keep_alive_enable = true,
        };
        if (!is_https) {
            config.transport_type = HTTP_TRANSPORT_OVER_TCP;
        }
        persistent_client = esp_http_client_init(&config);
        if (!persistent_client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            glucose->status = GLUCOSE_STATUS_NO_DATA;
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGI(TAG, "Reusing persistent HTTP client");
        esp_http_client_set_url(persistent_client, url);
    }

    http_response_len = 0;
    http_response[0] = '\0';
    esp_http_client_set_method(persistent_client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(persistent_client);
    int status_code = esp_http_client_get_status_code(persistent_client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        consecutive_failures++;
        if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            ESP_LOGW(TAG, "Connection error — closing client for fresh retry");
            nightscout_close_persistent_client();
        }
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return err;
    }

    if (status_code == 401) {
        ESP_LOGE(TAG, "Token rejected (401) — marking unauthenticated");
        is_authenticated = false;
        glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
        nightscout_close_persistent_client();
        return ESP_ERR_INVALID_STATE;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status_code);
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return ESP_FAIL;
    }

    consecutive_failures = 0;

    ESP_LOGI(TAG, "Response: HTTP %d, len=%d", status_code, http_response_len);

    // Parse JSON array: [{"sgv": 142, "date": 1711300200000, "trend": 4, "direction": "Flat"}]
    cJSON *root = cJSON_Parse(http_response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return ESP_FAIL;
    }

    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        ESP_LOGW(TAG, "Empty entries array — no glucose data");
        cJSON_Delete(root);
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return ESP_OK;  // Not an error — just no data
    }

    cJSON *entry = cJSON_GetArrayItem(root, 0);
    if (!entry) {
        cJSON_Delete(root);
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return ESP_OK;
    }

    cJSON *sgv = cJSON_GetObjectItem(entry, "sgv");
    if (!sgv || !cJSON_IsNumber(sgv)) {
        ESP_LOGE(TAG, "Missing 'sgv' field in entry");
        cJSON_Delete(root);
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        return ESP_FAIL;
    }

    glucose->value = sgv->valueint;

    // Extract timestamp (milliseconds → seconds)
    cJSON *date = cJSON_GetObjectItem(entry, "date");
    if (date && cJSON_IsNumber(date)) {
        glucose->timestamp = (time_t)(date->valuedouble / 1000.0);
    }

    // Extract trend — prefer numeric 'trend', fall back to 'direction' string
    cJSON *trend = cJSON_GetObjectItem(entry, "trend");
    if (trend && cJSON_IsNumber(trend)) {
        int t = trend->valueint;
        if (t >= 1 && t <= 9) {
            glucose->trend = (cgm_trend_t)t;
        } else {
            glucose->trend = TREND_NONE;
        }
    } else {
        cJSON *direction = cJSON_GetObjectItem(entry, "direction");
        if (direction && cJSON_IsString(direction)) {
            glucose->trend = map_direction_string(direction->valuestring);
        } else {
            glucose->trend = TREND_NONE;
        }
    }

    cJSON_Delete(root);

    time_t now = time(NULL);
    int age_sec = (int)difftime(now, glucose->timestamp);
    int age_min = age_sec / 60;

    ESP_LOGI(TAG, "Glucose: %d mg/dL, Trend: %s (Status: 0)",
             glucose->value, cgm_trend_description(glucose->trend));

    if (age_sec > 600) {  // >10 minutes old
        ESP_LOGW(TAG, "Data is %d min old — signal loss", age_min);
        glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
        glucose->valid = false;
        return ESP_OK;
    }

    if (glucose->timestamp == last_successful_timestamp && last_successful_timestamp > 0) {
        duplicate_reading_count++;
        ESP_LOGW(TAG, "Duplicate reading #%d (same timestamp %ld)",
                 duplicate_reading_count, (long)glucose->timestamp);
        sd_log(TAG, "Duplicate #%d (ts=%ld)", duplicate_reading_count, (long)glucose->timestamp);
    } else {
        duplicate_reading_count = 0;
        last_successful_timestamp = glucose->timestamp;
    }

    glucose->valid = true;
    glucose->status = GLUCOSE_STATUS_OK;

    sd_log(TAG, "Fetch: %d mg/dL, %s, age=%dmin",
           glucose->value, cgm_trend_description(glucose->trend), age_min);

    return ESP_OK;
}

// ============================================================================
// State Queries
// ============================================================================

bool nightscout_is_authenticated(void) {
    return is_authenticated;
}

bool nightscout_session_needs_refresh(void) {
    return false;  // Tokens are static, never expire
}

void nightscout_logout(void) {
    ESP_LOGI(TAG, "Logging out");
    nightscout_close_persistent_client();
    stored_url[0] = '\0';
    stored_token[0] = '\0';
    is_authenticated = false;
    is_https = false;
    last_successful_timestamp = 0;
    duplicate_reading_count = 0;
    consecutive_failures = 0;
}
