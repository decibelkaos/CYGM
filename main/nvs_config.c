#include "nvs_config.h"
#include "cgm_types.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CONFIG";
static const char *NVS_NAMESPACE = "cygm";
static const char *NVS_SSID_KEY = "wifi_ssid";
static const char *NVS_PASSWORD_KEY = "wifi_pass";
static const char *NVS_TIMEZONE_KEY = "timezone";
static const char *NVS_DST_KEY = "dst_enabled";
static const char *NVS_24HR_KEY = "format_24hr";
static const char *NVS_ZIPCODE_KEY = "zipcode";
static const char *NVS_TEMP_UNIT_KEY = "temp_celsius";
static const char *NVS_WEATHER_INTERVAL_KEY = "weather_int";
static const char *NVS_CGM_TYPE_KEY = "cgm_type";
static const char *NVS_BRIGHTNESS_KEY = "brightness";
static const char *NVS_DIM_ON_CHARGE_KEY = "dim_on_chg";

esp_err_t nvs_config_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_LOGW(TAG, "NVS partition needs erasing, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t nvs_save_wifi_credentials(const char *ssid, const char *password) {
    // Legacy function - now uses multi-network storage
    return nvs_add_wifi_network(ssid, password);
}

esp_err_t nvs_load_wifi_credentials(wifi_credentials_t *creds) {
    // Legacy function - loads first saved network
    if (creds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t count = 0;
    wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
    esp_err_t ret = nvs_get_saved_wifi_networks(networks, &count);

    if (ret == ESP_OK && count > 0) {
        memcpy(creds, &networks[0], sizeof(wifi_credentials_t));
        ESP_LOGI(TAG, "WiFi credentials loaded from NVS (SSID: %s)", creds->ssid);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

bool nvs_has_wifi_credentials(void) {
    return (nvs_get_saved_wifi_count() > 0);
}

esp_err_t nvs_clear_wifi_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Clear legacy keys
    nvs_erase_key(nvs_handle, NVS_SSID_KEY);
    nvs_erase_key(nvs_handle, NVS_PASSWORD_KEY);

    // Clear multi-network keys
    nvs_erase_key(nvs_handle, "wifi_count");
    for (int i = 0; i < MAX_SAVED_WIFI_NETWORKS; i++) {
        char ssid_key[16], pass_key[16];
        snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", i);
        snprintf(pass_key, sizeof(pass_key), "wifi_%d_pass", i);
        nvs_erase_key(nvs_handle, ssid_key);
        nvs_erase_key(nvs_handle, pass_key);
    }

    ret = nvs_commit(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

// ==================== Multi-Network WiFi Functions ====================

// Get number of saved WiFi networks
uint8_t nvs_get_saved_wifi_count(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    uint8_t count = 0;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return 0;
    }

    ret = nvs_get_u8(nvs_handle, "wifi_count", &count);
    nvs_close(nvs_handle);

    return (ret == ESP_OK) ? count : 0;
}

// Check if specific SSID is saved
bool nvs_is_wifi_saved(const char *ssid) {
    if (ssid == NULL) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return false;
    }

    uint8_t count = 0;
    ret = nvs_get_u8(nvs_handle, "wifi_count", &count);
    if (ret != ESP_OK || count == 0) {
        nvs_close(nvs_handle);
        return false;
    }

    for (int i = 0; i < count && i < MAX_SAVED_WIFI_NETWORKS; i++) {
        char ssid_key[16];
        char saved_ssid[MAX_SSID_LEN];
        size_t ssid_len = MAX_SSID_LEN;

        snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", i);
        ret = nvs_get_str(nvs_handle, ssid_key, saved_ssid, &ssid_len);

        if (ret == ESP_OK && strcmp(saved_ssid, ssid) == 0) {
            nvs_close(nvs_handle);
            return true;
        }
    }

    nvs_close(nvs_handle);
    return false;
}

// Save last successfully connected WiFi SSID
esp_err_t nvs_save_last_wifi_ssid(const char *ssid) {
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs_handle, "last_wifi", ssid);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Last connected WiFi saved: %s", ssid);
    return ret;
}

// Load last successfully connected WiFi SSID
esp_err_t nvs_load_last_wifi_ssid(char *ssid, size_t max_len) {
    if (ssid == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = max_len;
    ret = nvs_get_str(nvs_handle, "last_wifi", ssid, &len);
    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Last connected WiFi loaded: %s", ssid);
    }

    return ret;
}

// Get all saved WiFi networks
esp_err_t nvs_get_saved_wifi_networks(wifi_credentials_t *networks, uint8_t *count) {
    if (networks == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t saved_count = 0;
    ret = nvs_get_u8(nvs_handle, "wifi_count", &saved_count);
    if (ret != ESP_OK || saved_count == 0) {
        nvs_close(nvs_handle);
        return (ret == ESP_OK) ? ESP_OK : ret;
    }

    // Load each saved network (pack contiguously — skip failed slots)
    for (int i = 0; i < saved_count && i < MAX_SAVED_WIFI_NETWORKS; i++) {
        char ssid_key[16], pass_key[16];
        size_t ssid_len = MAX_SSID_LEN;
        size_t pass_len = MAX_PASSWORD_LEN;

        snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", i);
        snprintf(pass_key, sizeof(pass_key), "wifi_%d_pass", i);

        ret = nvs_get_str(nvs_handle, ssid_key, networks[*count].ssid, &ssid_len);
        if (ret != ESP_OK) continue;

        ret = nvs_get_str(nvs_handle, pass_key, networks[*count].password, &pass_len);
        if (ret != ESP_OK) continue;

        (*count)++;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded %d saved WiFi networks from NVS", *count);
    return ESP_OK;
}

// Add or update WiFi network (max 3)
esp_err_t nvs_add_wifi_network(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t count = 0;
    nvs_get_u8(nvs_handle, "wifi_count", &count);

    // Check if network already exists (update password)
    int existing_index = -1;
    for (int i = 0; i < count && i < MAX_SAVED_WIFI_NETWORKS; i++) {
        char ssid_key[16];
        char saved_ssid[MAX_SSID_LEN];
        size_t ssid_len = MAX_SSID_LEN;

        snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", i);
        ret = nvs_get_str(nvs_handle, ssid_key, saved_ssid, &ssid_len);

        if (ret == ESP_OK && strcmp(saved_ssid, ssid) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index >= 0) {
        char pass_key[16];
        snprintf(pass_key, sizeof(pass_key), "wifi_%d_pass", existing_index);
        ret = nvs_set_str(nvs_handle, pass_key, password);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Updated WiFi network: %s", ssid);
        }
        nvs_close(nvs_handle);
        return ret;
    }

    if (count >= MAX_SAVED_WIFI_NETWORKS) {
        ESP_LOGW(TAG, "Cannot add WiFi network - maximum %d networks reached", MAX_SAVED_WIFI_NETWORKS);
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    char ssid_key[16], pass_key[16];
    snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", count);
    snprintf(pass_key, sizeof(pass_key), "wifi_%d_pass", count);

    ret = nvs_set_str(nvs_handle, ssid_key, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_str(nvs_handle, pass_key, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    count++;
    ret = nvs_set_u8(nvs_handle, "wifi_count", count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi count: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Added WiFi network: %s (%d/%d)", ssid, count, MAX_SAVED_WIFI_NETWORKS);
    }

    nvs_close(nvs_handle);
    return ret;
}

// Remove WiFi network by SSID
esp_err_t nvs_remove_wifi_network(const char *ssid) {
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t count = 0;
    ret = nvs_get_u8(nvs_handle, "wifi_count", &count);
    if (ret != ESP_OK || count == 0) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    int remove_index = -1;
    for (int i = 0; i < count && i < MAX_SAVED_WIFI_NETWORKS; i++) {
        char ssid_key[16];
        char saved_ssid[MAX_SSID_LEN];
        size_t ssid_len = MAX_SSID_LEN;

        snprintf(ssid_key, sizeof(ssid_key), "wifi_%d_ssid", i);
        ret = nvs_get_str(nvs_handle, ssid_key, saved_ssid, &ssid_len);

        if (ret == ESP_OK && strcmp(saved_ssid, ssid) == 0) {
            remove_index = i;
            break;
        }
    }

    if (remove_index < 0) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Shift remaining networks down to fill the gap
    for (int i = remove_index; i < count - 1; i++) {
        char src_ssid_key[16], src_pass_key[16];
        char dst_ssid_key[16], dst_pass_key[16];
        char temp_ssid[MAX_SSID_LEN], temp_pass[MAX_PASSWORD_LEN];
        size_t ssid_len = MAX_SSID_LEN;
        size_t pass_len = MAX_PASSWORD_LEN;

        snprintf(src_ssid_key, sizeof(src_ssid_key), "wifi_%d_ssid", i + 1);
        snprintf(src_pass_key, sizeof(src_pass_key), "wifi_%d_pass", i + 1);

        nvs_get_str(nvs_handle, src_ssid_key, temp_ssid, &ssid_len);
        nvs_get_str(nvs_handle, src_pass_key, temp_pass, &pass_len);

        snprintf(dst_ssid_key, sizeof(dst_ssid_key), "wifi_%d_ssid", i);
        snprintf(dst_pass_key, sizeof(dst_pass_key), "wifi_%d_pass", i);

        nvs_set_str(nvs_handle, dst_ssid_key, temp_ssid);
        nvs_set_str(nvs_handle, dst_pass_key, temp_pass);
    }

    char last_ssid_key[16], last_pass_key[16];
    snprintf(last_ssid_key, sizeof(last_ssid_key), "wifi_%d_ssid", count - 1);
    snprintf(last_pass_key, sizeof(last_pass_key), "wifi_%d_pass", count - 1);
    nvs_erase_key(nvs_handle, last_ssid_key);
    nvs_erase_key(nvs_handle, last_pass_key);

    count--;
    nvs_set_u8(nvs_handle, "wifi_count", count);

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Removed WiFi network: %s (%d remaining)", ssid, count);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_save_time_settings(const char *timezone, bool dst_enabled, bool format_24hr) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_TIMEZONE_KEY, timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save timezone: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_DST_KEY, dst_enabled ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save DST setting: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_24HR_KEY, format_24hr ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save 24hr format: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Time settings saved to NVS (TZ: %s, DST: %d, 24hr: %d)",
                 timezone, dst_enabled, format_24hr);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_time_settings(time_settings_t *settings) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    size_t tz_len = MAX_TIMEZONE_LEN;
    uint8_t dst_val = 0;
    uint8_t format_val = 0;

    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_TIMEZONE_KEY, settings->timezone, &tz_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_u8(nvs_handle, NVS_DST_KEY, &dst_val);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }
    settings->dst_enabled = (dst_val != 0);

    ret = nvs_get_u8(nvs_handle, NVS_24HR_KEY, &format_val);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }
    settings->format_24hr = (format_val != 0);

    ESP_LOGI(TAG, "Time settings loaded from NVS (TZ: %s, DST: %d, 24hr: %d)",
             settings->timezone, settings->dst_enabled, settings->format_24hr);

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_time_settings(void) {
    time_settings_t settings;
    return (nvs_load_time_settings(&settings) == ESP_OK);
}

esp_err_t nvs_save_weather_settings(const char *zipcode, bool temp_celsius, uint8_t update_interval_min) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_ZIPCODE_KEY, zipcode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save zipcode: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_TEMP_UNIT_KEY, temp_celsius ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save temp unit: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_WEATHER_INTERVAL_KEY, update_interval_min);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save weather interval: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Weather settings saved to NVS (Zipcode: %s, Unit: %s, Interval: %d min)",
                 zipcode, temp_celsius ? "C" : "F", update_interval_min);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_save_weather_coords(const char *zipcode, float latitude, float longitude, const char *location) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Store coordinates as integers (micro-degrees) to avoid float precision issues
    int32_t lat_i32 = (int32_t)(latitude * 1000000.0f);
    int32_t lon_i32 = (int32_t)(longitude * 1000000.0f);

    ret = nvs_set_i32(nvs_handle, "weather_lat", lat_i32);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save latitude: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_i32(nvs_handle, "weather_lon", lon_i32);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save longitude: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Save the zipcode that was geocoded (to detect changes)
    ret = nvs_set_str(nvs_handle, "wx_zip_cached", zipcode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save geocoded zipcode: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Save location name (e.g., "Bethel, ME")
    ret = nvs_set_str(nvs_handle, "wx_location", location);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save location: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Weather location saved to NVS: %s (%.4f, %.4f, zipcode: %s)", location, latitude, longitude, zipcode);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_weather_settings(weather_settings_t *settings) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    size_t zip_len = MAX_ZIPCODE_LEN;
    uint8_t temp_val = 0;
    uint8_t interval_val = 5;  // Default 5 minutes

    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_ZIPCODE_KEY, settings->zipcode, &zip_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_u8(nvs_handle, NVS_TEMP_UNIT_KEY, &temp_val);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }
    settings->temp_celsius = (temp_val != 0);

    // Load update interval (default to 5 minutes if not found)
    ret = nvs_get_u8(nvs_handle, NVS_WEATHER_INTERVAL_KEY, &interval_val);
    if (ret != ESP_OK) {
        interval_val = 5;
    }
    // Clamp to valid range (5-90 minutes)
    if (interval_val < 5) interval_val = 5;
    if (interval_val > 90) interval_val = 90;
    settings->update_interval_min = interval_val;

    // Load cached coordinates and location (if available)
    int32_t lat_i32 = 0, lon_i32 = 0;
    size_t geocoded_zip_len = MAX_ZIPCODE_LEN;
    size_t location_len = MAX_LOCATION_LEN;
    if (nvs_get_i32(nvs_handle, "weather_lat", &lat_i32) == ESP_OK &&
        nvs_get_i32(nvs_handle, "weather_lon", &lon_i32) == ESP_OK &&
        nvs_get_str(nvs_handle, "wx_zip_cached", settings->geocoded_zipcode, &geocoded_zip_len) == ESP_OK) {
        settings->latitude = (float)lat_i32 / 1000000.0f;   // Convert from micro-degrees
        settings->longitude = (float)lon_i32 / 1000000.0f;
        settings->coords_valid = true;

        if (nvs_get_str(nvs_handle, "wx_location", settings->location, &location_len) == ESP_OK) {
            ESP_LOGI(TAG, "Cached location loaded: %s (%.4f, %.4f, zipcode: %s)",
                     settings->location, settings->latitude, settings->longitude, settings->geocoded_zipcode);
        } else {
            settings->location[0] = '\0';  // Empty string
            ESP_LOGI(TAG, "Cached coordinates loaded: %.4f, %.4f (for zipcode: %s)",
                     settings->latitude, settings->longitude, settings->geocoded_zipcode);
        }
    } else {
        settings->coords_valid = false;
        settings->geocoded_zipcode[0] = '\0';  // Empty string
        settings->location[0] = '\0';  // Empty string
    }

    ESP_LOGI(TAG, "Weather settings loaded from NVS (Zipcode: %s, Unit: %s, Interval: %d min)",
             settings->zipcode, settings->temp_celsius ? "C" : "F", settings->update_interval_min);

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_weather_settings(void) {
    weather_settings_t settings;
    return (nvs_load_weather_settings(&settings) == ESP_OK);
}

esp_err_t nvs_save_cgm_type(const char *cgm_type) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_CGM_TYPE_KEY, cgm_type);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save CGM type: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "CGM type saved to NVS: %s", cgm_type);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_cgm_type(char *cgm_type, size_t len) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    size_t type_len = len;

    if (cgm_type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_CGM_TYPE_KEY, cgm_type, &type_len);
    nvs_close(nvs_handle);
    return ret;
}


