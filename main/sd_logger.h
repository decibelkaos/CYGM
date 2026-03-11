/*
 * SD Card Logger — v2.1 (Mount-on-Write + Serial Capture)
 *
 * Three log streams to SD card:
 *   C*.LOG — Curated system log (explicit sd_log() calls)
 *   G*.CSV — Glucose readings CSV
 *   S*.LOG — Full serial capture (ALL ESP_LOG output, raw)
 *
 * Hardware: JC2432W328 microSD slot on SPI3_HOST (VSPI)
 * Pins: CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
 *
 * Permanent RAM: ~10.5KB (2KB curated + 512B glucose + 8KB serial + mutexes)
 * Temporary RAM: ~11KB during flush (~200ms every 90s)
 */

#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

/**
 * Initialize SD card SPI bus and probe for card presence.
 * If card detected, installs serial capture handler (esp_log_set_vprintf).
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
 * Flush all buffered data to SD card.
 * Mounts FAT → writes curated log + glucose CSV + serial log → unmounts.
 * Takes ~200ms, uses ~11KB temporary RAM.
 *
 * Files written per day:
 *   CYYMMDD.LOG — Curated system log
 *   GYYMMDD.CSV — Glucose readings
 *   SYYMMDD.LOG — Full serial output capture
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

/**
 * Mount SD card for external use (e.g., screenshot capture).
 * Acquires log_mutex and pauses serial capture for the duration.
 * Caller MUST call sd_card_unmount() when done.
 *
 * @param out_card  Receives the mounted card handle
 * @return ESP_OK on success, error otherwise
 */
esp_err_t sd_card_mount(sdmmc_card_t **out_card);

/**
 * Unmount SD card after external use.
 * Releases log_mutex and resumes serial capture.
 */
void sd_card_unmount(sdmmc_card_t *card);

/** Mount point path for SD card file operations */
#define SD_MOUNT_POINT "/sdcard"
