/*
 * SD Card Logger
 *
 * Two streams to the microSD slot (SPI3_HOST/VSPI, CS=5 MOSI=23 MISO=19 SCK=18):
 * GYYMMDD.CSV for glucose readings and SYYMMDD.LOG for raw serial capture.
 * The card is mounted once at boot and stays mounted — flush only opens, writes
 * and closes, so there is no mount/unmount heap churn. Costs ~10KB of RAM.
 */

#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

/**
 * Bring up the SPI bus, mount the card and install the serial capture handler.
 * Returns ESP_OK whether or not a card is present.
 */
esp_err_t sd_logger_init(void);

/** Log via ESP_LOGI; serial capture picks it up into S*.LOG. */
void sd_log(const char *tag, const char *fmt, ...);

/**
 * Write the buffered glucose CSV and serial log to the card. Thread-safe
 * (holds the mutex across the file operations); ~512B of heap impact.
 */
esp_err_t sd_logger_flush(void);

/** Append a reading to GYYMMDD.CSV as "YYYY-MM-DD HH:MM:SS,value,trend". */
void sd_log_glucose(int mg_dl, const char *trend);

/** True when the card is mounted and logging is active. */
bool sd_logger_available(void);

/**
 * Resume after suspension: clears the suspend state and failure counter and
 * re-enables serial capture. No-op when no card was ever detected.
 */
void sd_logger_resume(void);

/**
 * Acquire the card for external use (e.g. screenshot capture) and return its
 * handle. Takes the mutex only — the caller MUST pair this with sd_card_unmount().
 */
esp_err_t sd_card_mount(sdmmc_card_t **out_card);

/** Release the mutex after external use. Does NOT unmount. */
void sd_card_unmount(sdmmc_card_t *card);

/** Unmount for a clean shutdown (e.g. before OTA). All SD ops fail afterwards. */
void sd_logger_shutdown(void);

/** Enable/disable serial capture. Only effective while mounted and not suspended. */
void sd_serial_capture_set(bool enabled);

/** True when serial capture to SD is enabled. */
bool sd_serial_capture_get(void);

/** Mount point path for SD card file operations */
#define SD_MOUNT_POINT "/sdcard"