// ==================== Last-Seen Firmware Version ====================
// Drives the post-update "What's New" card: differs from the running version
// exactly once after each firmware update.

#define NVS_SEEN_VERSION_KEY "seen_ver"

esp_err_t nvs_save_seen_version(const char *version) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_SEEN_VERSION_KEY, version);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_seen_version(char *version, size_t len) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    size_t ver_len = len;

    if (version == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    version[0] = '\0';

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_SEEN_VERSION_KEY, version, &ver_len);
    nvs_close(nvs_handle);
    return ret;
}


// ==================== Dexcom Share Credentials ====================

#define NVS_DEXCOM_USER_KEY "dex_user"
#define NVS_DEXCOM_PASS_KEY "dex_pass"

esp_err_t nvs_set_dexcom_credentials(const char *username, const char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_DEXCOM_USER_KEY, username);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_DEXCOM_PASS_KEY, password);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Dexcom credentials saved to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_get_dexcom_credentials(char *username, size_t user_len, char *password, size_t pass_len) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_DEXCOM_USER_KEY, username, &user_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_DEXCOM_PASS_KEY, password, &pass_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_dexcom_credentials(void) {
    char user[64], pass[64];
    return (nvs_get_dexcom_credentials(user, sizeof(user), pass, sizeof(pass)) == ESP_OK);
}

