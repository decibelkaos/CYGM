/*
 * SD Card Logger — v3.0 (Persistent Mount + Serial Capture)
 *
 * Three log streams to SD card:
 *   C*.LOG — Curated system log (explicit sd_log() calls)
 *   G*.CSV — Glucose readings CSV
 *   S*.LOG — Full serial capture (ALL ESP_LOG output, raw)
 *
 * Hardware: JC2432W328 microSD slot on SPI3_HOST (VSPI)
 * Pins: CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
 *
 * SD card is mounted once at boot and stays mounted permanently.
 * Flush just opens/writes/closes files — no mount/unmount heap churn.
 * Permanent RAM: ~12KB (2KB curated + 512B glucose + 8KB serial + ~1.5KB mount)
 */

#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

/**
 * Initialize SD card SPI bus and mount card permanently.
 * If card detected, installs serial capture handler (esp_log_set_vprintf).
 * Safe to call even without SD card inserted — returns ESP_OK regardless.
 * Card stays mounted for the lifetime of the device (no mount/unmount churn).
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
 * Opens files, writes curated log + glucose CSV + serial log, closes files.
 * No mount/unmount — card is permanently mounted. Minimal heap impact (~512B).
 *
 * Files written per day:
 *   CYYMMDD.LOG — Curated system log
 *   GYYMMDD.CSV — Glucose readings
 *   SYYMMDD.LOG — Full serial output capture
 *
 * Thread-safe — holds mutex during file operations.
 */
esp_err_t sd_logger_flush(void);

/**
 * Log a glucose reading to the simple CSV log.
 * Format: YYYY-MM-DD HH:MM:SS,value,trend
 * Written to GYYMMDD.CSV (separate from system log for easy reading).
 */
void sd_log_glucose(int mg_dl, const char *trend);

/**
 * Returns true if SD card is mounted and logging is active.
 */
bool sd_logger_available(void);

/**
 * Resume SD logging after suspension.
 * Resets suspension state, failure counter, and re-enables serial capture.
 * No-op if SD was never initialized or no card detected.
 */
void sd_logger_resume(void);

/**
 * Acquire SD card for external use (e.g., screenshot capture).
 * Card is already mounted — just acquires the mutex and returns the handle.
 * Caller MUST call sd_card_unmount() when done.
 *
 * @param out_card  Receives the mounted card handle
 * @return ESP_OK on success, error otherwise
 */
esp_err_t sd_card_mount(sdmmc_card_t **out_card);

/**
 * Release SD card after external use.
 * Releases mutex — does NOT unmount (card stays mounted permanently).
 */
void sd_card_unmount(sdmmc_card_t *card);

/**
 * Unmount SD card for clean shutdown (e.g., before OTA).
 * After this call, all SD operations will fail.
 */
void sd_logger_shutdown(void);

/**
 * Enable or disable serial capture to SD card.
 * Only effective when SD card is mounted and not suspended.
 */
void sd_serial_capture_set(bool enabled);

/**
 * Returns true if serial capture to SD is currently enabled.
 */
bool sd_serial_capture_get(void);

/** Mount point path for SD card file operations */
#define SD_MOUNT_POINT "/sdcard"
