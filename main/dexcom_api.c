/*
 * Dexcom Share API Implementation
 *
 * Uses the unofficial Dexcom Share API for direct username/password login.
 * The endpoints are community-documented, not officially supported.
 */

#include "dexcom_api.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "glucose_history.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"

static const char *TAG = "DEXCOM";

// ============================================================================
// API Configuration
// ============================================================================

// Application IDs (from pydexcom)
#define APP_ID_US_OUS   "d89443d2-327c-4a6f-89e5-496bbb0317db"
#define APP_ID_JP       "d8665ade-9673-4e27-9ff6-92db4ce13d13"

// Base URLs for each region
static const char *BASE_URL_US  = "https://share2.dexcom.com/ShareWebServices/Services";
static const char *BASE_URL_OUS = "https://shareous1.dexcom.com/ShareWebServices/Services";
static const char *BASE_URL_JP  = "https://share.dexcom.jp/ShareWebServices/Services";

// Endpoints
#define ENDPOINT_AUTH_ACCOUNT   "/General/AuthenticatePublisherAccount"
#define ENDPOINT_LOGIN_BY_ID    "/General/LoginPublisherAccountById"
#define ENDPOINT_LOGIN_BY_NAME  "/General/LoginPublisherAccountByName"  // Fallback for older accounts
#define ENDPOINT_GLUCOSE        "/Publisher/ReadPublisherLatestGlucoseValues"

// HTTP buffer size
#define HTTP_BUFFER_SIZE 2048

// Rate limiting and session timeout
#define RATE_LIMIT_MS       90000ULL    // 90 seconds between glucose requests
#define SESSION_TIMEOUT_MS  43200000ULL // 12 hours
#define SESSION_REFRESH_MS  7200000ULL  // Proactive re-auth every 2 hours

// ============================================================================
// State Variables
// ============================================================================

static dexcom_region_t current_region = DEXCOM_REGION_US;
static char account_id[64] = {0};
static char session_id[64] = {0};
static char stored_username[64] = {0};
static char stored_password[64] = {0};
static bool is_authenticated = false;
static uint64_t last_login_time = 0;
static uint64_t dexcom_last_fetch_ms = 0;

// HTTP response buffer
static char http_response[HTTP_BUFFER_SIZE];
static int http_response_len = 0;

// Persistent HTTP client for connection reuse (saves SSL handshake overhead)
static esp_http_client_handle_t persistent_client = NULL;
static int consecutive_failures = 0;  // Track consecutive fetch failures
#define MAX_CONSECUTIVE_FAILURES 3  // Force fresh connection after 3 failures (memory-conscious)

// Re-auth cooldown. During a Dexcom outage the data endpoint 500s while auth
// still works, so re-authenticating every 90s burns two handshakes for nothing.
static uint64_t last_reauth_time = 0;
#define REAUTH_COOLDOWN_MS  300000ULL  // 5 minutes between re-auth attempts

// Duplicate reading detection — catch "same data returned over and over"
static time_t last_successful_timestamp = 0;
static int duplicate_reading_count = 0;
#define MAX_DUPLICATE_READINGS 6  // After 6 identical timestamps (~9 min), force session refresh

// History backfill — after a reboot or an offline stretch the 24h chart has a
// hole the one-reading-per-fetch path can never fill.
#define BACKFILL_GAP_MS      (15LL * 60 * 1000)      // gap worth an extra request
#define BACKFILL_COOLDOWN_MS (30ULL * 60 * 1000)     // at most one attempt per 30 min
#define BACKFILL_MAX_COUNT   24                      // 2h of readings; bounds the payload
#define BACKFILL_BUF_SIZE    4096                    // temporary, freed before returning
#define BACKFILL_MIN_BLOCK   (20 * 1024)             // don't carve up an already tight heap

static uint64_t last_backfill_ms = 0;

// A multi-reading payload does not fit the 2KB steady-state response buffer, so
// the event handler redirects into this bounded scratch buffer for one request.
static char  *backfill_buf = NULL;
static size_t backfill_len = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static const char* get_base_url(void) {
    switch (current_region) {
        case DEXCOM_REGION_OUS: return BASE_URL_OUS;
        case DEXCOM_REGION_JP:  return BASE_URL_JP;
        default:                return BASE_URL_US;
    }
}

