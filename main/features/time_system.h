/*
 * time_system.h
 *
 * Time synchronization and management module
 * SNTP configuration, timezone handling, time display updates
 */

#ifndef FEATURES_TIME_SYSTEM_H
#define FEATURES_TIME_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SNTP time synchronization
 * Configures NTP servers and sets timezone
 */
void initialize_sntp(void);

/**
 * Check if system time is synced (not 1970)
 * @return true if time is synced
 */
bool is_time_synced(void);

/**
 * Wait for SNTP time sync with timeout
 * @param timeout_ms Timeout in milliseconds
 * @return true if synced within timeout
 */
bool wait_for_time_sync(uint32_t timeout_ms);

/**
 * Update time display widgets (clock, date, AM/PM)
 * Safe to call before UI is initialized (will return early)
 */
void update_time_display(void);

/**
 * Time update task - updates display every 10 seconds
 * @param pvParameters Task parameter (unused)
 */
void time_update_task(void *pvParameters);

/**
 * Load time settings from NVS
 * Loads timezone, DST, 12/24hr format
 */
void load_time_settings(void);

/**
 * Get POSIX timezone string from IANA timezone identifier
 * @param iana_tz IANA timezone name (e.g., "America/New_York")
 * @return POSIX timezone string (e.g., "EST5EDT,M3.2.0,M11.1.0")
 */
const char* get_posix_timezone(const char *iana_tz);

#ifdef __cplusplus
}
#endif

#endif // FEATURES_TIME_SYSTEM_H