esp_err_t nvs_clear_dexcom_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_key(nvs_handle, NVS_DEXCOM_USER_KEY);
    nvs_erase_key(nvs_handle, NVS_DEXCOM_PASS_KEY);
    ret = nvs_commit(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Dexcom credentials cleared from NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

// ==================== LibreLinkUp Credentials ====================

#define NVS_LIBRE_EMAIL_KEY "libre_email"
#define NVS_LIBRE_PASS_KEY  "libre_pass"
#define NVS_LIBRE_TOKEN_KEY "libre_token"
#define NVS_LIBRE_PAT_KEY   "libre_pat_id"
#define NVS_LIBRE_REGION_KEY "libre_region"
#define NVS_LIBRE_EXP_KEY   "libre_tk_exp"
#define NVS_LIBRE_ACCT_KEY  "libre_acct_id"

esp_err_t nvs_set_libre_credentials(const char *email, const char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_LIBRE_EMAIL_KEY, email);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_LIBRE_PASS_KEY, password);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Libre credentials saved to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_get_libre_credentials(char *email, size_t email_len, char *password, size_t pass_len) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_LIBRE_EMAIL_KEY, email, &email_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_LIBRE_PASS_KEY, password, &pass_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_libre_credentials(void) {
    char email[64], pass[64];
    return (nvs_get_libre_credentials(email, sizeof(email), pass, sizeof(pass)) == ESP_OK);
}

