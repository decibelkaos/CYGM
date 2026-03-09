/*
 * SD Card Logger — v2 (Mount-on-Write)
 *
 * Logs CGM fetch events, memory status, and session data to SD card.
 * Uses mount-on-write pattern: ~2.5KB permanent RAM, ~11KB temporary during flush.
 *
 * Hardware: JC2432W328 microSD slot on SPI3_HOST (VSPI)
 * Pins: CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize SD card SPI bus and probe for card presence.
 * Safe to call even without SD card inserted — returns ESP_OK regardless.
 * Call once during boot after WiFi connects (so RTC is synced for timestamps).
 */
esp_err_t sd_logger_init(void);

/**
 * Buffer a timestamped log line. No SD I/O occurs — just appends to
 * a 2KB static buffer. Thread-safe. Silently drops if buffer full or no card.
 *
 * Format: [YYYY-MM-DD HH:MM:SS] TAG: message\n
 */
void sd_log(const char *tag, const char *fmt, ...);

/**
 * Flush buffered log lines to SD card.
 * Mounts FAT → opens daily log file (append) → writes → closes → unmounts.
 * Takes ~200ms, uses ~11KB temporary RAM.
 *
 * Call AFTER dexcom_close_persistent_client() to ensure headroom.
 * Thread-safe — holds mutex during entire mount-write-unmount cycle.
 */
esp_err_t sd_logger_flush(void);

/**
 * Log a glucose reading to the simple CSV log.
 * Format: YYYY-MM-DD HH:MM:SS,value,trend
 * Written to GYYMMDD.CSV (separate from system log for easy reading).
 */
void sd_log_glucose(int mg_dl, const char *trend);

/**
 * Returns true if SD card was detected during init.
 */
bool sd_logger_available(void);