static const char* get_app_id(void) {
    return (current_region == DEXCOM_REGION_JP) ? APP_ID_JP : APP_ID_US_OUS;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (backfill_buf != NULL) {
                // Backfill in flight: capture into the scratch buffer. An overrun
                // costs only the tail records — the parser stops at the last
                // complete one.
                if (backfill_len + evt->data_len < BACKFILL_BUF_SIZE - 1) {
                    memcpy(backfill_buf + backfill_len, evt->data, evt->data_len);
                    backfill_len += evt->data_len;
                    backfill_buf[backfill_len] = '\0';
                } else {
                    ESP_LOGW(TAG, "Backfill response exceeded %d bytes — truncating",
                             BACKFILL_BUF_SIZE);
                }
                break;
            }
            if (http_response_len + evt->data_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(http_response + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response[http_response_len] = '\0';
            } else {
                ESP_LOGE(TAG, "HTTP response buffer overflow! Response too large (%d + %d >= %d)",
                         http_response_len, evt->data_len, HTTP_BUFFER_SIZE);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Remove surrounding quotes from response string
static void strip_quotes(char *str) {
    if (str == NULL || strlen(str) < 2) return;

    size_t len = strlen(str);
    if (str[0] == '"' && str[len-1] == '"') {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

// Check if response is a null GUID or error message
static bool is_null_guid(const char *str) {
    if (str == NULL) return true;
    return (strstr(str, "00000000-0000-0000-0000-000000000000") != NULL);
}

static bool is_valid_id(const char *str) {
    if (str == NULL || strlen(str) == 0) return false;
    if (strstr(str, "AccountNotFound") != NULL) return false;
    if (strstr(str, "InvalidPassword") != NULL) return false;
    if (is_null_guid(str)) return false;
    return true;
}

// Parse trend string to enum
static dexcom_trend_t parse_trend(const char *trend_str) {
    if (trend_str == NULL) return TREND_NONE;
    
    if (strcmp(trend_str, "Flat") == 0) return TREND_FLAT;
    if (strcmp(trend_str, "FortyFiveUp") == 0) return TREND_FORTY_FIVE_UP;
    if (strcmp(trend_str, "FortyFiveDown") == 0) return TREND_FORTY_FIVE_DOWN;
    if (strcmp(trend_str, "SingleUp") == 0) return TREND_SINGLE_UP;
    if (strcmp(trend_str, "SingleDown") == 0) return TREND_SINGLE_DOWN;
    if (strcmp(trend_str, "DoubleUp") == 0) return TREND_DOUBLE_UP;
    if (strcmp(trend_str, "DoubleDown") == 0) return TREND_DOUBLE_DOWN;
    if (strcmp(trend_str, "NotComputable") == 0) return TREND_NOT_COMPUTABLE;
    if (strcmp(trend_str, "RateOutOfRange") == 0) return TREND_RATE_OUT_OF_RANGE;
    
    return TREND_NONE;
}

// Parse Dexcom timestamp: "Date(1731645818222)" or "Date(1731645818222+0000)" -> time_t
static time_t parse_dexcom_timestamp(const char *dt_str) {
    if (dt_str == NULL) return 0;

    // Find the number between "Date(" and ")"
    const char *start = strstr(dt_str, "Date(");
    if (start == NULL) {
        // Also check for "/Date(...)" format
        start = strstr(dt_str, "/Date(");
        if (start == NULL) return 0;
        start += 6; // Skip "/Date("
    } else {
        start += 5; // Skip "Date("
    }

    // Find end position (+ or - for timezone, or ) for no timezone)
    char temp[32];
    int i = 0;
    while (start[i] && start[i] != ')' && start[i] != '+' && start[i] != '-' && i < 31) {
        temp[i] = start[i];
        i++;
    }
    temp[i] = '\0';

    long long ms = atoll(temp);

    time_t timestamp = (time_t)(ms / 1000);
    time_t now = time(NULL);

    // Reject an implausible timestamp rather than masking it as "now";
    // returning 0 forces the staleness check to throw the reading away.
    if (timestamp <= 0 || timestamp > now + 300) {  // Allow 5 min clock skew
        ESP_LOGW(TAG, "Invalid timestamp %ld (future or epoch), rejecting", (long)timestamp);
        return 0;
    }
    if ((now - timestamp) > 86400) {
        ESP_LOGW(TAG, "Timestamp %ld is >24h old (now=%ld), rejecting", (long)timestamp, (long)now);
        return 0;
    }

    return timestamp;
}

// ============================================================================
// HTTP Client Management (Connection Reuse)
// ============================================================================

// Cleanup the persistent HTTP client
static void cleanup_persistent_client(void) {
    if (persistent_client != NULL) {
        ESP_LOGI(TAG, "Cleaning up persistent HTTP client");
        esp_http_client_close(persistent_client);
        esp_http_client_cleanup(persistent_client);
        persistent_client = NULL;
    }
}

// Initialize the persistent HTTP client (creates it only once)
static esp_err_t init_persistent_client(void) {
    if (persistent_client != NULL) {
        return ESP_OK;  // Already initialized
    }

    ESP_LOGI(TAG, "Initializing persistent HTTP client");

    // Create a minimal config (URL will be set per-request)
    esp_http_client_config_t config = {
        .url = get_base_url(),  // Set base URL initially
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .is_async = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };

    persistent_client = esp_http_client_init(&config);
    if (persistent_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize persistent HTTP client");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Persistent HTTP client initialized successfully");
    return ESP_OK;
}

// Prepare the client for a new request (set URL, headers, body)
static esp_err_t prepare_client_for_request(const char *url, const char *body) {
    if (persistent_client == NULL) {
        ESP_LOGE(TAG, "Cannot prepare client - not initialized");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_set_url(persistent_client, url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URL: %s", esp_err_to_name(err));
        return err;
    }

    esp_http_client_set_header(persistent_client, "Content-Type", "application/json");
    esp_http_client_set_header(persistent_client, "Accept", "application/json");
    esp_http_client_set_header(persistent_client, "User-Agent", "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0");

    // Set POST body (check for NULL to prevent crash)
    if (body != NULL) {
        esp_http_client_set_post_field(persistent_client, body, strlen(body));
    } else {
        ESP_LOGW(TAG, "POST body is NULL - skipping");
        esp_http_client_set_post_field(persistent_client, "", 0);
    }

    return ESP_OK;
}

// ============================================================================
// History Backfill
// ============================================================================

// Scan an array payload for {"WT":"Date(ms)"...,"Value":N,...} records and hand
// each one the ring buffer is missing to glucose_history_insert(). Hand-rolled
// rather than cJSON so it allocates nothing while an SSL session is still open.
static int backfill_parse_and_store(const char *json, time_t skip_ts) {
    int inserted = 0;
    const char *p = json;

    while ((p = strstr(p, "\"WT\"")) != NULL) {
        const char *record = p;
        p += 4;  // always advance past this key, whatever happens below

        const char *value_key = strstr(record, "\"Value\"");
        if (value_key == NULL) {
            break;  // truncated tail record — stop at the last complete one
        }
        // Only pair a value with the record it sits in; one that belongs to the
        // next record means this one is malformed, so skip rather than mismatch.
        const char *next_record = strstr(p, "\"WT\"");
        if (next_record != NULL && value_key > next_record) {
            continue;
        }

        time_t ts = parse_dexcom_timestamp(record);

        const char *digits = value_key + 7;  // past "Value"
        while (*digits == ':' || *digits == ' ') digits++;
        int value = atoi(digits);

        if (ts <= 0 || value <= 0) {
            continue;
        }
        // The caller stores the live reading itself; inserting it here too would
        // put the same point in the buffer twice.
        if (skip_ts > 0 && llabs((long long)(ts - skip_ts)) < 120) {
            continue;
        }
        if (glucose_history_insert(value, (int64_t)ts * 1000)) {
            inserted++;
        }
    }

    return inserted;
}

// Fill the holes in the history buffer over the session and socket the caller
// just used, so it costs no handshake. skip_ts is the reading the caller stores.
static void dexcom_backfill_history(time_t skip_ts) {
    if (!is_authenticated || strlen(session_id) == 0 || persistent_client == NULL) {
        return;
    }

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (last_backfill_ms != 0 && (now_ms - last_backfill_ms) < BACKFILL_COOLDOWN_MS) {
        return;
    }

    // Only worth a request when the buffer is empty (fresh boot, wiped NVS) or
    // its newest reading is far enough behind the wall clock to be a real gap.
    int64_t newest = glucose_history_newest_timestamp();
    int64_t wall_ms = (int64_t)time(NULL) * 1000;
    if (newest > 0 && (wall_ms - newest) < BACKFILL_GAP_MS) {
        return;
    }

    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < BACKFILL_MIN_BLOCK) {
        ESP_LOGW(TAG, "Skipping history backfill — largest free block too small");
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_GLUCOSE);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return;
    }
    cJSON_AddStringToObject(body, "sessionId", session_id);
    cJSON_AddNumberToObject(body, "minutes", 1440);
    cJSON_AddNumberToObject(body, "maxCount", BACKFILL_MAX_COUNT);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) {
        return;
    }

    backfill_buf = malloc(BACKFILL_BUF_SIZE);
    if (backfill_buf == NULL) {
        ESP_LOGW(TAG, "Backfill buffer allocation failed — skipping");
        free(body_str);
        return;
    }
    backfill_buf[0] = '\0';
    backfill_len = 0;

    ESP_LOGI(TAG, "History gap detected — backfilling up to %d readings", BACKFILL_MAX_COUNT);

    esp_err_t err = prepare_client_for_request(url, body_str);
    if (err == ESP_OK) {
        err = esp_http_client_perform(persistent_client);
    }
    int status = esp_http_client_get_status_code(persistent_client);
    free(body_str);

    // Detach the scratch buffer before anything else can route data into it.
    char *buf = backfill_buf;
    size_t len = backfill_len;
    backfill_buf = NULL;
    backfill_len = 0;

    last_backfill_ms = now_ms;  // cooldown applies to attempts, not successes

    if (err == ESP_OK && status == 200 && len > 0) {
        int added = backfill_parse_and_store(buf, skip_ts);
        ESP_LOGI(TAG, "History backfill: %d readings added (%u bytes parsed)",
                 added, (unsigned)len);
        sd_log(TAG, "Backfill: +%d readings", added);
    } else {
        ESP_LOGW(TAG, "History backfill failed (err=%s, status=%d)",
                 esp_err_to_name(err), status);
    }

    free(buf);

    // A transport error means the socket died under us; the next fetch must not
    // inherit it.
    if (err != ESP_OK) {
        cleanup_persistent_client();
    }
}

// ============================================================================
// API Functions
// ============================================================================

esp_err_t dexcom_api_init(void) {
    ESP_LOGI(TAG, "Dexcom Share API initialized");
    
    // Try to load saved credentials from NVS
    char saved_user[64], saved_pass[64];
    if (nvs_get_dexcom_credentials(saved_user, sizeof(saved_user), 
                                    saved_pass, sizeof(saved_pass)) == ESP_OK) {
        ESP_LOGI(TAG, "Found saved Dexcom credentials");
        strncpy(stored_username, saved_user, sizeof(stored_username) - 1);
        strncpy(stored_password, saved_pass, sizeof(stored_password) - 1);
        // Don't auto-authenticate - let UI trigger it
    }
    
    return ESP_OK;
}

void dexcom_set_region(dexcom_region_t region) {
    current_region = region;
    ESP_LOGI(TAG, "Region set to: %s", 
             region == DEXCOM_REGION_US ? "US" : 
             region == DEXCOM_REGION_OUS ? "Outside US" : "Japan");
}

// Step 1: Get account ID from username/password
static esp_err_t get_account_id(const char *username, const char *password) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_AUTH_ACCOUNT);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "accountName", username);
    cJSON_AddStringToObject(body, "password", password);
    cJSON_AddStringToObject(body, "applicationId", get_app_id());
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Getting account ID for: %s", username);
    ESP_LOGI(TAG, "URL: %s", url);

    memset(http_response, 0, sizeof(http_response));
    http_response_len = 0;

    // Initialize persistent client if needed
    esp_err_t err = init_persistent_client();
    if (err != ESP_OK) {
        free(body_str);
        return err;
    }

    err = prepare_client_for_request(url, body_str);
    if (err != ESP_OK) {
        free(body_str);
        cleanup_persistent_client();  // Clean up on error
        return err;
    }

    ESP_LOGI(TAG, "Performing request...");
    err = esp_http_client_perform(persistent_client);
    int status = esp_http_client_get_status_code(persistent_client);
    ESP_LOGI(TAG, "Result: %s (status %d)", esp_err_to_name(err), status);

    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        cleanup_persistent_client();  // Clean up on error
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Auth failed with status %d: %s", status, http_response);
        cleanup_persistent_client();  // Clean up on error
        return ESP_FAIL;
    }

    // Response is just the account ID in quotes: "uuid-here"
    strncpy(account_id, http_response, sizeof(account_id) - 1);
    strip_quotes(account_id);

    if (!is_valid_id(account_id)) {
        ESP_LOGE(TAG, "Invalid account ID received: %s", http_response);
        cleanup_persistent_client();  // Clean up on error
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Got account ID: %.8s...", account_id);
    // DON'T cleanup - keep connection alive for next request
    return ESP_OK;
}