esp_err_t nvs_clear_libre_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_key(nvs_handle, NVS_LIBRE_EMAIL_KEY);
    nvs_erase_key(nvs_handle, NVS_LIBRE_PASS_KEY);
    ret = nvs_commit(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Libre credentials cleared from NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_save_libre_session(const char *token, const char *patient_id,
                                  const char *region_url, int32_t token_expires,
                                  const char *account_id) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    if (token != NULL) {
        ret = nvs_set_str(nvs_handle, NVS_LIBRE_TOKEN_KEY, token);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (patient_id != NULL) {
        ret = nvs_set_str(nvs_handle, NVS_LIBRE_PAT_KEY, patient_id);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (region_url != NULL) {
        ret = nvs_set_str(nvs_handle, NVS_LIBRE_REGION_KEY, region_url);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    ret = nvs_set_i32(nvs_handle, NVS_LIBRE_EXP_KEY, token_expires);
    if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }

    if (account_id != NULL && account_id[0] != '\0') {
        ret = nvs_set_str(nvs_handle, NVS_LIBRE_ACCT_KEY, account_id);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Libre session saved to NVS (expires=%ld)", (long)token_expires);
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_libre_session(char *token, size_t token_max,
                                  char *patient_id, size_t pid_max,
                                  char *region_url, size_t region_max,
                                  int32_t *token_expires,
                                  char *account_id, size_t acct_max) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (token != NULL) {
        ret = nvs_get_str(nvs_handle, NVS_LIBRE_TOKEN_KEY, token, &token_max);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (patient_id != NULL) {
        ret = nvs_get_str(nvs_handle, NVS_LIBRE_PAT_KEY, patient_id, &pid_max);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (region_url != NULL) {
        ret = nvs_get_str(nvs_handle, NVS_LIBRE_REGION_KEY, region_url, &region_max);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (token_expires != NULL) {
        ret = nvs_get_i32(nvs_handle, NVS_LIBRE_EXP_KEY, token_expires);
        if (ret != ESP_OK) { nvs_close(nvs_handle); return ret; }
    }

    if (account_id != NULL) {
        ret = nvs_get_str(nvs_handle, NVS_LIBRE_ACCT_KEY, account_id, &acct_max);
        if (ret != ESP_OK) {
            // account_id is optional — older sessions won't have it
            account_id[0] = '\0';
        }
    }

    ESP_LOGI(TAG, "Libre session loaded from NVS");
    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t nvs_clear_libre_session(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_key(nvs_handle, NVS_LIBRE_TOKEN_KEY);
    nvs_erase_key(nvs_handle, NVS_LIBRE_PAT_KEY);
    nvs_erase_key(nvs_handle, NVS_LIBRE_REGION_KEY);
    nvs_erase_key(nvs_handle, NVS_LIBRE_EXP_KEY);
    nvs_erase_key(nvs_handle, NVS_LIBRE_ACCT_KEY);
    ret = nvs_commit(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Libre session cleared from NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

// ==================== Nightscout Credentials ====================

#define NVS_NS_URL_KEY   "ns_url"
#define NVS_NS_TOKEN_KEY "ns_token"

esp_err_t nvs_set_nightscout_credentials(const char *url, const char *token) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, NVS_NS_URL_KEY, url);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    // Token may be empty for public instances — store empty string
    ret = nvs_set_str(nvs_handle, NVS_NS_TOKEN_KEY, token ? token : "");
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Nightscout credentials saved to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_get_nightscout_credentials(char *url, size_t url_len, char *token, size_t token_len) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_NS_URL_KEY, url, &url_len);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, NVS_NS_TOKEN_KEY, token, &token_len);
    if (ret != ESP_OK) {
        // Token is optional — not an error if missing
        token[0] = '\0';
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_nightscout_credentials(void) {
    // Separate buffers: the token is read AFTER the url, so sharing one buffer
    // let an empty token (public sites) blank the url and report "no
    // credentials" on every boot — token-less users had to re-enter settings
    // after any power loss.
    char url[128];
    char token[64];
    return (nvs_get_nightscout_credentials(url, sizeof(url), token, sizeof(token)) == ESP_OK
            && url[0] != '\0');
}

esp_err_t nvs_clear_nightscout_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_key(nvs_handle, NVS_NS_URL_KEY);
    nvs_erase_key(nvs_handle, NVS_NS_TOKEN_KEY);
    ret = nvs_commit(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Nightscout credentials cleared from NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

// ============================================================================
// CGM Alarm Settings
// ============================================================================

#define NVS_ALARMS_KEY "cgm_alarms"

// Get default alarm settings (called when no settings exist)
void nvs_get_default_alarm_settings(cgm_alarms_t *alarms) {
    if (alarms == NULL) return;

    // High Alarm (urgent) - Default: Red, 250 mg/dL
    alarms->high_alarm.enabled = true;
    alarms->high_alarm.threshold = 250;
    alarms->high_alarm.text_color = 0xFF3333;  // Red
    alarms->high_alarm.audio_enabled = true;
    alarms->high_alarm.visual_enabled = true;
    alarms->high_alarm.led_enabled = true;
    alarms->high_alarm.tone = ALARM_TONE_BEEP_3;
    alarms->high_alarm.volume = 80;
    alarms->high_alarm.audio_repeat = true;  // Urgent alarms repeat

    // High Warning (precautionary) - Default: Amber, 180 mg/dL
    alarms->high_warning.enabled = true;
    alarms->high_warning.threshold = 180;
    alarms->high_warning.text_color = 0xFFAA00;  // Amber/Orange
    alarms->high_warning.audio_enabled = true;
    alarms->high_warning.visual_enabled = true;
    alarms->high_warning.led_enabled = true;
    alarms->high_warning.tone = ALARM_TONE_CHIME;
    alarms->high_warning.volume = 60;
    alarms->high_warning.audio_repeat = false;  // Warnings play once

    // Low Warning (precautionary) - Default: Amber, 80 mg/dL
    alarms->low_warning.enabled = true;
    alarms->low_warning.threshold = 80;
    alarms->low_warning.text_color = 0xFFAA00;  // Amber/Orange
    alarms->low_warning.audio_enabled = true;
    alarms->low_warning.visual_enabled = true;
    alarms->low_warning.led_enabled = true;
    alarms->low_warning.tone = ALARM_TONE_CHIME;
    alarms->low_warning.volume = 60;
    alarms->low_warning.audio_repeat = false;  // Warnings play once

    // Low Alarm (urgent) - Default: Red, 55 mg/dL
    alarms->low_alarm.enabled = true;
    alarms->low_alarm.threshold = 55;
    alarms->low_alarm.text_color = 0xFF3333;  // Red
    alarms->low_alarm.audio_enabled = true;
    alarms->low_alarm.visual_enabled = true;
    alarms->low_alarm.led_enabled = true;
    alarms->low_alarm.tone = ALARM_TONE_BEEP_3;
    alarms->low_alarm.volume = 80;
    alarms->low_alarm.audio_repeat = true;  // Urgent alarms repeat
}

esp_err_t nvs_save_alarm_settings(const cgm_alarms_t *alarms) {
    if (alarms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for alarm settings");
        return ret;
    }

    ret = nvs_set_blob(nvs_handle, NVS_ALARMS_KEY, alarms, sizeof(cgm_alarms_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save alarm settings to NVS");
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Alarm settings saved to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_alarm_settings(cgm_alarms_t *alarms) {
    if (alarms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading alarm settings, using defaults");
        nvs_get_default_alarm_settings(alarms);
        return ret;
    }

    size_t required_size = sizeof(cgm_alarms_t);
    ret = nvs_get_blob(nvs_handle, NVS_ALARMS_KEY, alarms, &required_size);

    if (ret != ESP_OK || required_size != sizeof(cgm_alarms_t)) {
        ESP_LOGW(TAG, "Alarm settings not found or corrupted, using defaults");
        nvs_get_default_alarm_settings(alarms);
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Alarm settings loaded from NVS");
    nvs_close(nvs_handle);
    return ESP_OK;
}

bool nvs_has_alarm_settings(void) {
    cgm_alarms_t temp;
    return (nvs_load_alarm_settings(&temp) == ESP_OK);
}

// ========== Brightness Settings ==========

esp_err_t nvs_save_brightness(uint8_t brightness_percent) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing brightness");
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_BRIGHTNESS_KEY, brightness_percent);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Brightness saved to NVS: %d%%", brightness_percent);
        }
    } else {
        ESP_LOGE(TAG, "Failed to save brightness to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t nvs_load_brightness(uint8_t *brightness_percent) {
    if (brightness_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading brightness, using default 80%%");
        *brightness_percent = 80;  // Default brightness
        return ret;
    }

    ret = nvs_get_u8(nvs_handle, NVS_BRIGHTNESS_KEY, brightness_percent);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Brightness not found in NVS, using default 80%%");
        *brightness_percent = 80;  // Default brightness
    } else {
        // Enforce minimum 1%
        if (*brightness_percent < 1) {
            *brightness_percent = 1;
        }
        ESP_LOGI(TAG, "Brightness loaded from NVS: %d%%", *brightness_percent);
    }

    nvs_close(nvs_handle);
    return ESP_OK;  // Always return OK, with default if not found
}

esp_err_t nvs_save_dim_while_charging(bool enabled) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing dim-while-charging");
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_DIM_ON_CHARGE_KEY, enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Dim-while-charging saved to NVS: %s", enabled ? "on" : "off");
        }
    } else {
        ESP_LOGE(TAG, "Failed to save dim-while-charging to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

bool nvs_load_dim_while_charging(void) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;  // Default: stay awake on the charger
    }

    uint8_t enabled = 0;
    nvs_get_u8(nvs_handle, NVS_DIM_ON_CHARGE_KEY, &enabled);
    nvs_close(nvs_handle);
    return enabled != 0;
}

// ========== Legal Disclaimer Acceptance ==========

esp_err_t nvs_set_disclaimer_accepted(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing disclaimer");
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, "disclaim_ok", 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Disclaimer acceptance saved to NVS");
        }
    }

    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_disclaimer_accepted(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return false;
    }

    uint8_t val = 0;
    ret = nvs_get_u8(nvs_handle, "disclaim_ok", &val);
    nvs_close(nvs_handle);

    return (ret == ESP_OK && val != 0);
}

// ========== SD Card Glucose Logging Toggle ==========

esp_err_t nvs_save_sd_glucose_logging(bool enabled) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing SD glucose logging");
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, "sd_glu_log", enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD glucose logging saved to NVS: %s", enabled ? "ON" : "OFF");
        }
    }

    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_sd_glucose_logging(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return true;  // Default: enabled (backward compat)
    }

    uint8_t val = 1;  // Default: enabled
    ret = nvs_get_u8(nvs_handle, "sd_glu_log", &val);
    nvs_close(nvs_handle);

    // If key not found, default to enabled (backward compat)
    return (ret != ESP_OK) ? true : (val != 0);
}

// ========== SD Serial Capture Toggle ==========

esp_err_t nvs_save_sd_serial_capture(bool enabled) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing SD serial capture");
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, "sd_ser_cap", enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD serial capture saved to NVS: %s", enabled ? "ON" : "OFF");
        }
    }

    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_sd_serial_capture(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return false;  // Default: disabled (don't auto-log to SD)
    }

    uint8_t val = 0;  // Default: disabled
    ret = nvs_get_u8(nvs_handle, "sd_ser_cap", &val);
    nvs_close(nvs_handle);

    return (ret != ESP_OK) ? false : (val != 0);
}

