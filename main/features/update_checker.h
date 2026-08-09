/*
 * update_checker.h
 *
 * Firmware update checker and OTA updater.
 * Checks version.json for newer firmware, downloads and flashes via OTA.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    bool available;       // True if newer version exists on server
    bool checked;         // True if a check has been completed
    bool check_failed;    // True if last check had a network/parse error
    char version[16];     // Latest version string e.g. "1.6.1"
    char stage[16];       // e.g. "Beta", "Release"
    char date[16];        // e.g. "2026-03-10"
    char firmware_url[128]; // OTA firmware download URL (empty = no OTA)
} update_info_t;

/**
 * Check for firmware updates (blocking HTTPS call).
 * Does NOT acquire network_mutex — caller must hold it or ensure no conflicts.
 */
esp_err_t update_check_now(void);

/** Get result of last check */
const update_info_t *update_get_info(void);

/** Returns true if >24h since last check or never checked */
bool update_should_check(void);

/** Show update result overlay. Must be called from LVGL context (or with lock held). */
void show_update_overlay(void);

/** Dismiss update overlay if visible */
void dismiss_update_overlay(void);

/**
 * Delete the weather/time/battery tasks and close the CGM SSL client to give the
 * TLS handshake enough contiguous heap. Pair with update_restore_tasks().
 */
void update_prepare_memory(void);

/** Recreate the background tasks and unpause after update_prepare_memory(). */
void update_restore_tasks(void);

/**
 * DELETES the weather, glucose, time and battery tasks (~19KB of stack) and
 * disables WiFi power save. The device MUST reboot afterwards to restore normal
 * operation. Call immediately before update_do_ota_download().
 */
void update_stop_all_tasks(void);

/** Show OTA progress overlay. Must be called with LVGL lock held. */
void show_ota_overlay(const char *status);

/** Declare whether a manual check task is polling. When true, update_start_ota()
 *  signals that task directly instead of creating a fallback task, whose 4KB
 *  stack would fragment the heap. */
void update_set_manual_polling(bool polling);

/**
 * Request an OTA install. A polling check task picks the request up directly;
 * otherwise a fallback task is created to run the OTA autonomously.
 */
void update_start_ota(void);

/** Returns true if Install button was pressed (polled by check task) */
bool update_ota_install_requested(void);

/** Clear the OTA install request flag */
void update_clear_ota_request(void);

/**
 * Download and flash the firmware in the CALLER's task context; reboots on
 * success, shows an error overlay on failure. Requires update_prepare_memory().
 */
void update_do_ota_download(void);

/** Returns true if OTA download is currently in progress */
bool update_ota_in_progress(void);

/** Confirm the running firmware from app_main() after init, blocking the
 *  automatic rollback to the previous slot. */
void update_confirm_boot(void);
