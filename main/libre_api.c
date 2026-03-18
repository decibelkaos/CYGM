/*
 * LibreLinkUp API Implementation
 *
 * Uses the LibreLinkUp follower API for cloud glucose data from
 * FreeStyle Libre CGM systems. Based on community research of the
 * unofficial LibreLinkUp API (libreview.io).
 */

#include "libre_api.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"
#include "mbedtls/sha256.h"

static const char *TAG = "LIBRE";

// ============================================================================
// API Configuration
// ============================================================================

// LibreLinkUp API headers (required for all requests)
#define LLU_PRODUCT     "llu.android"
#define LLU_VERSION     "4.16.0"

// Default base URL (US region)
#define DEFAULT_BASE_URL "https://api.libreview.io"

// Endpoints
#define ENDPOINT_LOGIN       "/llu/auth/login"
#define ENDPOINT_CONNECTIONS "/llu/connections"

// HTTP buffer size
#define HTTP_BUFFER_SIZE 2048

// ============================================================================
// State Variables
// ============================================================================

static char stored_email[64] = {0};
static char stored_password[64] = {0};
static char auth_token[1500] = {0};
static char patient_id[40] = {0};
static char region_base_url[64] = {0};
static char account_id_hash[65] = {0};  // SHA-256 hex of user ID (required header)
static int32_t token_expires = 0;
static bool is_authenticated = false;

// HTTP response buffer
static char http_response[HTTP_BUFFER_SIZE];
static int http_response_len = 0;

// Persistent HTTP client for connection reuse
static esp_http_client_handle_t persistent_client = NULL;
static int consecutive_failures = 0;
#define MAX_CONSECUTIVE_FAILURES 3

// ============================================================================
// Helper Functions
// ============================================================================

static const char* get_base_url(void) {
    if (region_base_url[0] != '\0') {
        return region_base_url;
    }
    return DEFAULT_BASE_URL;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(http_response + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response[http_response_len] = '\0';
            } else {
                ESP_LOGE(TAG, "HTTP response buffer overflow! (%d + %d >= %d)",
                         http_response_len, evt->data_len, HTTP_BUFFER_SIZE);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Map LibreLinkUp TrendArrow (1-5) to cgm_trend_t
static cgm_trend_t map_libre_trend(int trend_arrow) {
    switch (trend_arrow) {
        case 1: return TREND_DOUBLE_DOWN;      // Falling quickly
        case 2: return TREND_FORTY_FIVE_DOWN;  // Falling
        case 3: return TREND_FLAT;             // Stable
        case 4: return TREND_FORTY_FIVE_UP;    // Rising
        case 5: return TREND_DOUBLE_UP;        // Rising quickly
        default: return TREND_NONE;
    }
}

// Compute SHA-256 hex digest of a string (for account-id header)
static void sha256_hex(const char *input, char *output, size_t output_len) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)input, strlen(input), hash, 0);
    for (int i = 0; i < 32 && (i * 2 + 2) < (int)output_len; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[64] = '\0';
}

// Parse LibreLinkUp timestamp: "M/d/yyyy h:mm:ss tt" -> time_t
// Example: "3/17/2026 2:30:45 PM"
static time_t parse_libre_timestamp(const char *ts_str) {
    if (ts_str == NULL) return 0;

    int month = 0, day = 0, year = 0;
    int hour = 0, min = 0, sec = 0;
    char ampm[4] = {0};

    int parsed = sscanf(ts_str, "%d/%d/%d %d:%d:%d %3s",
                        &month, &day, &year, &hour, &min, &sec, ampm);

    if (parsed < 6) {
        ESP_LOGW(TAG, "Failed to parse timestamp: %s (fields=%d)", ts_str, parsed);
        return 0;
    }

    // 12h to 24h conversion (only if AM/PM was parsed)
    if (parsed >= 7) {
        if ((ampm[0] == 'P' || ampm[0] == 'p') && hour != 12) {
            hour += 12;
        } else if ((ampm[0] == 'A' || ampm[0] == 'a') && hour == 12) {
            hour = 0;
        }
    }

    struct tm tm_time = {0};
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = month - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = min;
    tm_time.tm_sec = sec;
    tm_time.tm_isdst = -1;  // Let mktime determine DST

    time_t result = mktime(&tm_time);

    // Sanity check
    time_t now = time(NULL);
    if (result <= 0 || result > now + 300) {
        ESP_LOGW(TAG, "Parsed timestamp %ld seems invalid (now=%ld)", (long)result, (long)now);
        return 0;
    }

    return result;
}

// ============================================================================
// Persistent Client Management
// ============================================================================

