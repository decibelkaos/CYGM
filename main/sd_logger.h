/*
 * SD Card Logger
 *
 * Two streams to the microSD slot (SPI3_HOST/VSPI, CS=5 MOSI=23 MISO=19 SCK=18):
 * GYYMMDD.CSV for glucose readings and SYYMMDD.LOG for raw serial capture.
 * Only the CSV is written by default; the serial diary starts off at every boot
 * and has to be asked for with "log on" each session.
 * The card is mounted once at boot and stays mounted — flush only opens, writes
 * and closes, so there is no mount/unmount heap churn. Costs ~10KB of RAM.
 */

#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "cgm_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/*
 * Column contract for GYYMMDD.CSV, written as the first line of each day file.
 * The card viewer parses these names, so the order is frozen: the first three
 * columns are what every file ever written begins with and must never move, and
 * anything new is APPENDED. A file that already has rows never gains a header.
 */
#define SD_GLUCOSE_CSV_HEADER \
    "time,mgdl,trend,epoch,provider,status,age_min,batt_pct,batt_v," \
    "charging,rssi,heap_free,uptime_s,alarm\n"

/**
 * Append one reading plus the device's health at that moment to GYYMMDD.CSV,
 * per SD_GLUCOSE_CSV_HEADER. The provider comes from the caller because it is
 * the one field with no global to read; everything else is sampled here.
 *
 * The file is handed to clinicians, so nothing identifying may ever be added:
 * no SSID, address, MAC, device id, coordinates, place name or account detail.
 * See docs/development/LOGGING_POLICY.md.
 */
void sd_log_glucose(int mg_dl, const char *trend, cgm_provider_t provider);

/** True when the card is mounted and logging is active. */
bool sd_logger_available(void);

/**
 * Resume after suspension: clears the suspend state and failure counter and
 * restores whatever serial capture the operator asked for in THIS session.
 * No-op when no card was ever detected.
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

/**
 * Enable/disable serial capture for the running session. Only effective while
 * mounted and not suspended. Capture is always OFF at boot regardless of the
 * stored preference — only CGM data goes to the card unless someone asks for
 * the diary over the console.
 */
void sd_serial_capture_set(bool enabled);

/** True when serial capture to SD is enabled. */
bool sd_serial_capture_get(void);

/*
 * Glucose CSV access for the serial console viewer. Both calls flush the day's
 * buffered lines first and hold the logger mutex across the card access, so the
 * callbacks below run with the card locked and must do nothing but write to the
 * console.
 */
typedef enum {
    SD_GLUCOSE_OK = 0,
    SD_GLUCOSE_NO_CARD,        // No card was mounted at boot
    SD_GLUCOSE_SUSPENDED,      // Card stopped accepting writes; logging is suspended
    SD_GLUCOSE_BAD_NAME,       // Not exactly G + six digits + .CSV
    SD_GLUCOSE_NOT_FOUND,      // No such file on the card
    SD_GLUCOSE_OPEN_FAILED,    // Directory could not be listed
    SD_GLUCOSE_READ_FAILED,    // File opened but reading it failed part-way
} sd_glucose_status_t;

/** Called once, after the listing succeeds and before the first entry. */
typedef void (*sd_glucose_ready_fn)(void *ctx);

/** Called once per file, newest name first. */
typedef void (*sd_glucose_entry_fn)(const char *name, uint32_t size, void *ctx);

/** Called once with the stat size, before any file data. */
typedef void (*sd_glucose_open_fn)(uint32_t size, void *ctx);

/** Called with the file bytes in order, never with len 0. */
typedef void (*sd_glucose_data_fn)(const char *data, size_t len, void *ctx);

/**
 * True when name is exactly G + six digits + .CSV, case-insensitive.
 * out_upper (needs 12 bytes) receives the upper-case form.
 */
bool sd_glucose_name_valid(const char *name, char *out_upper, size_t out_len);

/**
 * List every GYYMMDD.CSV on the card, directory order, not sorted. *out_count receives the
 * number of entries emitted.
 */
sd_glucose_status_t sd_glucose_list(sd_glucose_ready_fn on_ready,
                                    sd_glucose_entry_fn on_entry,
                                    void *ctx, int *out_count);

/**
 * Stream one GYYMMDD.CSV. *out_crc32 receives the CRC-32 (IEEE) of the bytes
 * emitted. A failure after on_open has run leaves the stream truncated.
 */
sd_glucose_status_t sd_glucose_read(const char *name,
                                    sd_glucose_open_fn on_open,
                                    sd_glucose_data_fn on_data,
                                    void *ctx, uint32_t *out_crc32);

/** Mount point path for SD card file operations */
#define SD_MOUNT_POINT "/sdcard"
