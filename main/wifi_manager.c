#include "wifi_manager.h"
#include "nvs_config.h"
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
            current_status = WIFI_STATUS_FAILED;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

esp_err_t wifi_manager_connect(void) {
    wifi_credentials_t creds;

    // Try to load saved credentials
    if (nvs_load_wifi_credentials(&creds) == ESP_OK) {
        ESP_LOGI(TAG, "Using saved WiFi credentials for: %s", creds.ssid);
    } else {
        ESP_LOGW(TAG, "No saved WiFi credentials found");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, creds.ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, creds.password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else {
        return ESP_FAIL;
    }
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

    // Find best matching saved network (strongest signal)
    int best_rssi = -127;
    wifi_credentials_t *best_creds = NULL;

    for (int i = 0; i < found; i++) {
        for (int j = 0; j < saved_count; j++) {
            if (strcmp((char *)scan_results[i].ssid, networks[j].ssid) == 0) {
                if (scan_results[i].rssi > best_rssi) {
                    best_rssi = scan_results[i].rssi;
                    best_creds = &networks[j];
                }
            }
        }
    }

    if (best_creds == NULL) {
        ESP_LOGD(TAG, "No known networks in range");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Auto-reconnect: trying %s (RSSI: %d)", best_creds->ssid, best_rssi);

    // Reset retry counter and event bits for fresh connection attempt
    retry_count = 0;
    current_status = WIFI_STATUS_CONNECTING;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Configure and connect
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    strncpy((char *)wifi_config.sta.ssid, best_creds->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, best_creds->password, sizeof(wifi_config.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    // Wait up to 15 seconds for result
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Auto-reconnect successful: %s", best_creds->ssid);
        nvs_save_last_wifi_ssid(best_creds->ssid);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Auto-reconnect failed for: %s", best_creds->ssid);
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