// Step 2: Get session ID from account ID/password
static esp_err_t get_session_id(const char *password) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_LOGIN_BY_ID);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "accountId", account_id);
    cJSON_AddStringToObject(body, "password", password);
    cJSON_AddStringToObject(body, "applicationId", get_app_id());
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Getting session ID...");

    memset(http_response, 0, sizeof(http_response));
    http_response_len = 0;

    // Should already be initialized by get_account_id(), but check anyway.
    if (persistent_client == NULL) {
        ESP_LOGW(TAG, "Persistent client not initialized, creating new one");
        esp_err_t err = init_persistent_client();
        if (err != ESP_OK) {
            free(body_str);
            return err;
        }
    }

    // Prepare the request — this reuses the SSL connection from get_account_id().
    esp_err_t err = prepare_client_for_request(url, body_str);
    if (err != ESP_OK) {
        free(body_str);
        cleanup_persistent_client();
        return err;
    }

    err = esp_http_client_perform(persistent_client);
    int status = esp_http_client_get_status_code(persistent_client);

    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        cleanup_persistent_client();
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Login failed with status %d: %s", status, http_response);
        cleanup_persistent_client();
        return ESP_FAIL;
    }

    // Response is the session ID in quotes
    strncpy(session_id, http_response, sizeof(session_id) - 1);
    strip_quotes(session_id);

    if (!is_valid_id(session_id)) {
        ESP_LOGE(TAG, "Invalid session ID received: %s", http_response);
        cleanup_persistent_client();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Got session ID: %.8s...", session_id);
    return ESP_OK;
}

