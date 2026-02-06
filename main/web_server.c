/*
 * Web Server Module
 * HTTP server with REST API and static file serving
 */

#include "web_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

// Project modules
#include "wifi_manager.h"
#include "nvs_config.h"
#include "dexcom_api.h"
#include "ui_config.h"
#include "shared_state.h"
#include "esp_lvgl_port.h"

static const char *TAG = "WebServer";

// ==================== Screenshot Configuration ====================

// Set this to your computer's IP address (shown in the screenshot server GUI)
#define SCREENSHOT_SERVER_IP "192.168.7.61"  // Your computer's IP from the GUI
#define SCREENSHOT_SERVER_PORT 8080

// ==================== Global State ====================

static httpd_handle_t server = NULL;

// ==================== BMP File Structures ====================

// BMP file header (14 bytes)
typedef struct __attribute__((packed)) {
    uint16_t file_type;      // 0x4D42 = 'BM'
    uint32_t file_size;      // Total file size
    uint16_t reserved1;      // Always 0
    uint16_t reserved2;      // Always 0
    uint32_t offset_data;    // Offset to image data
} bmp_file_header_t;

// BMP info header (40 bytes)
typedef struct __attribute__((packed)) {
    uint32_t size;              // Header size (40)
    int32_t width;              // Image width
    int32_t height;             // Image height
    uint16_t planes;            // Color planes (always 1)
    uint16_t bit_count;         // Bits per pixel (24)
    uint32_t compression;       // Compression type (0 = none)
    uint32_t size_image;        // Image size (can be 0 for uncompressed)
    int32_t x_pixels_per_meter; // Horizontal resolution
    int32_t y_pixels_per_meter; // Vertical resolution
    uint32_t colors_used;       // Number of colors (0 = all)
    uint32_t colors_important;  // Important colors (0 = all)
} bmp_info_header_t;

// ==================== MIME Type Detection ====================

static const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (ext == NULL) return "text/plain";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";

    return "text/plain";
}

// ==================== Static File Serving ====================

static esp_err_t serve_file(httpd_req_t *req, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Set content type
    const char* mime = get_mime_type(filepath);
    httpd_resp_set_type(req, mime);

    // Read and send file in chunks
    char* chunk = malloc(1024);
    if (chunk == NULL) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t read_len;
    while ((read_len = fread(chunk, 1, 1024, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_len) != ESP_OK) {
            fclose(file);
            free(chunk);
            return ESP_FAIL;
        }
    }

    fclose(file);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ==================== HTTP Handlers ====================

// Root handler - serves index.html
static esp_err_t root_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/index.html");
}

// Catch-all handler for static files
static esp_err_t static_handler(httpd_req_t *req) {
    char filepath[256];
    int len = snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);

    // Check for truncation
    if (len >= sizeof(filepath)) {
        ESP_LOGE(TAG, "URI too long: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
        return ESP_FAIL;
    }

    // Check if file exists
    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    return serve_file(req, filepath);
}

// System info endpoint
static esp_err_t system_info_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    // Heap information
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());

    // Uptime
    cJSON_AddNumberToObject(root, "uptime_sec", esp_timer_get_time() / 1000000);

    // WiFi info
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    }

    // IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", ip_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// System restart endpoint
static esp_err_t system_restart_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");

    // Restart after 1 second
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ==================== WiFi Endpoints ====================

