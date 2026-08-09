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

// CGM type (for future multi-CGM support: "dexcom", "libre", "nightscout")
#define MAX_CGM_TYPE_LEN 16

// Save/load CGM type to NVS
esp_err_t nvs_save_cgm_type(const char *cgm_type);
esp_err_t nvs_load_cgm_type(char *cgm_type, size_t len);

// Dexcom Share API credentials (username/password based)
esp_err_t nvs_set_dexcom_credentials(const char *username, const char *password);
esp_err_t nvs_get_dexcom_credentials(char *username, size_t user_len, char *password, size_t pass_len);
bool nvs_has_dexcom_credentials(void);
esp_err_t nvs_clear_dexcom_credentials(void);

// LibreLinkUp credentials (email/password)
esp_err_t nvs_set_libre_credentials(const char *email, const char *password);
esp_err_t nvs_get_libre_credentials(char *email, size_t email_len, char *password, size_t pass_len);
bool nvs_has_libre_credentials(void);
esp_err_t nvs_clear_libre_credentials(void);

// LibreLinkUp session (JWT token, patient ID, region URL, token expiry, account ID hash)
esp_err_t nvs_save_libre_session(const char *token, const char *patient_id,
                                  const char *region_url, int32_t token_expires,
                                  const char *account_id);
esp_err_t nvs_load_libre_session(char *token, size_t token_max,
                                  char *patient_id, size_t pid_max,
                                  char *region_url, size_t region_max,
                                  int32_t *token_expires,
                                  char *account_id, size_t acct_max);
esp_err_t nvs_clear_libre_session(void);

// Nightscout credentials (URL + optional API token)
esp_err_t nvs_set_nightscout_credentials(const char *url, const char *token);
esp_err_t nvs_get_nightscout_credentials(char *url, size_t url_len, char *token, size_t token_len);
bool nvs_has_nightscout_credentials(void);
esp_err_t nvs_clear_nightscout_credentials(void);

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
    // Indices below are persisted in NVS (alarm_config_t.tone and the
    // extension blob's gap_tone), so tones may only ever be APPENDED here.
    // Reordering or removing one silently repoints every saved alarm.
    ALARM_TONE_HEARTBEAT,
    ALARM_TONE_MARIMBA,
    ALARM_TONE_HARP,
    ALARM_TONE_SONAR,
    ALARM_TONE_CUCKOO,
    ALARM_TONE_TRILL,
    ALARM_TONE_SOS,
    ALARM_TONE_TRAIN,
    ALARM_TONE_PAGER,
    ALARM_TONE_SWEEP,
    ALARM_TONE_KLAXON,
    ALARM_TONE_RAPID_PULSE,
    ALARM_TONE_RANDOM,        // Generated fresh on every play — never the same twice
    ALARM_TONE_CHURCH_BELL,
    ALARM_TONE_XYLOPHONE,
    ALARM_TONE_ALERT_TRIPLE,
    ALARM_TONE_COUNT
} alarm_tone_t;

typedef struct {
    bool enabled;              // Alarm enabled/disabled
    int threshold;             // Glucose threshold (mg/dL)
    uint32_t text_color;       // RGB hex color for glucose text (0xRRGGBB)
    bool audio_enabled;        // Audio alert enabled
    bool visual_enabled;       // Visual alert enabled
    bool led_enabled;          // LED flash during alarm (default: true)
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

// Idle dim while on the charger (default off: plugged in = display stays on)
esp_err_t nvs_save_dim_while_charging(bool enabled);
bool nvs_load_dim_while_charging(void);

// Scheduled night dim — a clock window during which the backlight drops to a
// second, much lower level. Its own blob, loaded the same way as the alarm
// extension blob (seed defaults, overlay stored bytes, clamp), so appending a
// field can never invalidate the user's schedule.
typedef struct {
    uint8_t enabled;           // 0/1     default 0 (opt-in)
    uint8_t start_hour;        // 0-23    default 21
    uint8_t start_min;         // 0-59    default 30
    uint8_t end_hour;          // 0-23    default 6
    uint8_t end_min;           // 0-59    default 30
    uint8_t night_brightness;  // 1-100%  default 15
} cygm_night_cfg_t;

esp_err_t nvs_save_night_cfg(const cygm_night_cfg_t *cfg);
esp_err_t nvs_load_night_cfg(cygm_night_cfg_t *cfg);

// Legal disclaimer acceptance
esp_err_t nvs_set_disclaimer_accepted(void);
bool nvs_get_disclaimer_accepted(void);

// SD card glucose logging toggle (persisted)
esp_err_t nvs_save_sd_glucose_logging(bool enabled);
bool nvs_get_sd_glucose_logging(void);

// SD serial capture toggle (persisted across reboots)
esp_err_t nvs_save_sd_serial_capture(bool enabled);
bool nvs_get_sd_serial_capture(void);

// Factory reset — erases entire NVS partition
esp_err_t nvs_factory_reset(void);

// Welcome screen shown flag (one-time after first ToS accept)
bool nvs_get_welcome_shown(void);
esp_err_t nvs_set_welcome_shown(void);

// Locale: glucose units (false = mg/dL [default], true = mmol/L)
esp_err_t nvs_save_glucose_mmol(bool mmol);
bool nvs_get_glucose_mmol(void);

// Locale: date format (false = US month-day [default], true = day-month)
esp_err_t nvs_save_date_dmy(bool dmy);
bool nvs_get_date_dmy(void);

// Dexcom region cache (dexcom_region_t int; -1 = unknown / full probe)
esp_err_t nvs_save_dexcom_region(int region);
int nvs_get_dexcom_region(void);

#endif // NVS_CONFIG_H
