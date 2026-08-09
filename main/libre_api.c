/*
 * LibreLinkUp API Implementation
 *
 * Uses the LibreLinkUp follower API for cloud glucose data from FreeStyle Libre
 * systems. The API is unofficial and community-documented.
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

// Map LibreLinkUp TrendArrow (1-5) to cgm_trend_t. Libre has no "double" tier, so
// its steepest arrow (>2 mg/dL/min) maps onto the "single" tier.
static cgm_trend_t map_libre_trend(int trend_arrow) {
    switch (trend_arrow) {
        case 1: return TREND_SINGLE_DOWN;      // Falling
        case 2: return TREND_FORTY_FIVE_DOWN;  // Falling slowly
        case 3: return TREND_FLAT;             // Stable
        case 4: return TREND_FORTY_FIVE_UP;    // Rising slowly
        case 5: return TREND_SINGLE_UP;        // Rising
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

    time_t now = time(NULL);
    if (result <= 0 || result > now + 86400) {
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
        esp_http_client_close(persistent_client);
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
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            persistent_client = NULL;
        } else {
            esp_http_client_close(client);
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
                    if (glucose_out->timestamp == 0) {
                        ESP_LOGW(TAG, "Timestamp parse failed — using current time as fallback");
                        glucose_out->timestamp = time(NULL);
                    }
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
// Session Restore from NVS
// ============================================================================

esp_err_t libre_restore_session(void) {
    ESP_LOGI(TAG, "Attempting to restore Libre session from NVS...");

    char saved_token[1500] = {0};
    char saved_pid[40] = {0};
    char saved_region[64] = {0};
    int32_t saved_expires = 0;
    char saved_acct[65] = {0};

    esp_err_t ret = nvs_load_libre_session(saved_token, sizeof(saved_token),
                                            saved_pid, sizeof(saved_pid),
                                            saved_region, sizeof(saved_region),
                                            &saved_expires,
                                            saved_acct, sizeof(saved_acct));
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved Libre session in NVS");
        return ESP_FAIL;
    }

    if (saved_token[0] == '\0') {
        ESP_LOGW(TAG, "Saved token is empty");
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    if (saved_expires > 0 && (int32_t)now >= saved_expires) {
        ESP_LOGW(TAG, "Saved token expired (%ld >= %ld)", (long)now, (long)saved_expires);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Restoring Libre session from NVS (expires in %ld hours)",
             (long)(saved_expires - (int32_t)now) / 3600);

    strncpy(auth_token, saved_token, sizeof(auth_token) - 1);
    auth_token[sizeof(auth_token) - 1] = '\0';
    strncpy(patient_id, saved_pid, sizeof(patient_id) - 1);
    patient_id[sizeof(patient_id) - 1] = '\0';
    strncpy(region_base_url, saved_region, sizeof(region_base_url) - 1);
    region_base_url[sizeof(region_base_url) - 1] = '\0';
    if (saved_acct[0] != '\0') {
        strncpy(account_id_hash, saved_acct, sizeof(account_id_hash) - 1);
        account_id_hash[sizeof(account_id_hash) - 1] = '\0';
    }
    token_expires = saved_expires;
    is_authenticated = true;
    consecutive_failures = 0;

    // account_id_hash is required by the connections endpoint. If it is missing —
    // e.g. first boot after an OTA from older firmware — force a re-login rather
    // than looping on HTTP 400, which never triggers re-auth.
    if (account_id_hash[0] == '\0') {
        ESP_LOGW(TAG, "No account_id_hash in NVS — forcing re-login");
        is_authenticated = false;
        memset(auth_token, 0, sizeof(auth_token));
        return ESP_FAIL;
    }

    // Verify token with a test fetch (also gets initial glucose)
    ESP_LOGI(TAG, "Verifying restored session with test fetch...");
    esp_err_t fetch_ret = libre_fetch_connections(NULL);

    if (fetch_ret == ESP_ERR_INVALID_STATE) {
        // 401 — token rejected by server
        ESP_LOGW(TAG, "Restored token rejected (401) — need full re-auth");
        is_authenticated = false;
        memset(auth_token, 0, sizeof(auth_token));
        libre_close_persistent_client();
        return ESP_FAIL;
    }

    if (fetch_ret != ESP_OK) {
        // Network error or other failure — don't invalidate token, might be transient
        ESP_LOGW(TAG, "Test fetch failed (%s) — session restored but unverified",
                 esp_err_to_name(fetch_ret));
        // Keep is_authenticated = true; glucose_update_task will retry
    } else {
        ESP_LOGI(TAG, "Session restored and verified successfully");
        sd_log(TAG, "Session restored from NVS (skip login)");
    }

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

    char url[128];
    snprintf(url, sizeof(url), "%s%s", base_url, ENDPOINT_LOGIN);

    char body[160];
    snprintf(body, sizeof(body), "{\"email\":\"%s\",\"password\":\"%s\"}", email, password);

    http_response_len = 0;
    http_response[0] = '\0';

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
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Login response: HTTP %d, len=%d", status, http_response_len);
    esp_http_client_close(client);
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

    // LibreLinkUp can return HTTP 200 with no authTicket when its Terms of Use need
    // accepting. That cannot be automated, so the user must open the vendor's app.
    cJSON *auth_ticket = cJSON_GetObjectItem(data, "authTicket");
    if (auth_ticket == NULL || !cJSON_IsObject(auth_ticket)) {
        cJSON *step = cJSON_GetObjectItem(data, "step");
        if (step != NULL && cJSON_IsNumber(step)) {
            ESP_LOGE(TAG, "Terms of Use acceptance required (step=%d)", step->valueint);
            ESP_LOGE(TAG, "Open the LibreLinkUp app and accept any pending terms, then retry");
            sd_log(TAG, "Auth blocked: TOU acceptance required (step=%d)", step->valueint);
        } else {
            ESP_LOGE(TAG, "No authTicket in login response — Terms of Use may need acceptance");
            ESP_LOGE(TAG, "Open the LibreLinkUp app and accept any pending terms, then retry");
        }
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

    strncpy(auth_token, token_obj->valuestring, sizeof(auth_token) - 1);
    auth_token[sizeof(auth_token) - 1] = '\0';

    // Store expiry (Unix epoch seconds)
    if (cJSON_IsNumber(expires_obj)) {
        token_expires = (int32_t)expires_obj->valuedouble;
    } else {
        // Default: 180 days from now
        token_expires = (int32_t)time(NULL) + (180 * 24 * 3600);
    }

    // The account-id header on every authenticated request is the SHA-256 of the user ID.
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

    // Connections fetch deferred to glucose task — heap too fragmented here
    // for a second TLS handshake. libre_fetch_glucose() handles missing patient_id.

    // Save session to NVS for persistence across reboots
    nvs_save_libre_session(auth_token, patient_id, region_base_url, token_expires, account_id_hash);

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

    nvs_clear_libre_session();

    ESP_LOGI(TAG, "Logged out — session cleared");
}
