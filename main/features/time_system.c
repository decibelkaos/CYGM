/*
 * time_system.c
 *
 * Time synchronization and management implementation
 */

#include "time_system.h"
#include "shared_state.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "TIME_SYSTEM";

// Timezone data - IANA timezone names
static const char *timezone_list[] = {
    // US timezones
    "America/New_York",      // Eastern
    "America/Chicago",       // Central
    "America/Denver",        // Mountain
    "America/Phoenix",       // Arizona (no DST)
    "America/Los_Angeles",   // Pacific
    "America/Anchorage",     // Alaska
    "Pacific/Honolulu",      // Hawaii
    // International
    "Europe/London",         // UK
    "Europe/Paris",          // Central Europe
    "Europe/Berlin",         // Germany
    "Europe/Rome",           // Italy
    "Europe/Madrid",         // Spain
    "Europe/Moscow",         // Russia
    "Asia/Tokyo",            // Japan
    "Asia/Shanghai",         // China
    "Asia/Hong_Kong",        // Hong Kong
    "Asia/Singapore",        // Singapore
    "Asia/Dubai",            // UAE
    "Asia/Kolkata",          // India
    "Australia/Sydney",      // Australia East
    "Australia/Perth",       // Australia West
    "Pacific/Auckland"       // New Zealand
};

// POSIX timezone strings (for actual timezone conversion)
static const char *timezone_posix[] = {
    // US timezones
    "EST5EDT,M3.2.0,M11.1.0",     // Eastern (UTC-5/UTC-4)
    "CST6CDT,M3.2.0,M11.1.0",     // Central (UTC-6/UTC-5)
    "MST7MDT,M3.2.0,M11.1.0",     // Mountain (UTC-7/UTC-6)
    "MST7",                        // Arizona (UTC-7, no DST)
    "PST8PDT,M3.2.0,M11.1.0",     // Pacific (UTC-8/UTC-7)
    "AKST9AKDT,M3.2.0,M11.1.0",   // Alaska (UTC-9/UTC-8)
    "HST10",                       // Hawaii (UTC-10, no DST)
    // International
    "GMT0BST,M3.5.0/1,M10.5.0",   // UK (UTC+0/UTC+1)
    "CET-1CEST,M3.5.0,M10.5.0/3", // Central Europe (UTC+1/UTC+2)
    "CET-1CEST,M3.5.0,M10.5.0/3", // Germany (UTC+1/UTC+2)
    "CET-1CEST,M3.5.0,M10.5.0/3", // Italy (UTC+1/UTC+2)
    "CET-1CEST,M3.5.0,M10.5.0/3", // Spain (UTC+1/UTC+2)
    "MSK-3",                       // Moscow (UTC+3, no DST since 2014)
    "JST-9",                       // Japan (UTC+9, no DST)
    "CST-8",                       // China (UTC+8, no DST)
    "HKT-8",                       // Hong Kong (UTC+8, no DST)
    "SGT-8",                       // Singapore (UTC+8, no DST)
    "GST-4",                       // UAE (UTC+4, no DST)
    "IST-5:30",                    // India (UTC+5:30, no DST)
    "AEST-10AEDT,M10.1.0,M4.1.0/3", // Sydney (UTC+10/UTC+11)
    "AWST-8",                      // Perth (UTC+8, no DST)
    "NZST-12NZDT,M9.5.0,M4.1.0/3" // New Zealand (UTC+12/UTC+13)
};

static const int timezone_count = 22;

// Helper function to get POSIX timezone string from IANA identifier
const char* get_posix_timezone(const char *iana_tz) {
    for (int i = 0; i < timezone_count; i++) {
        if (strcmp(iana_tz, timezone_list[i]) == 0) {
            return timezone_posix[i];
        }
    }
    // Default to PST if not found
    ESP_LOGW(TAG, "Timezone '%s' not found, defaulting to PST", iana_tz);
    return "PST8PDT,M3.2.0,M11.1.0";
}

// SNTP time sync notification callback
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "=== TIME SYNCHRONIZED FROM SNTP SERVER ===");
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "New system time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sd_log(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