// ========== Factory Reset ==========
esp_err_t nvs_factory_reset(void) {
    ESP_LOGW(TAG, "FACTORY RESET: Erasing all NVS data");
    esp_err_t ret = nvs_flash_erase();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS erased successfully — rebooting");
    } else {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// ========== Welcome Screen Shown Flag ==========
esp_err_t nvs_set_welcome_shown(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(nvs_handle, "welcome_done", 1);
    if (ret == ESP_OK) ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_welcome_shown(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) return false;
    uint8_t val = 0;
    ret = nvs_get_u8(nvs_handle, "welcome_done", &val);
    nvs_close(nvs_handle);
    return (ret == ESP_OK && val != 0);
}

// ========== Locale: Glucose Units (mg/dL vs mmol/L) ==========
// Stored canonical mg/dL everywhere; this flag only affects display.
// Default false = mg/dL (preserves behavior for existing US installs).
esp_err_t nvs_save_glucose_mmol(bool mmol) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(nvs_handle, "glu_mmol", mmol ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Glucose units saved: %s", mmol ? "mmol/L" : "mg/dL");
    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_glucose_mmol(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) return false;  // Default: mg/dL
    uint8_t val = 0;
    ret = nvs_get_u8(nvs_handle, "glu_mmol", &val);
    nvs_close(nvs_handle);
    return (ret == ESP_OK && val != 0);
}

// ========== Locale: Date Format (US month-day vs day-month) ==========
// Default false = US "Mon, Jun 16"; true = rest-of-world "Mon, 16 Jun".
esp_err_t nvs_save_date_dmy(bool dmy) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(nvs_handle, "date_dmy", dmy ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Date format saved: %s", dmy ? "D/M" : "M/D");
    nvs_close(nvs_handle);
    return ret;
}

bool nvs_get_date_dmy(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) return false;  // Default: US M/D
    uint8_t val = 0;
    ret = nvs_get_u8(nvs_handle, "date_dmy", &val);
    nvs_close(nvs_handle);
    return (ret == ESP_OK && val != 0);
}