// Fallback: Older login endpoint for compatibility
static esp_err_t get_session_by_name(const char *username, const char *password) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_LOGIN_BY_NAME);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "accountName", username);
    cJSON_AddStringToObject(body, "password", password);
    cJSON_AddStringToObject(body, "applicationId", get_app_id());
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Trying legacy login by name...");

    memset(http_response, 0, sizeof(http_response));
    http_response_len = 0;

    // Initialize client if needed (might be first call if two-step failed)
    if (persistent_client == NULL) {
        esp_err_t err = init_persistent_client();
        if (err != ESP_OK) {
            free(body_str);
            return err;
        }
    }

    esp_err_t err = prepare_client_for_request(url, body_str);
    if (err != ESP_OK) {
        free(body_str);
        cleanup_persistent_client();
        return err;
    }

    err = esp_http_client_perform(persistent_client);
    int status = esp_http_client_get_status_code(persistent_client);

    free(body_str);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Legacy login failed: status %d", status);
        cleanup_persistent_client();
        return ESP_FAIL;
    }

    // Response is the session ID in quotes
    strncpy(session_id, http_response, sizeof(session_id) - 1);
    strip_quotes(session_id);

    if (!is_valid_id(session_id)) {
        ESP_LOGW(TAG, "Legacy login returned invalid session");
        cleanup_persistent_client();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Got session via legacy endpoint: %.8s...", session_id);
    // DON'T cleanup - keep connection alive for glucose fetch
    return ESP_OK;
}