// GET /api/wifi - Get WiFi status
static esp_err_t wifi_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    // Check if connected
    bool connected = wifi_manager_is_connected();
    cJSON_AddBoolToObject(root, "connected", connected);

    // Get current SSID if connected
    if (connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
        }
    } else if (nvs_has_wifi_credentials()) {
        wifi_credentials_t creds;
        if (nvs_load_wifi_credentials(&creds) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", creds.ssid);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/wifi - Save WiFi credentials
static esp_err_t wifi_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    if (ssid == NULL || !cJSON_IsString(ssid)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    const char *pass_str = (password && cJSON_IsString(password)) ? password->valuestring : "";

    // Save credentials
    esp_err_t err = nvs_save_wifi_credentials(ssid->valuestring, pass_str);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Trigger reconnection
    wifi_manager_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/wifi/scan - Scan for WiFi networks
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    wifi_ap_record_t ap_records[20];
    int count = wifi_manager_scan_networks(ap_records, 20);

    cJSON *root = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char*)ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "channel", ap_records[i].primary);
        cJSON_AddItemToArray(root, network);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// ==================== Time Endpoints ====================

// GET /api/time - Get time settings
static esp_err_t time_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    time_settings_t settings;
    if (nvs_load_time_settings(&settings) == ESP_OK) {
        cJSON_AddStringToObject(root, "timezone", settings.timezone);
        cJSON_AddBoolToObject(root, "dst_enabled", settings.dst_enabled);
        cJSON_AddBoolToObject(root, "format_24h", settings.format_24hr);
    } else {
        // Defaults
        cJSON_AddStringToObject(root, "timezone", "America/New_York");
        cJSON_AddBoolToObject(root, "dst_enabled", true);
        cJSON_AddBoolToObject(root, "format_24h", false);
    }

    // Add current time
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "current_time", time_str);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/time - Update time settings
static esp_err_t time_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    cJSON *dst_enabled = cJSON_GetObjectItem(root, "dst_enabled");
    cJSON *format_24h = cJSON_GetObjectItem(root, "format_24h");

    if (timezone == NULL || !cJSON_IsString(timezone)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing timezone");
        return ESP_FAIL;
    }

    bool dst = (dst_enabled && cJSON_IsTrue(dst_enabled));
    bool fmt24 = (format_24h && cJSON_IsTrue(format_24h));

    esp_err_t err = nvs_save_time_settings(timezone->valuestring, dst, fmt24);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Update timezone
    setenv("TZ", timezone->valuestring, 1);
    tzset();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ==================== Weather Endpoints ====================