// ========== Dexcom region cache ==========
// Caches the region that last authenticated so login is a single attempt rather
// than a US -> OUS -> JP probe, where one wrong password means three failed
// logins and risks rate-limiting. dexcom_region_t int; -1 = unknown, full probe.
esp_err_t nvs_save_dexcom_region(int region) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_i8(nvs_handle, "dex_region", (int8_t)region);
    if (ret == ESP_OK) ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Dexcom region cached: %d", region);
    nvs_close(nvs_handle);
    return ret;
}

int nvs_get_dexcom_region(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) return -1;  // Unknown -> full probe
    int8_t val = -1;
    ret = nvs_get_i8(nvs_handle, "dex_region", &val);
    nvs_close(nvs_handle);
    return (ret == ESP_OK) ? (int)val : -1;
}

// ========== Migration-tolerant blob helpers ==========
// Shared by the alarm-extension and night-schedule blobs. The caller's struct is
// already seeded with defaults; we overlay only the bytes actually stored, so a
// short, long or unreadable blob NEVER wipes a setting. A silent reset on size
// mismatch is exactly what erased every user's alarm configuration once before.

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Overlay a stored blob onto *dst. Returns ESP_OK when stored bytes were applied,
// ESP_ERR_NVS_NOT_FOUND on every keep-the-defaults path. max_blob bounds the stack
// scratch buffer; a stored blob larger than that is treated as corrupt, not read.
static esp_err_t nvs_overlay_blob(const char *key, void *dst, size_t dst_size, size_t max_blob) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Blob '%s': NVS open failed (%s), keeping defaults", key, esp_err_to_name(ret));
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t stored = 0;
    ret = nvs_get_blob(nvs_handle, key, NULL, &stored);
    if (ret != ESP_OK || stored == 0) {
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Blob '%s': none stored, keeping defaults", key);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (stored > max_blob) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG, "Blob '%s': implausible size %u, keeping defaults", key, (unsigned)stored);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    // nvs_get_blob refuses a short destination, so read the whole stored blob
    // and copy only what fits — a newer firmware's longer blob is truncated,
    // an older firmware's shorter one leaves the tail fields at their defaults.
    uint8_t raw[64];
    if (max_blob > sizeof(raw)) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Blob '%s': scratch buffer too small", key);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t len = stored;
    ret = nvs_get_blob(nvs_handle, key, raw, &len);
    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Blob '%s': read failed (%s), keeping defaults", key, esp_err_to_name(ret));
        return ESP_ERR_NVS_NOT_FOUND;
    }

    memcpy(dst, raw, (len < dst_size) ? len : dst_size);
    ESP_LOGI(TAG, "Blob '%s': applied %u stored bytes over defaults", key, (unsigned)len);
    return ESP_OK;
}