// Try authentication on a specific region
static esp_err_t try_authenticate_on_region(dexcom_region_t region, const char *username, const char *password) {
    current_region = region;
    ESP_LOGI(TAG, "Trying %s region...",
             region == DEXCOM_REGION_US ? "US" :
             region == DEXCOM_REGION_OUS ? "Outside US" : "Japan");

    // Try two-step login first
    esp_err_t err = get_account_id(username, password);
    if (err == ESP_OK) {
        // Small delay to let heap settle after SSL cleanup
        vTaskDelay(pdMS_TO_TICKS(500));

        err = get_session_id(password);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Two-step login failed, trying legacy endpoint...");

    vTaskDelay(pdMS_TO_TICKS(500));

    err = get_session_by_name(username, password);
    return err;
}

esp_err_t dexcom_authenticate(const char *username, const char *password) {
    ESP_LOGI(TAG, "Authenticating with Dexcom Share...");

    // TLS certificate validation needs a real clock — check before the handshake.
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "=== DEXCOM AUTH TIME CHECK ===");
    ESP_LOGI(TAG, "System time at auth: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    if (timeinfo.tm_year < (2020 - 1900)) {
        ESP_LOGE(TAG, "FATAL: System time is %d (before 2020)! SSL WILL FAIL!",
                 timeinfo.tm_year + 1900);
        return ESP_ERR_INVALID_STATE;
    }

    // Store credentials for potential re-auth
    strncpy(stored_username, username, sizeof(stored_username) - 1);
    strncpy(stored_password, password, sizeof(stored_password) - 1);

    // Probe the previously-successful region first (cached in NVS). Blindly
    // walking US -> OUS -> JP means one wrong password produces three failed
    // logins across Dexcom's servers, risking rate-limiting or lockout.
    dexcom_region_t order[3] = {DEXCOM_REGION_US, DEXCOM_REGION_OUS, DEXCOM_REGION_JP};
    int cached = nvs_get_dexcom_region();
    if (cached == DEXCOM_REGION_OUS) {
        order[0] = DEXCOM_REGION_OUS; order[1] = DEXCOM_REGION_US;
    } else if (cached == DEXCOM_REGION_JP) {
        order[0] = DEXCOM_REGION_JP;  order[1] = DEXCOM_REGION_US; order[2] = DEXCOM_REGION_OUS;
    }

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            ESP_LOGW(TAG, "Region %d failed, trying region %d...", order[i - 1], order[i]);
        }
        err = try_authenticate_on_region(order[i], username, password);
        if (err == ESP_OK) {
            goto success;
        }
    }

    // All regions failed
    ESP_LOGE(TAG, "Authentication failed on all regions");
    is_authenticated = false;
    cleanup_persistent_client();  // Free SSL resources
    return ESP_FAIL;

success:
    // Save credentials + the winning region (so next login is a single attempt)
    nvs_set_dexcom_credentials(username, password);
    nvs_save_dexcom_region((int)current_region);

    is_authenticated = true;
    last_login_time = (uint64_t)(esp_timer_get_time() / 1000);  // ms, no wrap
    last_reauth_time = last_login_time;  // Track for cooldown
    ESP_LOGI(TAG, "Dexcom authentication successful on %s region!",
             current_region == DEXCOM_REGION_US ? "US" :
             current_region == DEXCOM_REGION_OUS ? "Outside US" : "Japan");

    // Keep the persistent client alive for the glucose fetch; reusing it avoids
    // another ~40KB SSL handshake.
    ESP_LOGI(TAG, "Keeping persistent HTTP client alive for glucose fetches");

    return ESP_OK;
}


