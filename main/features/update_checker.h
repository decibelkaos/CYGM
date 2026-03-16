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
 * Free heap memory for update operations.
 * Pauses and deletes background tasks (weather/time/battery), closes Dexcom
 * SSL to maximize contiguous heap for TLS handshake.
 * Call update_restore_tasks() afterward to recreate tasks.
 */
void update_prepare_memory(void);

/**
 * Recreate background tasks and unpause after update_prepare_memory().
 */
void update_restore_tasks(void);

/**
 * Aggressively free memory for OTA download.
 * DELETES weather, glucose, time, and battery tasks to reclaim ~19KB of stack.
 * Disables WiFi power save for reliable download.
 * After this call, the device MUST reboot to restore normal operation.
 * Call this right before update_do_ota_download().
 */
void update_stop_all_tasks(void);

/** Show OTA progress overlay. Must be called with LVGL lock held. */
void show_ota_overlay(const char *status);

/** Tell update system whether a manual check task is actively polling.
 *  When true, update_start_ota() signals the check task directly instead
 *  of creating a fallback task (whose 4KB stack would fragment the heap). */
void update_set_manual_polling(bool polling);

/**
 * Signal that user wants to install the OTA update.
 * If the manual check task is polling, it picks up the request directly.
 * Otherwise, creates a fallback task to handle OTA autonomously.
 */
void update_start_ota(void);

/** Returns true if Install button was pressed (polled by check task) */
bool update_ota_install_requested(void);

/** Clear the OTA install request flag */
void update_clear_ota_request(void);

/**
 * Execute OTA firmware download and flash. Runs in the CALLER's task context.
 * Must be called with update_prepare_memory() already done.
 * On success: reboots. On failure: shows error overlay.
 */
void update_do_ota_download(void);

/** Returns true if OTA download is currently in progress */
bool update_ota_in_progress(void);

/**
 * Call from app_main() after successful init to confirm the running firmware.
 * Prevents automatic rollback to previous version.
 */
void update_confirm_boot(void);
