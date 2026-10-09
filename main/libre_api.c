/*
 * LibreLinkUp API Implementation
 *
 * Uses the LibreLinkUp follower API for cloud glucose data from FreeStyle Libre
 * systems. The API is unofficial and community-documented.
 */

#include "libre_api.h"
#include "json_scan.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "features/time_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
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

// HTTP client receive buffer; also the ceiling on one response header line
#define HTTP_BUFFER_SIZE 2048

// Status values the service puts in the response body, beside the HTTP status
#define LLU_STATUS_BAD_CREDENTIALS  2
#define LLU_STATUS_STEP_REQUIRED    4
#define LLU_STATUS_LOCKED_OUT       429
#define LLU_STATUS_VERSION_TOO_OLD  920

// Physiologically possible range; anything outside it is a service artefact.
#define GLUCOSE_MIN_MGDL 20
#define GLUCOSE_MAX_MGDL 600

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

// Response bodies are picked apart as they arrive and never stored: the login
// reply carries the whole account record, whose size the service decides, and
// a reply one byte over a fixed buffer used to fail the sign-in outright.
static json_scan_t response_scan;
static int http_response_len = 0;

// Holds the token of a login in flight, so a failed attempt cannot overwrite
// the one still in use. Static: too large for the calling tasks' stacks, and
// network_mutex serializes every caller.
static char login_token[sizeof(auth_token)];

// Persistent HTTP client for connection reuse
static esp_http_client_handle_t persistent_client = NULL;
static int consecutive_failures = 0;
static bool warned_no_factory_timestamp = false;
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
            json_scan_feed(&response_scan, (const char *)evt->data, evt->data_len);
            http_response_len += evt->data_len;
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

// Split "M/d/yyyy h:mm:ss tt" into its fields, normalised to a 24-hour clock.
// Example: "3/17/2026 2:30:45 PM". Returns false if the string does not match.
static bool split_libre_timestamp(const char *ts_str, struct tm *out) {
    if (ts_str == NULL || out == NULL) return false;

    int month = 0, day = 0, year = 0;
    int hour = 0, min = 0, sec = 0;
    char ampm[4] = {0};

    int parsed = sscanf(ts_str, "%d/%d/%d %d:%d:%d %3s",
                        &month, &day, &year, &hour, &min, &sec, ampm);
    if (parsed < 6) return false;

    if (parsed >= 7) {
        if ((ampm[0] == 'P' || ampm[0] == 'p') && hour != 12) {
            hour += 12;
        } else if ((ampm[0] == 'A' || ampm[0] == 'a') && hour == 12) {
            hour = 0;
        }
    }

    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon = month - 1;
    out->tm_mday = day;
    out->tm_hour = hour;
    out->tm_min = min;
    out->tm_sec = sec;
    return true;
}

// Days from 1970-01-01 to a proleptic-Gregorian civil date. The toolchain offers
// no timegm, and mktime would apply the device's zone to a field that is UTC.
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (long)((y >= 0 ? y : y - 399) / 400);
    long yoe = (long)y - era * 400;                                   // [0, 399]
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       // [0, 365]
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
    return era * 146097 + doe - 719468;
}