esp_err_t dexcom_fetch_glucose(dexcom_glucose_t *glucose) {
    if (glucose == NULL) return ESP_ERR_INVALID_ARG;

    glucose->valid = false;
    glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;

    if (!is_authenticated || strlen(session_id) == 0) {
        ESP_LOGE(TAG, "Not authenticated");
        return ESP_ERR_INVALID_STATE;
    }

    // Session timeout + proactive refresh (use esp_timer for 64-bit no-wrap)
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t now_ms = now_us / 1000;

    bool needs_reauth = false;
    if (last_login_time > 0) {
        uint64_t session_age = now_ms - last_login_time;
        if (session_age > SESSION_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Session expired (12 hours), re-authenticating...");
            needs_reauth = true;
        } else if (session_age > SESSION_REFRESH_MS) {
            ESP_LOGI(TAG, "Proactive session refresh (age: %lu min)", (unsigned long)(session_age / 60000));
            sd_log(TAG, "Session refresh (age=%lu min)", (unsigned long)(session_age / 60000));
            needs_reauth = true;
        }
    }

    if (needs_reauth) {
        is_authenticated = false;
        cleanup_persistent_client();
        if (strlen(stored_username) > 0 && strlen(stored_password) > 0) {
            esp_err_t err = dexcom_authenticate(stored_username, stored_password);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Rate limiting (90 seconds) — also use esp_timer
    if (dexcom_last_fetch_ms > 0) {
        uint64_t fetch_elapsed = now_ms - dexcom_last_fetch_ms;
        if (fetch_elapsed < RATE_LIMIT_MS) {
            ESP_LOGW(TAG, "Rate limited - wait %lu seconds", (unsigned long)((RATE_LIMIT_MS - fetch_elapsed) / 1000));
            glucose->status = GLUCOSE_STATUS_OK;
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Build URL (no query params - use POST body instead)
    char url[256];
    snprintf(url, sizeof(url), "%s%s", get_base_url(), ENDPOINT_GLUCOSE);

    // Build POST body (more reliable than URL query)
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON body - out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "sessionId", session_id);
    cJSON_AddNumberToObject(body, "minutes", 1440);   // Request 24h window (never miss a reading)
    cJSON_AddNumberToObject(body, "maxCount", 1);     // Only latest reading
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory (free heap: %lu bytes)", esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Fetching glucose data...");
    ESP_LOGI(TAG, "Free heap before glucose fetch: %lu bytes", esp_get_free_heap_size());

    memset(http_response, 0, sizeof(http_response));
    http_response_len = 0;

    // Entry is already gated on free heap in main.c. Closing a working SSL
    // connection just to reopen it wastes time and risks OOM in the new handshake.

    // Should already exist from authentication; check in case the session was
    // restored from NVS after a reboot.
    if (persistent_client == NULL) {
        ESP_LOGI(TAG, "Creating new persistent client (session restored from NVS or memory fix)");
        esp_err_t init_err = init_persistent_client();
        if (init_err != ESP_OK) {
            free(body_str);
            return init_err;
        }
    } else {
        ESP_LOGI(TAG, "Reusing persistent HTTP client (SSL connection already established)");
    }

    // Prepare the request — this reuses the SSL connection from authentication.
    esp_err_t err = prepare_client_for_request(url, body_str);
    if (err != ESP_OK) {
        free(body_str);
        cleanup_persistent_client();
        return err;
    }

    err = esp_http_client_perform(persistent_client);
    int status = esp_http_client_get_status_code(persistent_client);

    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        glucose->status = GLUCOSE_STATUS_NO_DATA;  // Network error, but still authenticated

        // Connection errors (write fail, connect fail) mean the TCP socket is dead —
        // keeping it alive guarantees the next retry also fails.  Close immediately.
        ESP_LOGW(TAG, "Connection error — closing dead SSL connection for fresh retry");
        cleanup_persistent_client();
        consecutive_failures = 0;
        return err;
    }

    // Non-200 handling turns on one distinction. An expired session (HTTP 401, or
    // a 500 whose body names a session error) is fixed by re-auth; a backend outage
    // (500 without those keywords, 502/503/504) is not. Treating repeated failures
    // as a session error produced an endless re-auth loop during outages — two SSL
    // handshakes every 90s for nothing — so leave those to task-level backoff.
    if (status != 200) {
        ESP_LOGE(TAG, "Glucose fetch failed with status %d: %s", status, http_response);

        // Determine if this is a confirmed session expiration error
        bool session_error = (status == 401);  // 401 always means session expired

        // Check 500 response body for session-related error strings
        if (status == 500 && http_response_len > 0) {
            if (strstr(http_response, "SessionNotValid") != NULL ||
                strstr(http_response, "SessionIdNotFound") != NULL ||
                strstr(http_response, "SessionNotFound") != NULL ||
                strstr(http_response, "InvalidSession") != NULL) {
                session_error = true;
                ESP_LOGW(TAG, "Dexcom 500 response indicates session expiration");
            }
        }

        // Track consecutive failures for connection management (NOT for session detection)
        if (!session_error) {
            consecutive_failures++;
            if (status >= 500) {
                ESP_LOGW(TAG, "Server error %d (not session-related), consecutive=%d",
                         status, consecutive_failures);
                sd_log(TAG, "Server error (HTTP %d), consecutive=%d", status, consecutive_failures);
            }
        }

        if (session_error) {
            // Check re-auth cooldown: prevent re-auth storm during outages
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
            uint64_t since_last_reauth = now_ms - last_reauth_time;

            if (last_reauth_time > 0 && since_last_reauth < REAUTH_COOLDOWN_MS) {
                ESP_LOGW(TAG, "Re-auth cooldown active (%lu s remaining) — skipping re-auth",
                         (unsigned long)((REAUTH_COOLDOWN_MS - since_last_reauth) / 1000));
                sd_log(TAG, "Re-auth cooldown (%lus left), skip", (unsigned long)((REAUTH_COOLDOWN_MS - since_last_reauth) / 1000));
                glucose->status = GLUCOSE_STATUS_NO_DATA;
                cleanup_persistent_client();
                return ESP_FAIL;
            }

            ESP_LOGW(TAG, "Session expired (status %d), re-authenticating...", status);
            sd_log(TAG, "Session error (HTTP %d), re-auth...", status);
            is_authenticated = false;
            cleanup_persistent_client();
            dexcom_last_fetch_ms = 0;  // Clear rate limit so retry works immediately

            if (strlen(stored_username) > 0 && strlen(stored_password) > 0) {
                esp_err_t reauth_err = dexcom_authenticate(stored_username, stored_password);
                if (reauth_err == ESP_OK) {
                    consecutive_failures = 0;
                    ESP_LOGI(TAG, "Re-auth succeeded — retrying glucose fetch immediately");
                    sd_log(TAG, "Re-auth OK, immediate retry");
                    // Retry now rather than waiting out the 90s cycle; the
                    // recursion is safe because re-auth reset the session state.
                    return dexcom_fetch_glucose(glucose);
                }
            }

            ESP_LOGE(TAG, "Re-authentication failed");
            glucose->status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
            return ESP_FAIL;
        }

        // Non-session server error — keep client alive but force fresh after too many
        glucose->status = GLUCOSE_STATUS_NO_DATA;
        if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            ESP_LOGW(TAG, "Too many consecutive errors (%d) - forcing fresh connection",
                     consecutive_failures);
            cleanup_persistent_client();
            consecutive_failures = 0;
        }
        return ESP_FAIL;
    }

    // An empty response usually means the SSL connection timed out; retrying on a
    // fresh connection recovers it, and skipping the parse avoids a bogus error.
    if (http_response_len == 0 || strlen(http_response) == 0) {
        ESP_LOGW(TAG, "Empty response from Dexcom (SSL connection may have timed out)");
        ESP_LOGW(TAG, "Response length: %d, strlen: %zu", http_response_len, strlen(http_response));
        glucose->status = GLUCOSE_STATUS_NO_DATA;  // Still authenticated, just empty data

        // Don't cleanup - empty response might be transient issue
        ESP_LOGW(TAG, "Keeping persistent client alive - will retry on next fetch");
        return ESP_FAIL;
    }

    // Parse JSON response: [{"WT":"Date(...)","ST":"Date(...)","DT":"Date(...)","Value":155,"Trend":"Flat"}]
    cJSON *root = cJSON_Parse(http_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        ESP_LOGE(TAG, "Response that failed to parse: %s", http_response);
        glucose->status = GLUCOSE_STATUS_NO_DATA;  // Still authenticated, just bad data

        // Don't cleanup on parse errors - might be transient server issue
        ESP_LOGW(TAG, "Keeping persistent client alive despite parse error");
        return ESP_FAIL;
    }

    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        ESP_LOGW(TAG, "No glucose data available (empty array from Dexcom)");

        // Determine likely reason for no data
        uint64_t time_since_login = (uint64_t)(esp_timer_get_time() / 1000) - last_login_time;
        if (time_since_login < 7200000) {  // Less than 2 hours since login
            ESP_LOGI(TAG, "Likely sensor warmup period (logged in %lu min ago)", (unsigned long)(time_since_login / 60000));
            glucose->status = GLUCOSE_STATUS_WARMUP;
        } else {
            ESP_LOGI(TAG, "No data available (signal loss or no sensor)");
            glucose->status = GLUCOSE_STATUS_NO_DATA;
        }

        cJSON_Delete(root);
        // Don't cleanup client - we're authenticated, just no data yet
        return ESP_FAIL;
    }

    // Get the first (most recent) reading
    cJSON *reading = cJSON_GetArrayItem(root, 0);
    if (reading == NULL) {
        ESP_LOGE(TAG, "Failed to get reading from array");
        glucose->status = GLUCOSE_STATUS_NO_DATA;  // Data parsing error, but still authenticated
        cJSON_Delete(root);

        // Don't cleanup on data extraction errors
        ESP_LOGW(TAG, "Keeping persistent client alive despite extraction error");
        return ESP_FAIL;
    }
    
    cJSON *value = cJSON_GetObjectItem(reading, "Value");
    if (cJSON_IsNumber(value)) {
        glucose->value = value->valueint;
    }
    
    cJSON *trend = cJSON_GetObjectItem(reading, "Trend");
    if (cJSON_IsString(trend)) {
        glucose->trend = parse_trend(trend->valuestring);
    }
    
    // Extract timestamp (WT = wall time in UTC)
    cJSON *wt = cJSON_GetObjectItem(reading, "WT");
    if (cJSON_IsString(wt)) {
        glucose->timestamp = parse_dexcom_timestamp(wt->valuestring);
    }

    // parse_dexcom_timestamp() returns 0 on failure — reject rather than show it.
    if (glucose->timestamp == 0) {
        ESP_LOGW(TAG, "Glucose timestamp invalid (parse failed) - rejecting data");
        glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
        glucose->valid = false;
        cJSON_Delete(root);
        dexcom_last_fetch_ms = (uint64_t)(esp_timer_get_time() / 1000);
        consecutive_failures = 0;
        return ESP_OK;  // API worked but data is unusable
    }

    // Check if data is stale (more than 10 minutes old = likely signal loss)
    time_t current_time;
    time(&current_time);
    int minutes_old = (int)difftime(current_time, glucose->timestamp) / 60;

    sd_log(TAG, "Fetch: %d mg/dL, %s, age=%dmin",
           glucose->value, dexcom_trend_description(glucose->trend), minutes_old);

    if (minutes_old > 10) {
        ESP_LOGW(TAG, "Glucose data is %d minutes old - signal loss or stale", minutes_old);
        glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
        glucose->valid = false;  // Mark as invalid due to age
    } else {
        glucose->status = GLUCOSE_STATUS_OK;
        glucose->valid = true;
    }

    // Duplicate reading detection: same timestamp returned multiple times
    // means Dexcom has no new data — possible transmitter/sensor issue
    if (glucose->timestamp == last_successful_timestamp) {
        duplicate_reading_count++;
        ESP_LOGW(TAG, "Duplicate reading #%d (same timestamp %ld)", duplicate_reading_count, (long)glucose->timestamp);
        sd_log(TAG, "Duplicate #%d (ts=%ld)", duplicate_reading_count, (long)glucose->timestamp);
        if (duplicate_reading_count >= MAX_DUPLICATE_READINGS) {
            ESP_LOGW(TAG, "SAFETY: %d duplicate readings — forcing session refresh", duplicate_reading_count);
            sd_log(TAG, "SAFETY: %d duplicates, forcing re-auth", duplicate_reading_count);
            duplicate_reading_count = 0;
            is_authenticated = false;
            cleanup_persistent_client();
            // Force re-auth on next fetch cycle
        }
    } else {
        if (glucose->valid) {
            last_successful_timestamp = glucose->timestamp;
            duplicate_reading_count = 0;
        }
    }

    dexcom_last_fetch_ms = (uint64_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "Glucose: %d mg/dL, Trend: %s (Status: %d)",
             glucose->value, dexcom_trend_description(glucose->trend), glucose->status);

    cJSON_Delete(root);

    // Keep the persistent client alive. Its ~40KB SSL context stays allocated, but
    // recreating the connection every 90 seconds fragments the heap instead.
    // It is closed on authentication errors and at shutdown.
    ESP_LOGD(TAG, "Keeping persistent client alive. Free heap: %lu bytes", esp_get_free_heap_size());

    consecutive_failures = 0;

    // Close the chart's holes while the session and socket are warm. Self-gated on
    // the history gap and a 30-minute cooldown; the live reading is skipped here.
    if (glucose->valid) {
        dexcom_backfill_history(glucose->timestamp);
    }

    return ESP_OK;
}

