/*
 * time_system.h
 *
 * SNTP sync, timezone handling, clock display and the night-dim schedule.
 */

#ifndef FEATURES_TIME_SYSTEM_H
#define FEATURES_TIME_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the NTP servers and timezone, and start SNTP. */
void initialize_sntp(void);

/** True once the system clock has been synced (i.e. is no longer at 1970). */
bool is_time_synced(void);

/** Block until SNTP sync completes or timeout_ms elapses. */
bool wait_for_time_sync(uint32_t timeout_ms);

/** Refresh the clock, date and AM/PM widgets. Returns early if the UI is not up yet. */
void update_time_display(void);

/**
 * Render a wall-clock time in the user's chosen 12/24-hour format: "22:00", or
 * "9:30a" / "6:30p" with no leading zero on the hour. Every settings surface
 * that prints a scheduled time uses this, so one preference drives all of them.
 * buf needs 7 bytes for the widest output; hour and minute are wrapped in range.
 */
void cygm_format_clock(char *buf, size_t len, uint8_t hour, uint8_t minute);

/** Time task: refreshes the display every 10 seconds. */
void time_update_task(void *pvParameters);

/** Load timezone, DST and 12/24-hour format from NVS. */
void load_time_settings(void);

/** Map an IANA timezone ("America/New_York") to POSIX TZ ("EST5EDT,M3.2.0,M11.1.0"). */
const char* get_posix_timezone(const char *iana_tz);

/**
 * Live night-dim schedule. Loaded by load_time_settings(); edit in place from
 * the settings UI and persist with nvs_save_night_cfg() — the engine reads this
 * global on every tick, so no reload is needed.
 */
extern cygm_night_cfg_t night_cfg;

/**
 * True while the clock sits inside the configured night window. Always false
 * when the schedule is disabled or the clock is unsynced — a device that booted
 * to 1970 must never decide it is night.
 */
bool cygm_night_active(void);

/**
 * Brightness (1-100) the schedule wants right now: the night level inside the
 * window, the daytime level outside it. Use this as the restore level after any
 * temporary dim.
 */
uint8_t cygm_current_scheduled_brightness(void);

#ifdef __cplusplus
}
#endif

#endif // FEATURES_TIME_SYSTEM_H
