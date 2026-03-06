#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>

#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_TIMEZONE_LEN 64
#define MAX_ZIPCODE_LEN 11

// WiFi credentials structure
typedef struct {
    char ssid[MAX_SSID_LEN];
    char password[MAX_PASSWORD_LEN];
} wifi_credentials_t;

// Time settings structure
typedef struct {
    char timezone[MAX_TIMEZONE_LEN];  // IANA timezone (e.g., "America/New_York")
    bool dst_enabled;                  // Daylight Saving Time enabled
    bool format_24hr;                  // true = 24hr, false = 12hr
} time_settings_t;

// Weather settings structure
#define MAX_LOCATION_LEN 64
typedef struct {
    char zipcode[MAX_ZIPCODE_LEN];    // US zipcode
    bool temp_celsius;                 // Temperature unit (false = F, true = C)
    uint8_t update_interval_min;       // Weather update interval (5-90 minutes, default 5)
    float latitude;                    // Cached latitude (from geocoding)
    float longitude;                   // Cached longitude (from geocoding)
    char geocoded_zipcode[MAX_ZIPCODE_LEN]; // Zipcode that was geocoded (to detect changes)
    char location[MAX_LOCATION_LEN];   // Location name (e.g., "Bethel, ME")
    bool coords_valid;                 // True if lat/lon have been geocoded
    bool use_postal_code_entry;        // true = postal code, false = city dropdown
} weather_settings_t;

// Initialize NVS
esp_err_t nvs_config_init(void);

// Maximum number of saved WiFi networks
#define MAX_SAVED_WIFI_NETWORKS 3

// Save WiFi credentials to NVS (legacy - saves as first network)
esp_err_t nvs_save_wifi_credentials(const char *ssid, const char *password);

// Load WiFi credentials from NVS (legacy - loads first network)
esp_err_t nvs_load_wifi_credentials(wifi_credentials_t *creds);

// Check if WiFi credentials exist in NVS (legacy - checks if any networks saved)
bool nvs_has_wifi_credentials(void);

// Clear WiFi credentials from NVS (legacy - clears all networks)
esp_err_t nvs_clear_wifi_credentials(void);

// Multi-network WiFi functions
esp_err_t nvs_add_wifi_network(const char *ssid, const char *password);  // Add or update network (max 3)
esp_err_t nvs_remove_wifi_network(const char *ssid);  // Remove network by SSID
esp_err_t nvs_get_saved_wifi_networks(wifi_credentials_t *networks, uint8_t *count);  // Get all saved networks
uint8_t nvs_get_saved_wifi_count(void);  // Get number of saved networks
bool nvs_is_wifi_saved(const char *ssid);  // Check if specific SSID is saved

// Last connected WiFi tracking
esp_err_t nvs_save_last_wifi_ssid(const char *ssid);  // Save last successfully connected SSID
esp_err_t nvs_load_last_wifi_ssid(char *ssid, size_t max_len);  // Load last connected SSID (ESP_ERR_NOT_FOUND if none)

// Save time settings to NVS
esp_err_t nvs_save_time_settings(const char *timezone, bool dst_enabled, bool format_24hr);

// Load time settings from NVS
esp_err_t nvs_load_time_settings(time_settings_t *settings);

// Check if time settings exist in NVS
bool nvs_has_time_settings(void);

// Save weather settings to NVS
esp_err_t nvs_save_weather_settings(const char *zipcode, bool temp_celsius, uint8_t update_interval_min);

// Save weather coordinates and location to NVS (after geocoding)
esp_err_t nvs_save_weather_coords(const char *zipcode, float latitude, float longitude, const char *location);

// Load weather settings from NVS
esp_err_t nvs_load_weather_settings(weather_settings_t *settings);

// Check if weather settings exist in NVS
bool nvs_has_weather_settings(void);

// CGM settings structure (OAuth2 tokens for Dexcom API)
#define MAX_ACCESS_TOKEN_LEN 512
#define MAX_REFRESH_TOKEN_LEN 512
#define MAX_CGM_TYPE_LEN 16

typedef struct {
    char cgm_type[MAX_CGM_TYPE_LEN];           // "dexcom", "libre", "none"
    char access_token[MAX_ACCESS_TOKEN_LEN];   // OAuth2 access token
    char refresh_token[MAX_REFRESH_TOKEN_LEN]; // OAuth2 refresh token
    int64_t token_expiry;                      // Unix timestamp when token expires
} cgm_settings_t;

// Save CGM type to NVS
esp_err_t nvs_save_cgm_type(const char *cgm_type);

// Load CGM type from NVS
esp_err_t nvs_load_cgm_type(char *cgm_type, size_t len);

// Save Dexcom OAuth tokens to NVS
esp_err_t nvs_save_dexcom_tokens(const char *access_token, const char *refresh_token, int64_t expiry);

// Load Dexcom OAuth tokens from NVS
esp_err_t nvs_load_dexcom_tokens(cgm_settings_t *settings);

// Check if Dexcom tokens exist in NVS
bool nvs_has_dexcom_tokens(void);

// Clear all CGM settings from NVS
esp_err_t nvs_clear_cgm_settings(void);

// Dexcom Share API credentials (username/password based)
esp_err_t nvs_set_dexcom_credentials(const char *username, const char *password);
esp_err_t nvs_get_dexcom_credentials(char *username, size_t user_len, char *password, size_t pass_len);
bool nvs_has_dexcom_credentials(void);
esp_err_t nvs_clear_dexcom_credentials(void);

// Alarm settings structures
typedef enum {
    ALARM_TONE_BEEP_1 = 0,
    ALARM_TONE_BEEP_2,
    ALARM_TONE_BEEP_3,
    ALARM_TONE_CHIME,
    ALARM_TONE_TWINKLE,
    ALARM_TONE_FUR_ELISE,
    ALARM_TONE_DIXIE_HORN,
    ALARM_TONE_SAINTS,
    ALARM_TONE_SIREN,
    ALARM_TONE_ASCENDING,
    ALARM_TONE_DOORBELL,
    ALARM_TONE_BIG_BEN,
    ALARM_TONE_COUNT
} alarm_tone_t;

typedef struct {
    bool enabled;              // Alarm enabled/disabled
    int threshold;             // Glucose threshold (mg/dL)
    uint32_t text_color;       // RGB hex color for glucose text (0xRRGGBB)
    bool audio_enabled;        // Audio alert enabled
    bool visual_enabled;       // Visual alert enabled
    alarm_tone_t tone;         // Alarm tone selection
    uint8_t volume;            // Volume (0-100)
    bool audio_repeat;         // Repeat audio until dismissed (true) or play once (false)
} alarm_config_t;

typedef struct {
    alarm_config_t high_alarm;     // High alarm (urgent)
    alarm_config_t high_warning;   // High warning (precautionary)
    alarm_config_t low_warning;    // Low warning (precautionary)
    alarm_config_t low_alarm;      // Low alarm (urgent)
} cgm_alarms_t;

// Alarm settings functions
esp_err_t nvs_save_alarm_settings(const cgm_alarms_t *alarms);
esp_err_t nvs_load_alarm_settings(cgm_alarms_t *alarms);
bool nvs_has_alarm_settings(void);
void nvs_get_default_alarm_settings(cgm_alarms_t *alarms);

// Brightness settings
esp_err_t nvs_save_brightness(uint8_t brightness_percent);
esp_err_t nvs_load_brightness(uint8_t *brightness_percent);

#endif // NVS_CONFIG_H