bool dexcom_is_authenticated(void) {
    if (!is_authenticated || strlen(session_id) == 0) {
        return false;
    }

    // Check if session has timed out (12 hours)
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (last_login_time > 0 && (now_ms - last_login_time) > SESSION_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Session has timed out (12 hours)");
        is_authenticated = false;
        memset(session_id, 0, sizeof(session_id));
        return false;
    }

    return true;
}

bool dexcom_session_needs_refresh(void) {
    if (!is_authenticated || last_login_time == 0) {
        return false;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t session_age = now_ms - last_login_time;
    return (session_age > SESSION_REFRESH_MS);
}

void dexcom_logout(void) {
    memset(account_id, 0, sizeof(account_id));
    memset(session_id, 0, sizeof(session_id));
    memset(stored_username, 0, sizeof(stored_username));
    memset(stored_password, 0, sizeof(stored_password));
    is_authenticated = false;
    last_login_time = 0;
    dexcom_last_fetch_ms = 0;
    last_reauth_time = 0;

    // Clear from NVS
    nvs_clear_dexcom_credentials();

    ESP_LOGI(TAG, "Logged out from Dexcom");
}

const char* dexcom_trend_arrow(dexcom_trend_t trend) {
    switch (trend) {
        case TREND_DOUBLE_UP:       return "⇈";
        case TREND_SINGLE_UP:       return "↑";
        case TREND_FORTY_FIVE_UP:   return "↗";
        case TREND_FLAT:            return "→";
        case TREND_FORTY_FIVE_DOWN: return "↘";
        case TREND_SINGLE_DOWN:     return "↓";
        case TREND_DOUBLE_DOWN:     return "⇊";
        case TREND_NOT_COMPUTABLE:  return "?";
        case TREND_RATE_OUT_OF_RANGE: return "!";
        default:                    return "—";
    }
}

const char* dexcom_trend_description(dexcom_trend_t trend) {
    switch (trend) {
        case TREND_DOUBLE_UP:       return "Rising quickly";
        case TREND_SINGLE_UP:       return "Rising";
        case TREND_FORTY_FIVE_UP:   return "Rising slowly";
        case TREND_FLAT:            return "Steady";
        case TREND_FORTY_FIVE_DOWN: return "Falling slowly";
        case TREND_SINGLE_DOWN:     return "Falling";
        case TREND_DOUBLE_DOWN:     return "Falling quickly";
        case TREND_NOT_COMPUTABLE:  return "Unknown";
        case TREND_RATE_OUT_OF_RANGE: return "Out of range";
        default:                    return "None";
    }
}

// Check if persistent client is currently open
bool dexcom_persistent_client_is_open(void) {
    return (persistent_client != NULL);
}

// Close the persistent HTTP client to free ~30KB — do this before other SSL work.
void dexcom_close_persistent_client(void) {
    if (persistent_client != NULL) {
        ESP_LOGI(TAG, "Closing persistent HTTP client to free memory (~30KB)");
        esp_http_client_close(persistent_client);
        esp_http_client_cleanup(persistent_client);
        persistent_client = NULL;
        ESP_LOGI(TAG, "Free heap after closing Dexcom client: %lu bytes", esp_get_free_heap_size());
    }
}