static esp_err_t nvs_write_blob(const char *key, const void *src, size_t size) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Blob '%s': NVS open failed (%s)", key, esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_blob(nvs_handle, key, src, size);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Blob '%s': write failed (%s)", key, esp_err_to_name(ret));
    }
    nvs_close(nvs_handle);
    return ret;
}

// ========== Alarm engine extension blob (layout frozen in cgm_types.h) ==========

esp_err_t nvs_save_alarm_ext(const cygm_alarm_ext_t *ext) {
    if (ext == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Stamp the identity fields at write time so a later build can tell what it
    // is reading without trusting whatever the caller left in them.
    cygm_alarm_ext_t blob = *ext;
    blob.version = CYGM_ALARM_EXT_VERSION;
    blob.size    = (uint16_t)sizeof(cygm_alarm_ext_t);

    esp_err_t ret = nvs_write_blob(CYGM_ALARM_EXT_NVS_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Alarm engine settings saved (v%u, %u bytes)",
                 (unsigned)blob.version, (unsigned)blob.size);
    }
    return ret;
}

esp_err_t nvs_load_alarm_ext(cygm_alarm_ext_t *ext) {
    if (ext == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cygm_alarm_ext_defaults(ext);
    esp_err_t ret = nvs_overlay_blob(CYGM_ALARM_EXT_NVS_KEY, ext, sizeof(*ext), 64);

    // Restamp identity, then clamp every field to the range documented beside it in
    // cgm_types.h. Clamping is per-field so one bad byte cannot cost the others.
    ext->version = CYGM_ALARM_EXT_VERSION;
    ext->size    = (uint16_t)sizeof(cygm_alarm_ext_t);

    ext->quiet_enabled       = ext->quiet_enabled ? 1 : 0;
    ext->quiet_start_hour    = clamp_u8(ext->quiet_start_hour, 0, 23);
    ext->quiet_start_min     = clamp_u8(ext->quiet_start_min, 0, 59);
    ext->quiet_end_hour      = clamp_u8(ext->quiet_end_hour, 0, 23);
    ext->quiet_end_min       = clamp_u8(ext->quiet_end_min, 0, 59);
    ext->escalate_enabled    = ext->escalate_enabled ? 1 : 0;
    ext->escalate_step_min   = clamp_u8(ext->escalate_step_min, 1, 30);
    ext->escalate_max_volume = clamp_u8(ext->escalate_max_volume, 10, 100);
    ext->gap_enabled         = ext->gap_enabled ? 1 : 0;
    ext->gap_minutes         = clamp_u8(ext->gap_minutes, 10, 60);
    ext->gap_tone            = clamp_u8(ext->gap_tone, 0, (uint8_t)(ALARM_TONE_COUNT - 1));
    ext->gap_volume          = clamp_u8(ext->gap_volume, 0, 100);
    ext->predict_enabled     = ext->predict_enabled ? 1 : 0;
    ext->predict_horizon_min = clamp_u8(ext->predict_horizon_min, 5, 45);
    ext->rate_enabled        = ext->rate_enabled ? 1 : 0;
    ext->rate_threshold_x10  = clamp_u8(ext->rate_threshold_x10, 10, 60);
    ext->suppress_min        = clamp_u8(ext->suppress_min, 5, 120);
    ext->snooze_default_min  = clamp_u8(ext->snooze_default_min, 5, 120);
    // Life safety: the urgent-low guard has a floor and a ceiling, never "off".
    ext->urgent_low_floor    = clamp_u8(ext->urgent_low_floor, 40, 90);
    ext->auto_snooze_disabled = ext->auto_snooze_disabled ? 1 : 0;

    return ret;
}

// ========== Scheduled night dim ==========

static void night_cfg_defaults(cygm_night_cfg_t *cfg) {
    cfg->enabled          = 0;
    cfg->start_hour       = 21;
    cfg->start_min        = 30;
    cfg->end_hour         = 6;
    cfg->end_min          = 30;
    cfg->night_brightness = 15;
}

esp_err_t nvs_save_night_cfg(const cygm_night_cfg_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_write_blob("night_cfg", cfg, sizeof(*cfg));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Night dim saved: %s %02d:%02d-%02d:%02d @ %d%%",
                 cfg->enabled ? "on" : "off", cfg->start_hour, cfg->start_min,
                 cfg->end_hour, cfg->end_min, cfg->night_brightness);
    }
    return ret;
}

esp_err_t nvs_load_night_cfg(cygm_night_cfg_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    night_cfg_defaults(cfg);
    esp_err_t ret = nvs_overlay_blob("night_cfg", cfg, sizeof(*cfg), 64);

    cfg->enabled          = cfg->enabled ? 1 : 0;
    cfg->start_hour       = clamp_u8(cfg->start_hour, 0, 23);
    cfg->start_min        = clamp_u8(cfg->start_min, 0, 59);
    cfg->end_hour         = clamp_u8(cfg->end_hour, 0, 23);
    cfg->end_min          = clamp_u8(cfg->end_min, 0, 59);
    cfg->night_brightness = clamp_u8(cfg->night_brightness, 1, 100);

    return ret;
}