bool is_time_synced(void) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // If year is still 1970, time is not synced
    if (timeinfo.tm_year < (2020 - 1900)) {
        return false;
    }
    return true;
}

bool wait_for_time_sync(uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Waiting for SNTP time sync...");
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int check_count = 0;

    while (!is_time_synced()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;
        check_count++;

        // Log every 5 seconds
        if (check_count % 10 == 0) {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "Still waiting for time sync... current: %04d-%02d-%02d %02d:%02d:%02d (elapsed: %lu ms)",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, elapsed);
        }

        if (elapsed >= timeout_ms) {
            ESP_LOGW(TAG, "Time sync timeout after %lu ms", elapsed);
            return false;
        }
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
}

void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");

    // Check if already initialized
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already initialized, restarting...");
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.nist.gov");  // Add third server for redundancy
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // Set timezone using POSIX format
    const char *posix_tz = get_posix_timezone(user_timezone);
    setenv("TZ", posix_tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set: %s -> %s", user_timezone, posix_tz);

    ESP_LOGI(TAG, "SNTP initialized with 3 servers. Waiting for system time to be set...");

    // Show current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Current system time BEFORE sync: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void update_time_display(void) {
    if (label_clock == NULL || label_date == NULL) {
        ESP_LOGW(TAG, "Clock labels not initialized yet");
        return;  // Not initialized yet
    }

    time_t now;
    struct tm timeinfo;
    time(&now);

    localtime_r(&now, &timeinfo);

    char strftime_buf[64];

    // Update clock — toggle colon every 10s as heartbeat (no recolor, no animations)
    static bool colon_visible = true;
    colon_visible = !colon_visible;
    if (user_24hr_format) {
        strftime(strftime_buf, sizeof(strftime_buf), colon_visible ? "%H:%M" : "%H %M", &timeinfo);
    } else {
        strftime(strftime_buf, sizeof(strftime_buf), colon_visible ? "%I:%M" : "%I %M", &timeinfo);
    }

    if (lvgl_port_lock(0)) {
        lv_label_set_text(label_clock, strftime_buf);

        // Update expanded CGM view time label
        if (expanded_time_label != NULL) {
            lv_label_set_text(expanded_time_label, strftime_buf);
        }

        // Update AM/PM label (12hr mode only)
        if (!user_24hr_format && label_ampm != NULL) {
            strftime(strftime_buf, sizeof(strftime_buf), "%p", &timeinfo);
            lv_label_set_text(label_ampm, strftime_buf);
            lv_obj_clear_flag(label_ampm, LV_OBJ_FLAG_HIDDEN);  // Show AM/PM
        } else if (label_ampm != NULL) {
            lv_obj_add_flag(label_ampm, LV_OBJ_FLAG_HIDDEN);  // Hide AM/PM in 24hr mode
        }

        // Update date
        strftime(strftime_buf, sizeof(strftime_buf), "%a, %b %d", &timeinfo);
        lv_label_set_text(label_date, strftime_buf);

        lvgl_port_unlock();
    }
}

void time_update_task(void *pvParameters) {
    while (1) {
        update_time_display();
        vTaskDelay(pdMS_TO_TICKS(10000));  // Update every 10 seconds
    }
}

void load_time_settings(void) {
    time_settings_t settings;

    // Try to load settings from NVS
    if (nvs_load_time_settings(&settings) == ESP_OK) {
        // Apply loaded settings
        strncpy(user_timezone, settings.timezone, sizeof(user_timezone) - 1);
        user_timezone[sizeof(user_timezone) - 1] = '\0';
        user_dst_enabled = settings.dst_enabled;
        user_24hr_format = settings.format_24hr;

        ESP_LOGI(TAG, "Loaded time settings from NVS:");
        ESP_LOGI(TAG, "  Timezone: %s", user_timezone);
        ESP_LOGI(TAG, "  DST: %s", user_dst_enabled ? "Enabled" : "Disabled");
        ESP_LOGI(TAG, "  Format: %s", user_24hr_format ? "24hr" : "12hr");
    } else {
        ESP_LOGI(TAG, "No saved time settings, using defaults (TZ: %s)", user_timezone);
    }
}
