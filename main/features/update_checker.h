/*
 * update_checker.h
 *
 * Firmware update checker — fetches version.json from website,
 * compares against running firmware, shows overlay if update available.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    bool available;       // True if newer version exists on server
    bool checked;         // True if a check has been completed
    bool check_failed;    // True if last check had a network/parse error
    char version[16];     // Latest version string e.g. "1.5.8"
    char stage[16];       // e.g. "Beta", "Release"
    char date[16];        // e.g. "2026-03-10"
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
