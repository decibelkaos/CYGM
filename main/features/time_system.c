/*
 * time_system.c
 *
 * Time synchronization and management implementation
 */

#include "time_system.h"
#include "shared_state.h"
#include "nvs_config.h"
#include "hardware/display.h"
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

// IANA name -> POSIX TZ string mapping.
//
// ESP-IDF's newlib has NO zoneinfo database, so setenv("TZ","Europe/Paris") does
// nothing useful — it needs a POSIX TZ rule string instead, and the geocoding API
// hands us IANA names. The table lives in flash (.rodata), so it costs no RAM.
// There are ~400 IANA zones but only a few dozen distinct POSIX rules, so this
// covers essentially every populated zone. Lookup is linear; it runs once per
// setup change, so entries are grouped by region for readability.
typedef struct {
    const char *iana;
    const char *posix;
} tz_map_t;

static const tz_map_t timezone_map[] = {
    // ---- North America ----
    {"America/New_York",      "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Detroit",       "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto",       "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",       "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Winnipeg",      "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",        "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Edmonton",      "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",       "MST7"},
    {"America/Los_Angeles",   "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver",     "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",     "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu",      "HST10"},
    {"America/Halifax",       "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns",      "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City",   "CST6"},
    {"America/Tijuana",       "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Chihuahua",     "CST6"},  // Mexico abolished DST in 2022
    {"America/Monterrey",     "CST6"},
    {"America/Indiana/Indianapolis", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Regina",        "CST6"},  // Saskatchewan, no DST
    // ---- Central/South America ----
    {"America/Bogota",        "COT5"},
    {"America/Lima",          "PET5"},
    {"America/Sao_Paulo",     "BRT3"},
    {"America/Argentina/Buenos_Aires", "ART3"},
    {"America/Santiago",      "CLT4CLST,M9.1.6/24,M4.1.6/24"},
    {"America/Caracas",       "VET4"},
    // ---- Europe ----
    {"Europe/London",         "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",         "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon",         "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome",           "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo",           "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen",     "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens",         "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest",      "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kiev",           "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul",       "TRT-3"},
    {"Europe/Moscow",         "MSK-3"},
    // ---- Middle East / Africa ----
    {"Asia/Jerusalem",        "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Dubai",            "GST-4"},
    {"Asia/Riyadh",           "AST-3"},
    {"Asia/Tehran",           "IRST-3:30"},
    {"Africa/Cairo",          "EET-2EEST,M4.5.5/0,M10.5.4/24"},  // Egypt reinstated DST in 2023
    {"Africa/Johannesburg",   "SAST-2"},
    {"Africa/Lagos",          "WAT-1"},
    {"Africa/Nairobi",        "EAT-3"},
    {"Africa/Casablanca",     "WET0WEST,M3.5.0,M10.5.0/3"},
    // ---- Asia ----
    {"Asia/Karachi",          "PKT-5"},
    {"Asia/Kolkata",          "IST-5:30"},
    {"Asia/Colombo",          "IST-5:30"},
    {"Asia/Kathmandu",        "NPT-5:45"},
    {"Asia/Dhaka",            "BST-6"},
    {"Asia/Bangkok",          "ICT-7"},
    {"Asia/Jakarta",          "WIB-7"},
    {"Asia/Shanghai",         "CST-8"},
    {"Asia/Hong_Kong",        "HKT-8"},
    {"Asia/Singapore",        "SGT-8"},
    {"Asia/Taipei",           "CST-8"},
    {"Asia/Manila",           "PST-8"},
    {"Asia/Kuala_Lumpur",     "MYT-8"},
    {"Asia/Seoul",            "KST-9"},
    {"Asia/Tokyo",            "JST-9"},
    {"Asia/Ho_Chi_Minh",      "ICT-7"},
    {"Asia/Yangon",           "MMT-6:30"},
    {"Asia/Baku",             "AZT-4"},
    {"Asia/Tashkent",         "UZT-5"},
    {"Asia/Almaty",           "ALMT-6"},
    {"Asia/Yekaterinburg",    "YEKT-5"},
    {"Asia/Omsk",             "OMST-6"},
    {"Asia/Novosibirsk",      "NOVT-7"},
    {"Asia/Krasnoyarsk",      "KRAT-7"},
    {"Asia/Irkutsk",          "IRKT-8"},
    {"Asia/Vladivostok",      "VLAT-10"},
    {"Asia/Magadan",          "MAGT-11"},
    {"Asia/Kamchatka",        "PETT-12"},
    // ---- Oceania ----
    {"Australia/Perth",       "AWST-8"},
    {"Australia/Adelaide",    "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Darwin",      "ACST-9:30"},
    {"Australia/Brisbane",    "AEST-10"},
    {"Australia/Sydney",      "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne",   "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland",      "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Fiji",          "FJT-12"},
    {"Pacific/Guam",          "ChST-10"},
    {"Pacific/Port_Moresby",  "PGT-10"},
    {"Pacific/Guadalcanal",   "SBT-11"},
};

static const int timezone_count = sizeof(timezone_map) / sizeof(timezone_map[0]);

// POSIX timezone string for an IANA identifier. The fallback is UTC, NOT US
// Pacific: a silent PST fallback let setup succeed with correct weather but a
// clock hours wrong for any unlisted zone. UTC makes a mapping gap obvious
// instead of shipping a plausible-but-wrong local time.
const char* get_posix_timezone(const char *iana_tz) {
    if (iana_tz == NULL || iana_tz[0] == '\0') {
        ESP_LOGW(TAG, "Empty timezone, defaulting to UTC");
        return "GMT0";
    }
    for (int i = 0; i < timezone_count; i++) {
        if (strcmp(iana_tz, timezone_map[i].iana) == 0) {
            return timezone_map[i].posix;
        }
    }
    ESP_LOGW(TAG, "Timezone '%s' not in map, defaulting to UTC", iana_tz);
    return "GMT0";
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

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Current system time BEFORE sync: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void cygm_format_clock(char *buf, size_t len, uint8_t hour, uint8_t minute) {
    if (buf == NULL || len == 0) return;

    int h = hour % 24;
    int m = minute % 60;

    if (user_24hr_format) {
        snprintf(buf, len, "%02d:%02d", h, m);
    } else {
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, len, "%d:%02d%s", h12, m, (h < 12) ? "a" : "p");
    }
}

void update_time_display(void) {
    if (label_clock_h == NULL || label_date == NULL) {
        ESP_LOGW(TAG, "Clock labels not initialized yet");
        return;  // Not initialized yet
    }

    time_t now;
    struct tm timeinfo;
    time(&now);

    localtime_r(&now, &timeinfo);

    char strftime_buf[64];

    // Update clock (split into hour + minute labels; colon blinks via heartbeat timer)
    char hbuf[4], mbuf[4];
    if (user_24hr_format) {
        snprintf(hbuf, sizeof(hbuf), "%02d", timeinfo.tm_hour);
    } else {
        int h12 = timeinfo.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(hbuf, sizeof(hbuf), "%02d", h12);
    }
    snprintf(mbuf, sizeof(mbuf), "%02d", timeinfo.tm_min);

    if (lvgl_port_lock(1)) {
        lv_label_set_text(label_clock_h, hbuf);
        lv_label_set_text(label_clock_m, mbuf);

        if (expanded_time_h != NULL) lv_label_set_text(expanded_time_h, hbuf);
        if (expanded_time_m != NULL) lv_label_set_text(expanded_time_m, mbuf);

        // Update AM/PM label (12hr mode only)
        if (!user_24hr_format && label_ampm != NULL) {
            strftime(strftime_buf, sizeof(strftime_buf), "%p", &timeinfo);
            lv_label_set_text(label_ampm, strftime_buf);
            lv_obj_clear_flag(label_ampm, LV_OBJ_FLAG_HIDDEN);  // Show AM/PM
        } else if (label_ampm != NULL) {
            lv_obj_add_flag(label_ampm, LV_OBJ_FLAG_HIDDEN);  // Hide AM/PM in 24hr mode
        }

        // Update date — US month-day ("Mon, Jun 16") vs rest-of-world day-month
        // ("Mon, 16 Jun"), selected by user_date_dmy. %d zero-pads the day.
        if (user_date_dmy) {
            strftime(strftime_buf, sizeof(strftime_buf), "%a, %d %b", &timeinfo);
        } else {
            strftime(strftime_buf, sizeof(strftime_buf), "%a, %b %d", &timeinfo);
        }
        lv_label_set_text(label_date, strftime_buf);

        lvgl_port_unlock();
    }
}

// ==================== Scheduled night dim ====================
//
// The schedule owns the backlight only at the two window edges — it fades once on
// entry and once on exit — so a manual brightness change inside the window stands
// until the next transition.
//
// Two other owners outrank it: the visual alarm, which forces 100% and restores the
// level itself, and the inactivity dimmer, whose wake already applies
// cygm_current_scheduled_brightness(). A transition that lands while the user is
// away is therefore handed to the dimmer rather than fought over — fading against
// it would flash a dark screen bright.
//
// The fade is a short series of display_set_brightness() calls from this task: no
// LVGL animation, no extra task or timer, and display.c is left alone.

cygm_night_cfg_t night_cfg = {
    .enabled = 0, .start_hour = 21, .start_min = 30,
    .end_hour = 6, .end_min = 30, .night_brightness = 15,
};

static uint8_t s_day_brightness = 80;    // user's daytime level (NVS brightness key)
static bool    s_night_applied  = false; // engine is currently holding the night level
static bool    s_alarm_held     = false; // an alarm had the backlight on the last tick
static int     s_engine_level   = -1;    // level this engine last drove, -1 = unknown

#define NIGHT_FADE_STEPS       10
#define NIGHT_FADE_STEP_MS    150   // 10 steps x 150ms = 1.5s, inside the 2s budget
#define NIGHT_PRESENT_IDLE_MS 30000 // below the inactivity dim timeout by a wide margin

bool cygm_night_active(void) {
    if (!night_cfg.enabled) return false;
    if (!is_time_synced()) return false;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int cur   = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int start = night_cfg.start_hour * 60 + night_cfg.start_min;
    int end   = night_cfg.end_hour   * 60 + night_cfg.end_min;

    if (start == end) return false;                 // zero-length window
    if (start < end)  return (cur >= start && cur < end);
    return (cur >= start || cur < end);             // window wraps midnight
}

uint8_t cygm_current_scheduled_brightness(void) {
    if (cygm_night_active()) {
        return night_cfg.night_brightness;
    }
    return (s_day_brightness < 1) ? 1 : s_day_brightness;
}

// Drive the backlight to target, stepping only when the level it is coming from is
// known (this engine put it there); otherwise apply it in one call, since stepping
// from a wrong "from" visibly overshoots. Always ends with an explicit call on the
// target so the hardware lands there even when no stepping happened.
static void night_apply(uint8_t target) {
    int to = target;
    if (to < 1) to = 1;
    if (to > 100) to = 100;

    int from = (s_engine_level >= 1) ? s_engine_level : to;

    // Checked BEFORE every write, not after: an alarm forces 100% and nothing
    // re-asserts it for the rest of the alert, so a single late fade step would
    // leave the takeover dim until it is dismissed.
    for (int step = 1; step < NIGHT_FADE_STEPS && from != to; step++) {
        if (visual_alarm_active) {
            s_engine_level = -1;  // the alarm owns the backlight now
            return;
        }
        int level = from + ((to - from) * step) / NIGHT_FADE_STEPS;
        display_set_brightness((uint8_t)level);
        vTaskDelay(pdMS_TO_TICKS(NIGHT_FADE_STEP_MS));
    }

    if (visual_alarm_active) {
        s_engine_level = -1;
        return;
    }

    display_set_brightness((uint8_t)to);
    s_engine_level = to;
    screen_brightness_percent = (uint8_t)to;
}

static void night_dim_tick(void) {
    if (visual_alarm_active) {
        s_alarm_held = true;
        return;
    }
    bool alarm_just_ended = s_alarm_held;
    s_alarm_held = false;

    bool want_night = cygm_night_active();

    if (!want_night && !s_night_applied) {
        // Daytime with nothing owed: the brightness control owns the level, so
        // track it — that is what the night window will fade back to.
        if (screen_brightness_percent >= 1) {
            s_day_brightness = screen_brightness_percent;
            s_engine_level = screen_brightness_percent;
        }
        return;
    }

    if (want_night == s_night_applied && !alarm_just_ended) {
        return;
    }

    // User away: the inactivity dimmer holds the backlight and its wake restores
    // cygm_current_scheduled_brightness(), which is this transition's target.
    if (lv_disp_get_inactive_time(NULL) >= NIGHT_PRESENT_IDLE_MS) {
        s_night_applied = want_night;
        s_engine_level = -1;
        ESP_LOGI(TAG, "Night dim: %s window entered while idle — wake will apply it",
                 want_night ? "night" : "day");
        return;
    }

    uint8_t target = want_night ? night_cfg.night_brightness : s_day_brightness;
    ESP_LOGI(TAG, "Night dim: %s -> %d%%", want_night ? "night" : "day", target);
    night_apply(target);
    s_night_applied = want_night;
}

void time_update_task(void *pvParameters) {
    while (1) {
        update_time_display();
        night_dim_tick();
        vTaskDelay(pdMS_TO_TICKS(10000));  // Update every 10 seconds
    }
}

void load_time_settings(void) {
    time_settings_t settings;

    if (nvs_load_time_settings(&settings) == ESP_OK) {
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

    // Locale display prefs (standalone NVS keys; default to US conventions for
    // backward compatibility with existing installs).
    user_glucose_mmol = nvs_get_glucose_mmol();
    user_date_dmy = nvs_get_date_dmy();
    ESP_LOGI(TAG, "  Glucose units: %s", user_glucose_mmol ? "mmol/L" : "mg/dL");
    ESP_LOGI(TAG, "  Date format: %s", user_date_dmy ? "D/M" : "M/D");

    // Night-dim schedule plus the daytime level it fades back to. Loaded here,
    // before the display comes up, so the time task's first tick already knows
    // which side of the window it is on.
    nvs_load_night_cfg(&night_cfg);
    uint8_t day_level = 80;
    nvs_load_brightness(&day_level);
    s_day_brightness = (day_level < 1) ? 1 : day_level;
    ESP_LOGI(TAG, "  Night dim: %s %02d:%02d-%02d:%02d @ %d%% (day %d%%)",
             night_cfg.enabled ? "enabled" : "disabled",
             night_cfg.start_hour, night_cfg.start_min,
             night_cfg.end_hour, night_cfg.end_min,
             night_cfg.night_brightness, s_day_brightness);
}