bool libre_persistent_client_is_open(void) {
    return (persistent_client != NULL);
}

void libre_close_persistent_client(void) {
    if (persistent_client != NULL) {
        ESP_LOGI(TAG, "Closing persistent HTTP client");
        esp_http_client_cleanup(persistent_client);
        persistent_client = NULL;
        ESP_LOGI(TAG, "Free heap after closing Libre client: %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
    }
}

esp_err_t libre_reopen_persistent_client(void) {
    if (persistent_client != NULL) {
        return ESP_OK;
    }

    if (!is_authenticated) {
        ESP_LOGW(TAG, "Cannot reopen client — not authenticated");
        return ESP_ERR_INVALID_STATE;
    }

    // Build connections URL
    char url[128];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_CONNECTIONS);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = HTTP_BUFFER_SIZE,
        .buffer_size_tx = 2048,  // JWT Bearer header is ~1500 bytes
    };

    persistent_client = esp_http_client_init(&config);
    if (persistent_client == NULL) {
        ESP_LOGE(TAG, "Failed to create persistent client");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Persistent client re-opened");
    return ESP_OK;
}

// ============================================================================
// Internal: Fetch patient connections (also returns latest glucose)
// ============================================================================

static esp_err_t libre_fetch_connections(cgm_glucose_t *glucose_out) {
    char url[128];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_CONNECTIONS);

    // Build auth header — MUST be static to avoid 1550 bytes on the
    // glucose_update_task's 4KB stack. Safe: network_mutex serializes all callers.
    static char auth_header[1550];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);

    // Use persistent client if available, else create new
    esp_http_client_handle_t client = persistent_client;
    bool using_persistent = (client != NULL);

    if (!using_persistent) {
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .timeout_ms = 15000,
            .keep_alive_enable = true,
            .buffer_size = HTTP_BUFFER_SIZE,
            .buffer_size_tx = 2048,  // JWT Bearer header is ~1500 bytes
        };
        client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to create HTTP client for connections");
            return ESP_FAIL;
        }
    }

    // Reset response buffer
    http_response_len = 0;
    http_response[0] = '\0';

    // Configure request — all headers required by LibreLinkUp API
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "product", LLU_PRODUCT);
    esp_http_client_set_header(client, "version", LLU_VERSION);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "cache-control", "no-cache");
    if (account_id_hash[0] != '\0') {
        esp_http_client_set_header(client, "account-id", account_id_hash);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connections request failed: %s", esp_err_to_name(err));
        if (using_persistent) {
            // Connection died — clean up and let caller retry
            esp_http_client_cleanup(client);
            persistent_client = NULL;
        } else {
            esp_http_client_cleanup(client);
        }
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Connections response: HTTP %d, len=%d", status, http_response_len);

    // Save as persistent client if new
    if (!using_persistent) {
        persistent_client = client;
    }

    if (status == 401) {
        ESP_LOGW(TAG, "Token expired (401) — need re-auth");
        is_authenticated = false;
        return ESP_ERR_INVALID_STATE;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Connections failed: HTTP %d — %s", status, http_response);
        return ESP_FAIL;
    }

    // Parse response
    cJSON *root = cJSON_Parse(http_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse connections JSON");
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) == 0) {
        ESP_LOGE(TAG, "No patient connections found");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    // Auto-use first patient connection
    cJSON *connection = cJSON_GetArrayItem(data, 0);
    if (connection == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    // Extract patientId
    cJSON *pid = cJSON_GetObjectItem(connection, "patientId");
    if (cJSON_IsString(pid) && pid->valuestring != NULL) {
        strncpy(patient_id, pid->valuestring, sizeof(patient_id) - 1);
        patient_id[sizeof(patient_id) - 1] = '\0';
        ESP_LOGI(TAG, "Patient ID: %s", patient_id);
    }

    // Extract glucose measurement if present and caller wants it
    if (glucose_out != NULL) {
        cJSON *gm = cJSON_GetObjectItem(connection, "glucoseMeasurement");
        if (gm != NULL) {
            cJSON *value = cJSON_GetObjectItem(gm, "ValueInMgPerDl");
            cJSON *trend = cJSON_GetObjectItem(gm, "TrendArrow");
            cJSON *ts = cJSON_GetObjectItem(gm, "Timestamp");

            if (cJSON_IsNumber(value)) {
                glucose_out->value = value->valueint;
                glucose_out->valid = true;
                glucose_out->status = GLUCOSE_STATUS_OK;

                if (cJSON_IsNumber(trend)) {
                    glucose_out->trend = map_libre_trend(trend->valueint);
                } else {
                    glucose_out->trend = TREND_NONE;
                }

                if (cJSON_IsString(ts) && ts->valuestring != NULL) {
                    glucose_out->timestamp = parse_libre_timestamp(ts->valuestring);
                } else {
                    glucose_out->timestamp = time(NULL);
                }

                ESP_LOGI(TAG, "Glucose from connections: %d mg/dL, trend=%d",
                         glucose_out->value, glucose_out->trend);
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// Authentication
// ============================================================================

esp_err_t libre_authenticate(const char *email, const char *password) {
    if (email == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Authenticating with LibreLinkUp (email: %s)", email);

    // Store credentials
    strncpy(stored_email, email, sizeof(stored_email) - 1);
    stored_email[sizeof(stored_email) - 1] = '\0';
    strncpy(stored_password, password, sizeof(stored_password) - 1);
    stored_password[sizeof(stored_password) - 1] = '\0';

    // Close any existing client
    libre_close_persistent_client();

    // Start with default base URL (may redirect to regional)
    const char *base_url = DEFAULT_BASE_URL;
    bool redirected = false;

auth_attempt:
    ;  // Label requires a statement

    // Build login URL
    char url[128];
    snprintf(url, sizeof(url), "%s%s", base_url, ENDPOINT_LOGIN);

    // Build JSON body
    char body[160];
    snprintf(body, sizeof(body), "{\"email\":\"%s\",\"password\":\"%s\"}", email, password);

    // Reset response buffer
    http_response_len = 0;
    http_response[0] = '\0';

    // Create HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = HTTP_BUFFER_SIZE,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    // Set headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "product", LLU_PRODUCT);
    esp_http_client_set_header(client, "version", LLU_VERSION);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "POST %s", url);
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Login request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Login response: HTTP %d, len=%d", status, http_response_len);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Login failed: HTTP %d", status);
        if (status == 401) {
            ESP_LOGE(TAG, "Invalid credentials");
        } else if (status == 429) {
            ESP_LOGW(TAG, "Rate limited — try again later");
        }
        return ESP_FAIL;
    }

    // Parse login response
    cJSON *root = cJSON_Parse(http_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse login JSON");
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL) {
        ESP_LOGE(TAG, "No 'data' in login response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Check for regional redirect
    cJSON *redirect = cJSON_GetObjectItem(data, "redirect");
    if (cJSON_IsTrue(redirect) && !redirected) {
        cJSON *region = cJSON_GetObjectItem(data, "region");
        if (cJSON_IsString(region) && region->valuestring != NULL) {
            ESP_LOGI(TAG, "Redirecting to region: %s", region->valuestring);

            // Build regional URL
            if (strcmp(region->valuestring, "ru") == 0) {
                snprintf(region_base_url, sizeof(region_base_url),
                         "https://api.libreview.ru");
            } else {
                snprintf(region_base_url, sizeof(region_base_url),
                         "https://api-%s.libreview.io", region->valuestring);
            }

            base_url = region_base_url;
            redirected = true;
            cJSON_Delete(root);

            // Reset and retry with regional URL
            http_response_len = 0;
            http_response[0] = '\0';
            goto auth_attempt;
        }
    }

    // Extract auth ticket
    cJSON *auth_ticket = cJSON_GetObjectItem(data, "authTicket");
    if (auth_ticket == NULL) {
        ESP_LOGE(TAG, "No authTicket in login response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token_obj = cJSON_GetObjectItem(auth_ticket, "token");
    cJSON *expires_obj = cJSON_GetObjectItem(auth_ticket, "expires");

    if (!cJSON_IsString(token_obj) || token_obj->valuestring == NULL) {
        ESP_LOGE(TAG, "No token in authTicket");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Store token
    strncpy(auth_token, token_obj->valuestring, sizeof(auth_token) - 1);
    auth_token[sizeof(auth_token) - 1] = '\0';

    // Store expiry (Unix epoch seconds)
    if (cJSON_IsNumber(expires_obj)) {
        token_expires = (int32_t)expires_obj->valuedouble;
    } else {
        // Default: 180 days from now
        token_expires = (int32_t)time(NULL) + (180 * 24 * 3600);
    }

    // Extract user ID and compute SHA-256 hash for account-id header
    // Required by LibreLinkUp API on all authenticated requests
    cJSON *user_obj = cJSON_GetObjectItem(data, "user");
    if (user_obj != NULL) {
        cJSON *user_id = cJSON_GetObjectItem(user_obj, "id");
        if (cJSON_IsString(user_id) && user_id->valuestring != NULL) {
            sha256_hex(user_id->valuestring, account_id_hash, sizeof(account_id_hash));
            ESP_LOGI(TAG, "Account ID hash: %.16s...", account_id_hash);
        }
    }

    // If we didn't redirect, set region URL to default
    if (!redirected) {
        strncpy(region_base_url, DEFAULT_BASE_URL, sizeof(region_base_url) - 1);
    }

    ESP_LOGI(TAG, "Login successful — token expires %ld", (long)token_expires);

    cJSON_Delete(root);

    // Mark authenticated before fetching connections
    is_authenticated = true;
    consecutive_failures = 0;

    // Fetch patient connections (also gets initial glucose)
    esp_err_t conn_ret = libre_fetch_connections(NULL);
    if (conn_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to fetch connections: %s", esp_err_to_name(conn_ret));
        // Auth succeeded but connections failed — still consider authenticated
        // Glucose fetch will retry connections
    }

    // Save session to NVS for persistence across reboots
    nvs_save_libre_session(auth_token, patient_id, region_base_url, token_expires);

    sd_log(TAG, "Auth OK email=%s region=%s patient=%s",
           stored_email, region_base_url, patient_id);

    return ESP_OK;
}

// ============================================================================
// Glucose Fetch
// ============================================================================

esp_err_t libre_fetch_glucose(cgm_glucose_t *glucose) {
    if (glucose == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(glucose, 0, sizeof(cgm_glucose_t));
    glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;

    if (!is_authenticated) {
        ESP_LOGW(TAG, "Not authenticated");
        return ESP_ERR_INVALID_STATE;
    }

    // Check for patient ID
    if (patient_id[0] == '\0') {
        ESP_LOGW(TAG, "No patient ID — fetching connections first");
        esp_err_t ret = libre_fetch_connections(glucose);
        if (ret != ESP_OK || patient_id[0] == '\0') {
            ESP_LOGE(TAG, "Failed to get patient connection");
            glucose->status = GLUCOSE_STATUS_NO_DATA;
            return ret != ESP_OK ? ret : ESP_FAIL;
        }
        // If connections already gave us glucose, return it
        if (glucose->valid) {
            return ESP_OK;
        }
    }

    // Fetch glucose via connections endpoint (smaller response than /graph)
    esp_err_t ret = libre_fetch_connections(glucose);

    if (ret == ESP_ERR_INVALID_STATE) {
        // Token expired — attempt re-auth
        ESP_LOGW(TAG, "Token expired during fetch — re-authenticating");
        libre_close_persistent_client();

        ret = libre_authenticate(stored_email, stored_password);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Re-auth failed");
            glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
            return ret;
        }

        // Retry fetch after re-auth
        memset(glucose, 0, sizeof(cgm_glucose_t));
        ret = libre_fetch_connections(glucose);
    }

    if (ret == ESP_OK) {
        consecutive_failures = 0;

        if (!glucose->valid) {
            glucose->status = GLUCOSE_STATUS_NO_DATA;
        } else {
            // Check staleness (>10 minutes old)
            time_t now = time(NULL);
            if (glucose->timestamp > 0 && (now - glucose->timestamp) > 600) {
                ESP_LOGW(TAG, "Data is %ld seconds old (>10min)",
                         (long)(now - glucose->timestamp));
                glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
            }
        }
    } else {
        consecutive_failures++;
        ESP_LOGW(TAG, "Fetch failed (%d consecutive)", consecutive_failures);

        if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            ESP_LOGW(TAG, "Too many failures — closing client for fresh connection");
            libre_close_persistent_client();
            consecutive_failures = 0;
        }

        glucose->status = GLUCOSE_STATUS_NO_DATA;
    }

    return ret;
}

// ============================================================================
// Session Management
// ============================================================================

bool libre_is_authenticated(void) {
    return is_authenticated;
}

bool libre_session_needs_refresh(void) {
    if (!is_authenticated || token_expires == 0) {
        return false;
    }

    time_t now = time(NULL);
    // Refresh if within 24 hours of expiry
    return ((int32_t)now >= (token_expires - 86400));
}

void libre_logout(void) {
    ESP_LOGI(TAG, "Logging out");
    libre_close_persistent_client();

    memset(stored_email, 0, sizeof(stored_email));
    memset(stored_password, 0, sizeof(stored_password));
    memset(auth_token, 0, sizeof(auth_token));
    memset(patient_id, 0, sizeof(patient_id));
    memset(region_base_url, 0, sizeof(region_base_url));
    memset(account_id_hash, 0, sizeof(account_id_hash));
    token_expires = 0;
    is_authenticated = false;
    consecutive_failures = 0;

    // Clear NVS session
    nvs_clear_libre_session();

    ESP_LOGI(TAG, "Logged out — session cleared");
}