// GET /api/weather - Get weather settings
static esp_err_t weather_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    weather_settings_t settings;
    if (nvs_load_weather_settings(&settings) == ESP_OK) {
        cJSON_AddStringToObject(root, "zipcode", settings.zipcode);
        cJSON_AddStringToObject(root, "temp_unit", settings.temp_celsius ? "C" : "F");
        cJSON_AddNumberToObject(root, "update_interval_min", settings.update_interval_min);
    } else {
        // Defaults
        cJSON_AddStringToObject(root, "zipcode", "");
        cJSON_AddStringToObject(root, "temp_unit", "F");
        cJSON_AddNumberToObject(root, "update_interval_min", 5);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/weather - Update weather settings
static esp_err_t weather_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *zipcode = cJSON_GetObjectItem(root, "zipcode");
    cJSON *temp_unit = cJSON_GetObjectItem(root, "temp_unit");
    cJSON *interval = cJSON_GetObjectItem(root, "update_interval_min");

    if (zipcode == NULL || !cJSON_IsString(zipcode)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing zipcode");
        return ESP_FAIL;
    }

    bool celsius = (temp_unit && cJSON_IsString(temp_unit) &&
                    strcmp(temp_unit->valuestring, "C") == 0);

    // Parse interval (default 5 minutes, clamp to 5-90 range)
    uint8_t interval_min = 5;
    if (interval && cJSON_IsNumber(interval)) {
        interval_min = (uint8_t)interval->valueint;
        if (interval_min < 5) interval_min = 5;
        if (interval_min > 90) interval_min = 90;
    }

    esp_err_t err = nvs_save_weather_settings(zipcode->valuestring, celsius, interval_min);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ==================== CGM Endpoints ====================

// GET /api/cgm - Get CGM status
static esp_err_t cgm_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();

    char cgm_type[MAX_CGM_TYPE_LEN] = "none";
    nvs_load_cgm_type(cgm_type, sizeof(cgm_type));
    cJSON_AddStringToObject(root, "type", cgm_type);

    bool authenticated = dexcom_is_authenticated();
    cJSON_AddBoolToObject(root, "authenticated", authenticated);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/cgm/login - Login to Dexcom
static esp_err_t cgm_login_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    if (username == NULL || !cJSON_IsString(username) ||
        password == NULL || !cJSON_IsString(password)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing credentials");
        return ESP_FAIL;
    }

    // Attempt authentication
    esp_err_t err = dexcom_authenticate(username->valuestring, password->valuestring);

    if (err == ESP_OK) {
        // Save credentials
        nvs_set_dexcom_credentials(username->valuestring, password->valuestring);
        nvs_save_cgm_type("dexcom");
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authentication failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/cgm/auth - Logout from CGM
static esp_err_t cgm_logout_handler(httpd_req_t *req) {
    dexcom_logout();
    nvs_clear_dexcom_credentials();
    nvs_clear_cgm_settings();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ==================== UI Layout Endpoints ====================

// GET /api/ui/layout - Get current UI layout
static esp_err_t ui_layout_get_handler(httpd_req_t *req) {
    char *json_str = ui_config_get_layout_json();

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/ui/layout - Save UI layout
static esp_err_t ui_layout_post_handler(httpd_req_t *req) {
    char *buf = malloc(4096);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, 4095);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = ui_config_save_layout_json(buf);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/ui/defaults - Reset to default layout
static esp_err_t ui_defaults_handler(httpd_req_t *req) {
    esp_err_t err = ui_config_reset_layout();

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/ui/screenshot - Screenshot disabled (insufficient memory)
static esp_err_t ui_screenshot_handler(httpd_req_t *req) {
    // Screenshot feature disabled - ESP32 has insufficient free RAM (~50KB available, 230KB needed)
    // The Python GUI screenshot_server.py is ready for future use with devices that have SPIRAM

    ESP_LOGW(TAG, "Screenshot requested but feature is disabled (insufficient memory)");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"disabled\",\"message\":\"Screenshot feature disabled due to insufficient memory. Device has ~50KB free RAM but requires 230KB for screenshots. Enable SPIRAM to use this feature.\"}");

    return ESP_OK;
}

// ==================== Icon Endpoints ====================

// Helper function to get URL parameter
static esp_err_t get_url_param(httpd_req_t *req, const char *param_name, char *buf, size_t buf_len) {
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_FAIL;
    }

    char *query = malloc(query_len + 1);
    if (query == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (httpd_req_get_url_query_str(req, query, query_len + 1) != ESP_OK) {
        free(query);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_query_key_value(query, param_name, buf, buf_len);
    free(query);

    return ret;
}

// GET /api/icons/trend?id=X - Get trend arrow design
static esp_err_t icon_trend_get_handler(httpd_req_t *req) {
    char icon_id[32];
    if (get_url_param(req, "id", icon_id, sizeof(icon_id)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing icon ID");
        return ESP_FAIL;
    }

    char *json_str = ui_config_get_icon_json("trend", icon_id);

    if (json_str == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/icons/trend - Save trend arrow design
static esp_err_t icon_trend_post_handler(httpd_req_t *req) {
    char *buf = malloc(2048);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, 2047);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = ui_config_save_icon_json("trend", buf);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/icons/weather?code=X - Get weather icon design
static esp_err_t icon_weather_get_handler(httpd_req_t *req) {
    char icon_code[32];
    if (get_url_param(req, "code", icon_code, sizeof(icon_code)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing icon code");
        return ESP_FAIL;
    }

    char *json_str = ui_config_get_icon_json("weather", icon_code);

    if (json_str == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// POST /api/icons/weather - Save weather icon design
static esp_err_t icon_weather_post_handler(httpd_req_t *req) {
    char *buf = malloc(2048);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, 2047);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = ui_config_save_icon_json("weather", buf);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ==================== URI Handlers Registration ====================

static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_system_info = {
    .uri       = "/api/system/info",
    .method    = HTTP_GET,
    .handler   = system_info_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_system_restart = {
    .uri       = "/api/system/restart",
    .method    = HTTP_POST,
    .handler   = system_restart_handler,
    .user_ctx  = NULL
};

// WiFi endpoints
static const httpd_uri_t uri_wifi_get = {
    .uri       = "/api/wifi",
    .method    = HTTP_GET,
    .handler   = wifi_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_wifi_post = {
    .uri       = "/api/wifi",
    .method    = HTTP_POST,
    .handler   = wifi_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_wifi_scan = {
    .uri       = "/api/wifi/scan",
    .method    = HTTP_GET,
    .handler   = wifi_scan_handler,
    .user_ctx  = NULL
};

// Time endpoints
static const httpd_uri_t uri_time_get = {
    .uri       = "/api/time",
    .method    = HTTP_GET,
    .handler   = time_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_time_post = {
    .uri       = "/api/time",
    .method    = HTTP_POST,
    .handler   = time_post_handler,
    .user_ctx  = NULL
};

// Weather endpoints
static const httpd_uri_t uri_weather_get = {
    .uri       = "/api/weather",
    .method    = HTTP_GET,
    .handler   = weather_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_weather_post = {
    .uri       = "/api/weather",
    .method    = HTTP_POST,
    .handler   = weather_post_handler,
    .user_ctx  = NULL
};

// CGM endpoints
static const httpd_uri_t uri_cgm_get = {
    .uri       = "/api/cgm",
    .method    = HTTP_GET,
    .handler   = cgm_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_cgm_login = {
    .uri       = "/api/cgm/login",
    .method    = HTTP_POST,
    .handler   = cgm_login_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_cgm_logout = {
    .uri       = "/api/cgm/auth",
    .method    = HTTP_DELETE,
    .handler   = cgm_logout_handler,
    .user_ctx  = NULL
};

// UI Layout endpoints
static const httpd_uri_t uri_ui_layout_get = {
    .uri       = "/api/ui/layout",
    .method    = HTTP_GET,
    .handler   = ui_layout_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_ui_layout_post = {
    .uri       = "/api/ui/layout",
    .method    = HTTP_POST,
    .handler   = ui_layout_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_ui_defaults = {
    .uri       = "/api/ui/defaults",
    .method    = HTTP_GET,
    .handler   = ui_defaults_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_ui_screenshot = {
    .uri       = "/api/ui/screenshot",
    .method    = HTTP_GET,
    .handler   = ui_screenshot_handler,
    .user_ctx  = NULL
};

// Icon endpoints
static const httpd_uri_t uri_icon_trend_get = {
    .uri       = "/api/icons/trend",
    .method    = HTTP_GET,
    .handler   = icon_trend_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_icon_trend_post = {
    .uri       = "/api/icons/trend",
    .method    = HTTP_POST,
    .handler   = icon_trend_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_icon_weather_get = {
    .uri       = "/api/icons/weather",
    .method    = HTTP_GET,
    .handler   = icon_weather_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_icon_weather_post = {
    .uri       = "/api/icons/weather",
    .method    = HTTP_POST,
    .handler   = icon_weather_post_handler,
    .user_ctx  = NULL
};

// Catch-all for static files (must be registered last)
static const httpd_uri_t uri_static = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_handler,
    .user_ctx  = NULL
};

// ==================== SPIFFS Initialization ====================

static esp_err_t init_spiffs(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",  // Explicitly specify partition name
        .max_files = 10,
        .format_if_mount_failed = false  // Don't format - we want to know if flash failed
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SPIFFS - partition may not be flashed!");
            ESP_LOGE(TAG, "Run: idf.py -p COM40 erase-flash flash");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS partition not found in partition table!");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPIFFS mounted: total=%d KB, used=%d KB", total / 1024, used / 1024);

    // List files for debugging
    ESP_LOGI(TAG, "SPIFFS files:");
    DIR *dir = opendir("/spiffs");
    if (dir != NULL) {
        struct dirent *entry;
        int file_count = 0;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "  - %s", entry->d_name);
            file_count++;
        }
        closedir(dir);
        ESP_LOGI(TAG, "Total files found: %d", file_count);
    } else {
        ESP_LOGE(TAG, "Failed to open /spiffs directory");
    }

    return ESP_OK;
}

// ==================== Public Functions ====================

esp_err_t start_webserver(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    // Initialize SPIFFS
    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 4096;  // Reduced from 8192 to save memory
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;  // Enable LRU purge to free memory

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_system_info);
    httpd_register_uri_handler(server, &uri_system_restart);

    // WiFi endpoints
    httpd_register_uri_handler(server, &uri_wifi_get);
    httpd_register_uri_handler(server, &uri_wifi_post);
    httpd_register_uri_handler(server, &uri_wifi_scan);

    // Time endpoints
    httpd_register_uri_handler(server, &uri_time_get);
    httpd_register_uri_handler(server, &uri_time_post);

    // Weather endpoints
    httpd_register_uri_handler(server, &uri_weather_get);
    httpd_register_uri_handler(server, &uri_weather_post);

    // CGM endpoints
    httpd_register_uri_handler(server, &uri_cgm_get);
    httpd_register_uri_handler(server, &uri_cgm_login);
    httpd_register_uri_handler(server, &uri_cgm_logout);

    // UI Layout endpoints
    httpd_register_uri_handler(server, &uri_ui_layout_get);
    httpd_register_uri_handler(server, &uri_ui_layout_post);
    httpd_register_uri_handler(server, &uri_ui_defaults);
    httpd_register_uri_handler(server, &uri_ui_screenshot);

    // Icon endpoints
    httpd_register_uri_handler(server, &uri_icon_trend_get);
    httpd_register_uri_handler(server, &uri_icon_trend_post);
    httpd_register_uri_handler(server, &uri_icon_weather_get);
    httpd_register_uri_handler(server, &uri_icon_weather_post);

    // Register catch-all last
    httpd_register_uri_handler(server, &uri_static);

    ESP_LOGI(TAG, "Web server started successfully");
    return ESP_OK;
}

esp_err_t stop_webserver(void) {
    if (server == NULL) {
        ESP_LOGW(TAG, "Server not running");
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(server);
    if (ret == ESP_OK) {
        server = NULL;
        esp_vfs_spiffs_unregister(NULL);
        ESP_LOGI(TAG, "Web server stopped");
    }
    return ret;
}

httpd_handle_t get_webserver_handle(void) {
    return server;
}