// FactoryTimestamp is the same string format but expressed in UTC.
static time_t parse_libre_timestamp_utc(const char *ts_str) {
    struct tm t;
    if (!split_libre_timestamp(ts_str, &t)) return 0;

    // Accumulate wider than time_t: a date the service controls must not be able
    // to wrap into the window the staleness checks accept.
    long days = days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    int64_t secs = (int64_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    if (secs <= 0 || secs > (int64_t)INT32_MAX) return 0;
    return (time_t)secs;
}

// Timestamp carries a local zone the service never names, so it can only be read
// as the device's own zone. Used when the UTC sibling field is absent.
static time_t parse_libre_timestamp_local(const char *ts_str) {
    struct tm t;
    if (!split_libre_timestamp(ts_str, &t)) return 0;

    t.tm_isdst = -1;  // Let mktime determine DST
    time_t result = mktime(&t);
    return (result > 0) ? result : 0;
}

// Returns 0 for anything the staleness and alarm logic must not treat as a reading.
static time_t bound_libre_timestamp(time_t t) {
    // Before SNTP lands, now sits in 1970 and every real reading looks like a
    // decades-ahead skew; the caller reports the unsynced clock instead.
    if (!is_time_synced()) return 0;

    time_t now = time(NULL);

    if (t <= 0) return 0;
    if (t > now + 300) {  // Allow 5 min clock skew
        ESP_LOGW(TAG, "Reading dated %ld s ahead of the device clock — rejecting",
                 (long)(t - now));
        return 0;
    }
    return t;
}

// The region code is interpolated into a hostname and persisted to NVS, so the
// server may only steer us within the vendor's own namespace. Real codes are
// two- and three-letter lowercase ("eu2", "ap", "au").
static bool region_code_is_valid(const char *region) {
    size_t len = strlen(region);
    if (len == 0 || len > 8) return false;

    for (size_t i = 0; i < len; i++) {
        char c = region[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// A base URL restored from NVS may have been written by a build that verified
// neither the TLS peer nor the region code, so it is held to the same namespace
// before it is allowed to steer a request.
static bool region_base_url_is_valid(const char *url) {
    if (strcmp(url, DEFAULT_BASE_URL) == 0) return true;
    if (strcmp(url, "https://api.libreview.ru") == 0) return true;

    static const char prefix[] = "https://api-";
    static const char suffix[] = ".libreview.io";
    if (strncmp(url, prefix, sizeof(prefix) - 1) != 0) return false;

    const char *code = url + sizeof(prefix) - 1;
    const char *tail = strstr(code, suffix);
    if (tail == NULL || tail[sizeof(suffix) - 1] != '\0') return false;

    size_t len = (size_t)(tail - code);
    if (len == 0 || len > 8) return false;
    for (size_t i = 0; i < len; i++) {
        char c = code[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// A picked value counts only if it arrived whole and with the expected type.
static bool pick_is(const json_pick_t *p, json_scan_type_t type) {
    return p->type == type && !p->truncated;
}

// Number text as the service sent it, truncated toward zero. Anything that
// would not fit an int reads as 0, which no caller treats as a usable value.
static int pick_int(const char *text) {
    double v = strtod(text, NULL);
    if (!(v > -2.1e9 && v < 2.1e9)) return 0;
    return (int)v;
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
        // The bearer token rides on this connection; authenticate the server
        // against the bundled roots rather than only encrypting to it.
        .crt_bundle_attach = esp_crt_bundle_attach,
        // Following a redirect would re-send the bearer header, and the request
        // body on the login path, to whatever host the response names.
        .disable_auto_redirect = true,
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
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
        };
        client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to create HTTP client for connections");
            return ESP_FAIL;
        }
    }

    char pid_buf[sizeof(patient_id)];
    char value_buf[16];
    char trend_buf[8];
    char ts_buf[32];
    char fts_buf[32];
    enum { C_DATA, C_FIRST, C_PID, C_VALUE, C_TREND, C_TS, C_FTS, C_COUNT };
    json_pick_t picks[C_COUNT] = {
        [C_DATA]  = { .path = "data" },
        [C_FIRST] = { .path = "data[0]" },
        [C_PID]   = { .path = "data[0].patientId", .out = pid_buf, .out_size = sizeof(pid_buf) },
        [C_VALUE] = { .path = "data[0].glucoseMeasurement.ValueInMgPerDl",
                      .out = value_buf, .out_size = sizeof(value_buf) },
        [C_TREND] = { .path = "data[0].glucoseMeasurement.TrendArrow",
                      .out = trend_buf, .out_size = sizeof(trend_buf) },
        [C_TS]    = { .path = "data[0].glucoseMeasurement.Timestamp",
                      .out = ts_buf, .out_size = sizeof(ts_buf) },
        [C_FTS]   = { .path = "data[0].glucoseMeasurement.FactoryTimestamp",
                      .out = fts_buf, .out_size = sizeof(fts_buf) },
    };
    json_scan_init(&response_scan, picks, C_COUNT);
    http_response_len = 0;

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
        // esp_http_client turns a 401 it cannot answer into
        // ESP_ERR_NOT_SUPPORTED inside perform(), before the caller sees the
        // status line, so the 401 check further down is unreachable without
        // this. A rejected token is the commonest way to land here and it
        // read as an unspecified failure.
        if (esp_http_client_get_status_code(client) == 401) {
            ESP_LOGW(TAG, "Token expired (401) - need re-auth");
            is_authenticated = false;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (using_persistent) persistent_client = NULL;
            return ESP_ERR_INVALID_STATE;
        }
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
        ESP_LOGE(TAG, "Connections failed: HTTP %d", status);
        return ESP_FAIL;
    }

    if (!json_scan_complete(&response_scan)) {
        ESP_LOGE(TAG, "Failed to parse connections JSON");
        return ESP_FAIL;
    }

    // Auto-use first patient connection
    if (picks[C_DATA].type != JSON_SCAN_CONTAINER || picks[C_FIRST].type == JSON_SCAN_NONE) {
        ESP_LOGE(TAG, "No patient connections found");
        return ESP_ERR_NOT_FOUND;
    }

    if (pick_is(&picks[C_PID], JSON_SCAN_STRING)) {
        strncpy(patient_id, pid_buf, sizeof(patient_id) - 1);
        patient_id[sizeof(patient_id) - 1] = '\0';
        ESP_LOGI(TAG, "Patient connection established");
    }

    // Extract glucose measurement if present and caller wants it
    if (glucose_out != NULL && pick_is(&picks[C_VALUE], JSON_SCAN_NUMBER)) {
        glucose_out->value = pick_int(value_buf);
        glucose_out->valid = true;
        glucose_out->status = GLUCOSE_STATUS_OK;

        if (pick_is(&picks[C_TREND], JSON_SCAN_NUMBER)) {
            glucose_out->trend = map_libre_trend(pick_int(trend_buf));
        } else {
            glucose_out->trend = TREND_NONE;
        }

        // FactoryTimestamp is UTC; Timestamp is a local zone the service
        // never names, so it can only be read as the device's own.
        const char *raw_ts = NULL;
        time_t parsed = 0;
        if (pick_is(&picks[C_FTS], JSON_SCAN_STRING)) {
            raw_ts = fts_buf;
            parsed = parse_libre_timestamp_utc(raw_ts);
        }
        // The service owns both formats; falling through keeps a change to
        // the UTC field from taking every reading with it.
        if (parsed == 0 && pick_is(&picks[C_TS], JSON_SCAN_STRING)) {
            if (!warned_no_factory_timestamp) {
                warned_no_factory_timestamp = true;
                ESP_LOGW(TAG, "No usable FactoryTimestamp in reading — reading "
                              "Timestamp as device-local time, which is wrong if "
                              "the sensor is in another zone");
            }
            raw_ts = ts_buf;
            parsed = parse_libre_timestamp_local(raw_ts);
        }
        glucose_out->timestamp = bound_libre_timestamp(parsed);

        if (!is_time_synced()) {
            ESP_LOGW(TAG, "Device clock not synced — rejecting reading");
            glucose_out->timestamp = 0;
            glucose_out->valid = false;
            glucose_out->status = GLUCOSE_STATUS_SIGNAL_LOSS;
        } else if (glucose_out->timestamp == 0) {
            // No usable time means every staleness and alarm-age gate would
            // be measured against a fabricated "now".
            ESP_LOGW(TAG, "Unusable reading timestamp \"%s\" — rejecting reading",
                     raw_ts != NULL ? raw_ts : "(absent)");
            glucose_out->valid = false;
            glucose_out->status = GLUCOSE_STATUS_SIGNAL_LOSS;
        } else if (glucose_out->value < GLUCOSE_MIN_MGDL ||
                   glucose_out->value > GLUCOSE_MAX_MGDL) {
            ESP_LOGW(TAG, "Implausible glucose value %d mg/dL — rejecting",
                     glucose_out->value);
            glucose_out->value = 0;
            glucose_out->timestamp = 0;
            glucose_out->valid = false;
            glucose_out->status = GLUCOSE_STATUS_NO_DATA;
        } else {
            ESP_LOGI(TAG, "Glucose from connections: %d mg/dL, trend=%d",
                     glucose_out->value, glucose_out->trend);
        }
    }

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

    if (saved_region[0] != '\0' && !region_base_url_is_valid(saved_region)) {
        ESP_LOGW(TAG, "Saved server address is outside the service namespace — forcing re-login");
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

    ESP_LOGI(TAG, "Authenticating with LibreLinkUp");

    strncpy(stored_email, email, sizeof(stored_email) - 1);
    stored_email[sizeof(stored_email) - 1] = '\0';
    strncpy(stored_password, password, sizeof(stored_password) - 1);
    stored_password[sizeof(stored_password) - 1] = '\0';

    // Close any existing client
    libre_close_persistent_client();

    // Start with default base URL (may redirect to regional)
    const char *base_url = DEFAULT_BASE_URL;
    bool redirected = false;

    char status_buf[12];
    char redirect_buf[8];
    char region_buf[12];
    char step_buf[24];
    char min_version_buf[16];
    char expires_buf[24];
    char user_id_buf[48];
    enum { L_STATUS, L_DATA, L_REDIRECT, L_REGION, L_STEP, L_STEP_TYPE, L_MIN_VERSION,
           L_TOKEN, L_EXPIRES, L_USER_ID, L_COUNT };
    json_pick_t picks[L_COUNT] = {
        [L_STATUS]      = { .path = "status", .out = status_buf, .out_size = sizeof(status_buf) },
        [L_DATA]        = { .path = "data" },
        [L_REDIRECT]    = { .path = "data.redirect", .out = redirect_buf, .out_size = sizeof(redirect_buf) },
        [L_REGION]      = { .path = "data.region", .out = region_buf, .out_size = sizeof(region_buf) },
        [L_STEP]        = { .path = "data.step" },
        [L_STEP_TYPE]   = { .path = "data.step.type", .out = step_buf, .out_size = sizeof(step_buf) },
        [L_MIN_VERSION] = { .path = "data.minimumVersion",
                            .out = min_version_buf, .out_size = sizeof(min_version_buf) },
        [L_TOKEN]       = { .path = "data.authTicket.token",
                            .out = login_token, .out_size = sizeof(login_token) },
        [L_EXPIRES]     = { .path = "data.authTicket.expires",
                            .out = expires_buf, .out_size = sizeof(expires_buf) },
        [L_USER_ID]     = { .path = "data.user.id", .out = user_id_buf, .out_size = sizeof(user_id_buf) },
    };

auth_attempt:
    ;  // Label requires a statement

    char url[128];
    snprintf(url, sizeof(url), "%s%s", base_url, ENDPOINT_LOGIN);

    // Credentials may contain quotes or backslashes, so the body is serialized
    // rather than formatted; its length is whatever NVS holds, not a fixed cap.
    cJSON *login_body = cJSON_CreateObject();
    if (login_body == NULL) {
        ESP_LOGE(TAG, "Out of memory building login request");
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(login_body, "email", email) == NULL ||
        cJSON_AddStringToObject(login_body, "password", password) == NULL) {
        cJSON_Delete(login_body);
        ESP_LOGE(TAG, "Out of memory building login request");
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(login_body);
    cJSON_Delete(login_body);
    if (body == NULL) {
        ESP_LOGE(TAG, "Out of memory building login request");
        return ESP_ERR_NO_MEM;
    }

    json_scan_init(&response_scan, picks, L_COUNT);
    http_response_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = HTTP_BUFFER_SIZE,
        .buffer_size_tx = 512,
        // The account email and password are in this request body.
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        cJSON_free(body);
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
        // esp_http_client turns a 401 it cannot answer into
        // ESP_ERR_NOT_SUPPORTED inside perform(), before the caller sees the
        // status line, so the 401 check further down is unreachable without
        // this. A rejected token is the commonest way to land here and it
        // read as an unspecified failure.
        if (esp_http_client_get_status_code(client) == 401) {
            ESP_LOGE(TAG, "Invalid credentials (401)");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            cJSON_free(body);
            return ESP_ERR_INVALID_STATE;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        cJSON_free(body);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Login response: HTTP %d, len=%d", status, http_response_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    int api_status = pick_is(&picks[L_STATUS], JSON_SCAN_NUMBER)
                         ? pick_int(status_buf) : 0;

    // The service reports these in the body, and not always with a matching
    // HTTP status, so they are read before either is trusted.
    if (api_status == LLU_STATUS_BAD_CREDENTIALS || status == 401) {
        ESP_LOGE(TAG, "Invalid credentials");
        return ESP_ERR_INVALID_STATE;
    }
    if (api_status == LLU_STATUS_VERSION_TOO_OLD) {
        ESP_LOGE(TAG, "Service requires a newer client version (minimum %s, sent %s)",
                 pick_is(&picks[L_MIN_VERSION], JSON_SCAN_STRING) ? min_version_buf : "unknown",
                 LLU_VERSION);
        sd_log(TAG, "Auth blocked: service requires a newer client version");
        return ESP_ERR_INVALID_VERSION;
    }
    if (api_status == LLU_STATUS_LOCKED_OUT || status == 429) {
        ESP_LOGW(TAG, "Rate limited — try again later");
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Login failed: HTTP %d, service status %d", status, api_status);
        return ESP_FAIL;
    }

    if (!json_scan_complete(&response_scan)) {
        ESP_LOGE(TAG, "Failed to parse login JSON");
        return ESP_FAIL;
    }

    // Terms of use, a privacy policy or an unverified email. The reply to these
    // still carries an authTicket, but it is a short-lived one that only the
    // vendor's own app can use to finish the step, so it must not be kept.
    if (api_status == LLU_STATUS_STEP_REQUIRED || picks[L_STEP].type != JSON_SCAN_NONE) {
        const char *step = pick_is(&picks[L_STEP_TYPE], JSON_SCAN_STRING) ? step_buf : "unknown";
        ESP_LOGE(TAG, "Account needs a step completed in the LibreLinkUp app first (%s)", step);
        ESP_LOGE(TAG, "Open the LibreLinkUp app, accept or verify what it asks for, then retry");
        sd_log(TAG, "Auth blocked: account step required (%s)", step);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (api_status != 0) {
        ESP_LOGE(TAG, "Login failed: service status %d", api_status);
        return ESP_FAIL;
    }

    if (picks[L_DATA].type == JSON_SCAN_NONE) {
        ESP_LOGE(TAG, "No 'data' in login response");
        return ESP_FAIL;
    }

    // Check for regional redirect
    if (pick_is(&picks[L_REDIRECT], JSON_SCAN_BOOL) && strcmp(redirect_buf, "true") == 0 &&
        !redirected) {
        if (pick_is(&picks[L_REGION], JSON_SCAN_STRING) && region_code_is_valid(region_buf)) {
            ESP_LOGI(TAG, "Redirecting to region: %s", region_buf);

            if (strcmp(region_buf, "ru") == 0) {
                snprintf(region_base_url, sizeof(region_base_url),
                         "https://api.libreview.ru");
            } else {
                snprintf(region_base_url, sizeof(region_base_url),
                         "https://api-%s.libreview.io", region_buf);
            }

            base_url = region_base_url;
            redirected = true;
            goto auth_attempt;
        }
        if (picks[L_REGION].type == JSON_SCAN_STRING) {
            ESP_LOGE(TAG, "Server asked to redirect to an unrecognised region code");
            return ESP_FAIL;
        }
    }

    if (picks[L_TOKEN].type != JSON_SCAN_STRING || login_token[0] == '\0') {
        ESP_LOGE(TAG, "No token in login response");
        return ESP_FAIL;
    }
    if (picks[L_TOKEN].truncated) {
        ESP_LOGE(TAG, "Session token is longer than the %d bytes reserved for it",
                 (int)sizeof(login_token) - 1);
        return ESP_FAIL;
    }

    strncpy(auth_token, login_token, sizeof(auth_token) - 1);
    auth_token[sizeof(auth_token) - 1] = '\0';

    // Store expiry (Unix epoch seconds)
    if (pick_is(&picks[L_EXPIRES], JSON_SCAN_NUMBER)) {
        token_expires = (int32_t)pick_int(expires_buf);
    } else {
        // Default: 180 days from now
        token_expires = (int32_t)time(NULL) + (180 * 24 * 3600);
    }

    // The account-id header on every authenticated request is the SHA-256 of the user ID.
    if (pick_is(&picks[L_USER_ID], JSON_SCAN_STRING)) {
        sha256_hex(user_id_buf, account_id_hash, sizeof(account_id_hash));
        ESP_LOGI(TAG, "Account header derived");
    }

    // If we didn't redirect, set region URL to default
    if (!redirected) {
        strncpy(region_base_url, DEFAULT_BASE_URL, sizeof(region_base_url) - 1);
    }

    ESP_LOGI(TAG, "Login successful (region=%s)", region_base_url);

    // Mark authenticated before fetching connections
    is_authenticated = true;
    consecutive_failures = 0;

    // Connections fetch deferred to glucose task — heap too fragmented here
    // for a second TLS handshake. libre_fetch_glucose() handles missing patient_id.

    // Save session to NVS for persistence across reboots
    nvs_save_libre_session(auth_token, patient_id, region_base_url, token_expires, account_id_hash);

    sd_log(TAG, "Login successful (region=%s)", region_base_url);

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
            // A reading rejected during parsing already carries its own reason.
            if (glucose->status != GLUCOSE_STATUS_SIGNAL_LOSS) {
                glucose->status = GLUCOSE_STATUS_NO_DATA;
            }
        } else {
            // Check staleness (>10 minutes old)
            time_t now = time(NULL);
            if (glucose->timestamp > 0 && (now - glucose->timestamp) > 600) {
                ESP_LOGW(TAG, "Data is %ld seconds old (>10min)",
                         (long)(now - glucose->timestamp));
                glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
                glucose->valid = false;
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
