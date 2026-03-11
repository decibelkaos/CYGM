#include "wifi_manager.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t wifi_event_group;
static wifi_status_t current_status = WIFI_STATUS_DISCONNECTED;
static int retry_count = 0;
static const int MAX_RETRY = 5;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started (ready for connection)");
        // Don't auto-connect here - wait for wifi_manager_connect() to be called
        current_status = WIFI_STATUS_DISCONNECTED;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Update global WiFi status immediately on disconnection
        extern bool wifi_connected;
        wifi_connected = false;

        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", retry_count, MAX_RETRY);
            current_status = WIFI_STATUS_CONNECTING;
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d attempts", MAX_RETRY);
            sd_log(TAG, "WiFi: FAILED after %d retries", MAX_RETRY);
            current_status = WIFI_STATUS_FAILED;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        sd_log(TAG, "WiFi: connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        current_status = WIFI_STATUS_CONNECTED;

        // Update global WiFi status on successful connection
        extern bool wifi_connected;
        wifi_connected = true;
    }
}

esp_err_t wifi_manager_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create WiFi station interface and set custom hostname
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // Set hostname for DHCP identification (shows in router's device list)
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, "CYGM_Monitor"));
    ESP_LOGI(TAG, "WiFi hostname set to: CYGM_Monitor (will appear in DHCP client lists)");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_connect_to(const char *ssid, const char *password) {
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    // Suppress event handler auto-retry during switchover — without this,
    // the DISCONNECTED event fires and reconnects to the OLD config before
    // we can set the new SSID/password, causing "sta is connected" errors.
    retry_count = MAX_RETRY;

    // Disconnect any existing connection first
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Reset state for fresh attempt (event handler is now safe — new config below)
    retry_count = 0;
    current_status = WIFI_STATUS_CONNECTING;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,  // Accept any auth mode (WPA/WPA2/WPA3/Open)
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set_config failed: %s", esp_err_to_name(err));
        current_status = WIFI_STATUS_FAILED;
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        current_status = WIFI_STATUS_FAILED;
        return err;
    }

    // Wait for connection or failure
    // With MAX_RETRY=5 retries at ~5-10s each, allow up to 60s total
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(60000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to: %s", ssid);
        return ESP_OK;
    }

    // Timeout or failure - clean up
    if (!(bits & WIFI_FAIL_BIT)) {
        ESP_LOGW(TAG, "Connection timeout for: %s", ssid);
        esp_wifi_disconnect();
    }

    current_status = WIFI_STATUS_FAILED;
    return ESP_FAIL;
}

esp_err_t wifi_manager_connect(void) {
    wifi_credentials_t creds;

    if (nvs_load_wifi_credentials(&creds) == ESP_OK) {
        ESP_LOGI(TAG, "Using saved WiFi credentials for: %s", creds.ssid);
        return wifi_manager_connect_to(creds.ssid, creds.password);
    }

    ESP_LOGW(TAG, "No saved WiFi credentials found");
    return ESP_ERR_NOT_FOUND;
}

wifi_status_t wifi_manager_get_status(void) {
    return current_status;
}

bool wifi_manager_is_connected(void) {
    return current_status == WIFI_STATUS_CONNECTED;
}

void wifi_manager_disconnect(void) {
    esp_wifi_disconnect();
    current_status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi disconnected");
}

// Note: Credential prompting will be implemented in main.c since it needs
// access to display and CardKB functions
esp_err_t wifi_manager_prompt_credentials(char *ssid_out, char *password_out) {
    // This function is a placeholder - actual implementation will be in main.c
    // using display and CardKB functions
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_try_reconnect(void) {
    // Already connected - nothing to do
    if (wifi_manager_is_connected()) {
        return ESP_OK;
    }

    // Load saved networks from NVS
    wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
    uint8_t saved_count = 0;
    nvs_get_saved_wifi_networks(networks, &saved_count);
    if (saved_count == 0) {
        ESP_LOGD(TAG, "No saved networks for auto-reconnect");
        return ESP_ERR_NOT_FOUND;
    }

    // Scan for available networks
    wifi_ap_record_t scan_results[10];
    int found = wifi_manager_scan_networks(scan_results, 10);
    if (found == 0) {
        ESP_LOGD(TAG, "No networks visible for auto-reconnect");
        return ESP_ERR_NOT_FOUND;
    }

    // Build list of matching saved networks with best RSSI for each
    typedef struct { int creds_idx; int8_t rssi; } wifi_match_t;
    wifi_match_t matches[MAX_SAVED_WIFI_NETWORKS];
    int match_count = 0;

    for (int j = 0; j < saved_count; j++) {
        int8_t best_rssi = -127;
        bool found_ap = false;
        for (int i = 0; i < found; i++) {
            if (strcmp((char *)scan_results[i].ssid, networks[j].ssid) == 0) {
                if (scan_results[i].rssi > best_rssi) {
                    best_rssi = scan_results[i].rssi;
                    found_ap = true;
                }
            }
        }
        if (found_ap) {
            matches[match_count].creds_idx = j;
            matches[match_count].rssi = best_rssi;
            match_count++;
        }
    }

    if (match_count == 0) {
        ESP_LOGD(TAG, "No known networks in range");
        return ESP_ERR_NOT_FOUND;
    }

    // Sort by RSSI descending (strongest first)
    for (int i = 0; i < match_count - 1; i++) {
        for (int j = 0; j < match_count - i - 1; j++) {
            if (matches[j].rssi < matches[j + 1].rssi) {
                wifi_match_t tmp = matches[j];
                matches[j] = matches[j + 1];
                matches[j + 1] = tmp;
            }
        }
    }

    // Try each matching network in RSSI order
    for (int m = 0; m < match_count; m++) {
        int idx = matches[m].creds_idx;
        ESP_LOGI(TAG, "Auto-reconnect: trying %s (RSSI: %d) [%d/%d]",
                 networks[idx].ssid, matches[m].rssi, m + 1, match_count);

        esp_err_t ret = wifi_manager_connect_to(networks[idx].ssid, networks[idx].password);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Auto-reconnect successful: %s", networks[idx].ssid);
            sd_log(TAG, "WiFi: reconnected to %s (RSSI=%d)", networks[idx].ssid, matches[m].rssi);
            nvs_save_last_wifi_ssid(networks[idx].ssid);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Auto-reconnect failed for: %s", networks[idx].ssid);
        if (m < match_count - 1) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause before next attempt
        }
    }

    sd_log(TAG, "WiFi: reconnect FAILED for all %d networks", match_count);
    return ESP_FAIL;
}

int wifi_manager_scan_networks(wifi_ap_record_t *ap_records, int max_records) {
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    ESP_LOGI(TAG, "Starting WiFi scan...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No WiFi networks found");
        return 0;
    }

    if (ap_count > max_records) {
        ap_count = max_records;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(ret));
        return 0;
    }

    ESP_LOGI(TAG, "Found %d WiFi networks", ap_count);
    return ap_count;
}
