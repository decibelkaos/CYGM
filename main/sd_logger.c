/*
 * SD Card Logger
 *
 * The card is mounted ONCE at boot and stays mounted; flush only opens, writes and
 * closes. Repeated mount/unmount churns the heap badly enough to break SSL
 * reconnection on this chip.
 *
 * Two streams: GYYMMDD.CSV for glucose readings and SYYMMDD.LOG for raw serial
 * capture, which sd_log() feeds via ESP_LOGI. Permanent cost is ~10KB.
 *
 * Only the CSV is written by default. The serial diary is a debugging aid, so it
 * starts OFF at every boot whatever NVS holds and has to be asked for with
 * "log on" each session.
 */

#include "sd_logger.h"
#include "nvs_config.h"
#include "shared_state.h"
#include "battery.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_wifi.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

static const char *TAG = "SD_LOG";

// Hardware pins (JC2432W328 — VSPI bus, separate from display on HSPI)
#define SD_PIN_CS    5
#define SD_PIN_MOSI  23
#define SD_PIN_MISO  19
#define SD_PIN_SCK   18
#define SD_SPI_HOST  SPI3_HOST
#define MOUNT_POINT  "/sdcard"

static SemaphoreHandle_t log_mutex = NULL;

// Glucose CSV buffer. A row runs ~82 bytes; 201 is the longest the format can
// produce with every integer field at its type limit, so the reserve below is
// above that and a row can never be truncated into a half line. Readings arrive
// every 90s and the battery task flushes at least once a minute, so a buffer
// holding ten typical rows is never close to full.
#define SD_GLUCOSE_LINE_MAX     224
#define SD_GLUCOSE_BUFFER_SIZE  1024
static char glucose_buffer[SD_GLUCOSE_BUFFER_SIZE];
static int glucose_buffer_pos = 0;

// Serial capture buffer — ALL ESP_LOG output
#define SD_SERIAL_BUFFER_SIZE  8192
static char serial_buffer[SD_SERIAL_BUFFER_SIZE];
static int serial_buffer_pos = 0;
static SemaphoreHandle_t serial_mutex = NULL;
static bool serial_capture_enabled = false;
// What the operator asked for over the console this session. Boot leaves it
// false whatever NVS holds, and it is what a resume restores — a suspension
// must not turn the diary on, and must not turn off one that was asked for.
static bool serial_capture_requested = false;
static volatile bool serial_capture_paused = false;

// SPI/SD state
static bool sd_suspended = false;           // True when SD logging is suspended due to persistent failures
static int  sd_flush_fail_count = 0;        // Consecutive flush failures
#define SD_SUSPEND_AFTER_FAILURES  5        // Suspend after this many consecutive failures
static bool sd_write_error_logged = false;  // Write-error detail is logged once per failure run
static int64_t sd_resume_probe_us = 0;      // esp_timer stamp of the last resume probe
#define SD_RESUME_PROBE_INTERVAL_US  (3600LL * 1000000LL)  // At most one resume probe per hour
static sdmmc_host_t host;
static sdspi_device_config_t slot_config;
static sdmmc_card_t *mounted_card = NULL;   // Persistent mount — stays mounted for device lifetime

// Custom vprintf handler — captures ALL ESP_LOG output to serial buffer
static int serial_capture_vprintf(const char *fmt, va_list args)
{
    // Copy args before consuming — va_list can only be used once
    va_list args_copy;
    va_copy(args_copy, args);

    int ret = vprintf(fmt, args);

    // Buffer to SD if enabled, not paused, and not in ISR context
    if (serial_capture_enabled && !serial_capture_paused && serial_mutex) {
        // Non-blocking mutex take — never delay the logging caller
        if (xSemaphoreTake(serial_mutex, 0) == pdTRUE) {
            int remaining = SD_SERIAL_BUFFER_SIZE - serial_buffer_pos;
            if (remaining > 2) {
                int written = vsnprintf(serial_buffer + serial_buffer_pos, remaining, fmt, args_copy);
                if (written > 0) {
                    if (written >= remaining) {
                        // Message truncated — force newline at end to prevent
                        // garbled concatenation with the next log message.
                        written = remaining - 1;
                        serial_buffer[serial_buffer_pos + written - 1] = '\n';
                    }
                    serial_buffer_pos += written;
                }
            }
            xSemaphoreGive(serial_mutex);
        }
    }

    va_end(args_copy);
    return ret;
}

esp_err_t sd_logger_init(void)
{
    log_mutex = xSemaphoreCreateMutex();
    if (!log_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI3 unavailable — SD logging disabled");
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
        return ESP_OK;
    }

    host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    slot_config = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = SD_SPI_HOST;

    // Mount SD card — stays mounted for the lifetime of the device.
    // Mounting at boot (before WiFi/SSL) ensures mount allocations don't
    // interleave with SSL buffers, preventing heap fragmentation.
    size_t heap_before_mount = esp_get_free_heap_size();

    // Temporarily suppress noisy SDMMC/SDSPI errors during probe
    esp_log_level_set("sdspi_transaction", ESP_LOG_NONE);
    esp_log_level_set("sdspi_host", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);

    sdmmc_card_t *card = NULL;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = 0,
    };

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_cfg, &card);

    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);
    esp_log_level_set("sdspi_host", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_init", ESP_LOG_WARN);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);

    if (ret == ESP_OK) {
        mounted_card = card;  // Keep mounted permanently — no unmount!
        size_t mount_cost = heap_before_mount - esp_get_free_heap_size();
        ESP_LOGI(TAG, "SD card: %s, %luMB — logging enabled (mount cost: %u bytes)",
                 card->cid.name,
                 (unsigned long)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024)),
                 (unsigned)mount_cost);

        // Install custom vprintf handler (always — needed for "log on" command).
        // The stored preference is deliberately NOT read here: the card carries
        // CGM data only unless someone asks for the diary in this session, so a
        // device that once had capture on does not keep filling S*.LOG forever.
        serial_mutex = xSemaphoreCreateMutex();
        if (serial_mutex) {
            esp_log_set_vprintf(serial_capture_vprintf);
            ESP_LOGI(TAG, "SD card ready — glucose CSV only. Serial diary is OFF "
                          "at every boot; type 'log on' to record this session.");
        }
    } else {
        // No card — free the SPI bus to reclaim memory (~0.5KB + DMA)
        mounted_card = NULL;
        spi_bus_free(SD_SPI_HOST);
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
        ESP_LOGI(TAG, "No SD card — logging disabled (0 bytes used)");
    }

    return ESP_OK;  // Always succeed — missing card is not fatal
}

void sd_log(const char *tag, const char *fmt, ...)
{
    // Forward to ESP_LOGI — serial capture picks it up into S*.LOG
    va_list args;
    va_start(args, fmt);
    esp_log_writev(ESP_LOG_INFO, tag ? tag : "SD_LOG", fmt, args);
    va_end(args);
}

// One line per failure run, not per flush: a full card fails every flush forever
// and the log itself is one of the things being written.
static void sd_report_write_error(const char *what, int err)
{
    if (sd_write_error_logged) return;
    sd_write_error_logged = true;
    // FATFS can short-write without setting errno, so the cause is not always
    // available and the message must not claim one it does not have.
    ESP_LOGW(TAG, "SD write failed (%s): %s — card may be full or write-protected",
             what, err ? strerror(err) : "short write, no errno reported");
}

esp_err_t sd_logger_flush(void)
{
    if (!mounted_card || sd_suspended) return ESP_ERR_NOT_FOUND;
    if (!log_mutex) return ESP_ERR_INVALID_STATE;

    // Skip the flush when the heap is too low for the ~4KB contiguous SD DMA
    // buffers. Data stays buffered for the next attempt, so an SSL-alive period
    // cannot trip the five-strike suspension.
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largest_block < 4096) {
        ESP_LOGD(TAG, "Flush deferred: heap too low for DMA (%lu)", (unsigned long)largest_block);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Pause serial capture to prevent re-entrancy from file I/O ESP_LOG calls.
    // Only the mutex holder may write this flag: a caller that gave up waiting
    // must not un-pause capture while another flush is still writing.
    serial_capture_paused = true;

    // Snapshot serial buffer (separate mutex, brief hold)
    int serial_flush_len = 0;
    if (serial_capture_enabled && serial_mutex) {
        if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            serial_buffer[serial_buffer_pos] = '\0';
            serial_flush_len = serial_buffer_pos;
            serial_buffer_pos = 0;  // Reset — capture is paused, safe to reset
            xSemaphoreGive(serial_mutex);
        }
    }

    if (glucose_buffer_pos == 0 && serial_flush_len == 0) {
        serial_capture_paused = false;
        xSemaphoreGive(log_mutex);
        return ESP_OK;
    }

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    // Skip flush if RTC hasn't synced yet — buffer retains data for next flush
    if (ti.tm_year <= 100) {
        // Restore serial buffer since we can't flush yet
        if (serial_flush_len > 0 && serial_mutex) {
            if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                serial_buffer_pos = serial_flush_len;
                xSemaphoreGive(serial_mutex);
            }
        }
        serial_capture_paused = false;
        xSemaphoreGive(log_mutex);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    bool any_write_failed = false;

    // Write glucose CSV if buffered
    if (glucose_buffer_pos > 0) {
        glucose_buffer[glucose_buffer_pos] = '\0';
        char gpath[32];
        snprintf(gpath, sizeof(gpath), MOUNT_POINT "/G%02d%02d%02d.CSV",
                 ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);

        // Only a file that does not exist yet gets the column names. A day file
        // written by an older build already has rows and must never gain one.
        struct stat gst;
        size_t header_len = 0;
        if (stat(gpath, &gst) != 0 || gst.st_size == 0) {
            header_len = strlen(SD_GLUCOSE_CSV_HEADER);
        }

        errno = 0;
        FILE *gf = fopen(gpath, "a");
        if (gf) {
            errno = 0;
            // A full volume shows up as a short write or a failing close, never
            // as a failed open — the day-file already exists and needs no cluster.
            size_t written = 0;
            if (header_len > 0) {
                written += fwrite(SD_GLUCOSE_CSV_HEADER, 1, header_len, gf);
            }
            written += fwrite(glucose_buffer, 1, glucose_buffer_pos, gf);
            int write_errno = errno;
            bool closed_ok = (fclose(gf) == 0);
            if (written != header_len + (size_t)glucose_buffer_pos || !closed_ok) {
                sd_report_write_error("glucose CSV", write_errno ? write_errno : errno);
                any_write_failed = true;
            }
        } else {
            sd_report_write_error("glucose CSV open", errno);
            any_write_failed = true;
        }
        glucose_buffer_pos = 0;
    }

    // Write serial capture log if buffered
    if (serial_flush_len > 0) {
        char spath[32];
        snprintf(spath, sizeof(spath), MOUNT_POINT "/S%02d%02d%02d.LOG",
                 ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);
        errno = 0;
        FILE *sf = fopen(spath, "a");
        if (sf) {
            errno = 0;
            size_t written = fwrite(serial_buffer, 1, serial_flush_len, sf);
            int write_errno = errno;
            bool closed_ok = (fclose(sf) == 0);
            if (written != (size_t)serial_flush_len || !closed_ok) {
                sd_report_write_error("serial log", write_errno ? write_errno : errno);
                any_write_failed = true;
            }
        } else {
            sd_report_write_error("serial log open", errno);
            any_write_failed = true;
        }
    }

    // Track consecutive failures (card removal detection)
    if (any_write_failed) {
        sd_flush_fail_count++;
        if (sd_flush_fail_count >= SD_SUSPEND_AFTER_FAILURES) {
            sd_suspended = true;
            sd_resume_probe_us = 0;  // Each suspension earns one immediate probe
            serial_capture_enabled = false;
            glucose_buffer_pos = 0;
            if (serial_mutex && xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                serial_buffer_pos = 0;
                xSemaphoreGive(serial_mutex);
            }
            ESP_LOGW(TAG, "SD logging SUSPENDED after %d consecutive write failures",
                     sd_flush_fail_count);
        }
        ret = ESP_FAIL;
    } else {
        sd_flush_fail_count = 0;
        sd_write_error_logged = false;
    }

    serial_capture_paused = false;
    xSemaphoreGive(log_mutex);
    return ret;
}

// The three words below are the CSV's vocabulary. Each maps an existing enum;
// none of them may grow a case that names an account, a network or a place.
static const char *csv_provider_word(cgm_provider_t provider)
{
    switch (provider) {
        case CGM_PROVIDER_LIBRE:      return "libre";
        case CGM_PROVIDER_NIGHTSCOUT: return "nightscout";
        case CGM_PROVIDER_DEXCOM:     return "dexcom";
    }
    return "dexcom";
}

static const char *csv_status_word(dexcom_status_t status)
{
    switch (status) {
        case GLUCOSE_STATUS_OK:                return "ok";
        case GLUCOSE_STATUS_WARMUP:            return "warmup";
        case GLUCOSE_STATUS_SIGNAL_LOSS:       return "signal-loss";
        case GLUCOSE_STATUS_NOT_AUTHENTICATED: return "not-authenticated";
        case GLUCOSE_STATUS_NO_DATA:           return "unknown";
    }
    return "unknown";
}

// Empty, not a word, when no tier is in force: a reader filtering on this column
// should see nothing rather than a value it has to special-case.
static const char *csv_alarm_word(active_alarm_state_t state)
{
    switch (state) {
        case ALARM_STATE_HIGH_ALARM:   return "high-alarm";
        case ALARM_STATE_HIGH_WARNING: return "high-warning";
        case ALARM_STATE_LOW_WARNING:  return "low-warning";
        case ALARM_STATE_LOW_ALARM:    return "low-alarm";
        case ALARM_STATE_NONE:         return "";
    }
    return "";
}

void sd_log_glucose(int mg_dl, const char *trend, cgm_provider_t provider)
{
    if (!mounted_card || !log_mutex || sd_suspended) return;

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    // Skip if RTC not synced
    if (ti.tm_year <= 100) return;

    // Sampled before the mutex: none of it needs the lock, and the WiFi driver
    // call is the one part of this that can take more than a few microseconds.
    bool age_known = false;
    int age_min = cygm_glucose_age_min(&age_known);
    if (!age_known) age_min = -1;

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    int remaining = SD_GLUCOSE_BUFFER_SIZE - glucose_buffer_pos - 2;
    if (remaining < SD_GLUCOSE_LINE_MAX) {
        xSemaphoreGive(log_mutex);
        return;
    }

    int written = snprintf(glucose_buffer + glucose_buffer_pos, remaining,
                           "%04d-%02d-%02d %02d:%02d:%02d,%d,%s,%ld,%s,%s,%d,%d,%.2f,%d,%d,%lu,%ld,%s\n",
                           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                           ti.tm_hour, ti.tm_min, ti.tm_sec,
                           mg_dl, trend ? trend : "?",
                           (long)glucose_timestamp,
                           csv_provider_word(provider),
                           csv_status_word(glucose_status),
                           age_min,
                           battery_percent, battery_voltage,
                           battery_is_charging() ? 1 : 0,
                           rssi,
                           (unsigned long)esp_get_free_heap_size(),
                           (long)(esp_timer_get_time() / 1000000),
                           csv_alarm_word(current_alarm_state));
    if (written > 0 && written < remaining) {
        glucose_buffer_pos += written;
    }

    xSemaphoreGive(log_mutex);
}

bool sd_logger_available(void)
{
    return mounted_card != NULL && !sd_suspended;
}

void sd_logger_resume(void)
{
    if (!mounted_card || !log_mutex) return;
    if (!sd_suspended) return;

    // Callers reach here from the failed-fetch path, which can run every 90
    // seconds. A suspension only lifts on evidence that the card answers again,
    // and the probe that gathers it costs an SD command, so it is rate-limited.
    int64_t now_us = esp_timer_get_time();
    if (sd_resume_probe_us != 0 && (now_us - sd_resume_probe_us) < SD_RESUME_PROBE_INTERVAL_US) {
        return;
    }

    // Same floor the flush uses: SD I/O needs contiguous DMA memory, and a
    // command that fails for want of heap says nothing about the card.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4096) return;

    // Every other card access in this module is serialised by log_mutex, and a
    // screenshot can be part-way through writing to the same card.
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    sd_resume_probe_us = now_us;
    esp_err_t probe = sdmmc_get_status(mounted_card);
    if (probe == ESP_OK) {
        sd_suspended = false;
        sd_flush_fail_count = 0;
        sd_write_error_logged = false;
        // Restore this session's choice; a suspension must not turn capture on.
        serial_capture_enabled = serial_capture_requested;
    }
    xSemaphoreGive(log_mutex);

    if (probe != ESP_OK) {
        ESP_LOGD(TAG, "SD card still not responding — logging stays suspended");
        return;
    }

    ESP_LOGI(TAG, "SD logging RESUMED (card responded to probe, serial diary %s)",
             serial_capture_enabled ? "ON" : "OFF");
}

esp_err_t sd_card_mount(sdmmc_card_t **out_card)
{
    if (!mounted_card) return ESP_ERR_NOT_FOUND;
    if (!log_mutex || !out_card) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    serial_capture_paused = true;
    *out_card = mounted_card;  // Already mounted — just return the handle
    return ESP_OK;
}

void sd_card_unmount(sdmmc_card_t *card)
{
    (void)card;  // Don't actually unmount — card stays mounted permanently
    serial_capture_paused = false;
    if (log_mutex) {
        xSemaphoreGive(log_mutex);
    }
}

void sd_serial_capture_set(bool enabled)
{
    if (!mounted_card || sd_suspended) return;
    serial_capture_requested = enabled;
    serial_capture_enabled = enabled;
    ESP_LOGI(TAG, "Serial diary %s (this session only — off again after a reboot)",
             enabled ? "ON" : "OFF");
}

bool sd_serial_capture_get(void)
{
    return serial_capture_enabled;
}

// ==================== Glucose File Access (serial console viewer) ====================

// Entries stream out in whatever order the directory yields them, so a card
// holding years of day-files costs no more RAM than one holding a week. The
// reader sorts; SDLS-END carries the count so a truncated listing is visible.
#define SD_GLUCOSE_CHUNK     512

bool sd_glucose_name_valid(const char *name, char *out_upper, size_t out_len)
{
    if (!name || strlen(name) != 11) return false;
    if (toupper((unsigned char)name[0]) != 'G') return false;
    for (int i = 1; i < 7; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    if (name[7] != '.') return false;
    if (toupper((unsigned char)name[8]) != 'C' ||
        toupper((unsigned char)name[9]) != 'S' ||
        toupper((unsigned char)name[10]) != 'V') return false;

    if (out_upper) {
        if (out_len < 12) return false;
        for (int i = 0; i < 11; i++) {
            out_upper[i] = (char)toupper((unsigned char)name[i]);
        }
        out_upper[11] = '\0';
    }
    return true;
}

sd_glucose_status_t sd_glucose_list(sd_glucose_ready_fn on_ready,
                                    sd_glucose_entry_fn on_entry,
                                    void *ctx, int *out_count)
{
    if (out_count) *out_count = 0;
    if (!mounted_card || !log_mutex) return SD_GLUCOSE_NO_CARD;
    if (sd_suspended) return SD_GLUCOSE_SUSPENDED;
    if (!on_entry) return SD_GLUCOSE_OPEN_FAILED;

    // Today's file must carry the buffered lines before its size is reported.
    sd_logger_flush();

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return SD_GLUCOSE_OPEN_FAILED;
    }
    serial_capture_paused = true;

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        serial_capture_paused = false;
        xSemaphoreGive(log_mutex);
        return SD_GLUCOSE_OPEN_FAILED;
    }

    if (on_ready) on_ready(ctx);

    int n = 0;
    unsigned chunks = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char upper[12];
        if (!sd_glucose_name_valid(ent->d_name, upper, sizeof(upper))) continue;

        char path[32];
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", upper);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        on_entry(upper, (uint32_t)st.st_size, ctx);
        n++;

        // The console TX path busy-waits on the UART at priority 2; a card with
        // years of files would otherwise starve IDLE0 past the task watchdog.
        if ((++chunks & 15) == 0) vTaskDelay(1);
    }
    closedir(dir);

    serial_capture_paused = false;
    xSemaphoreGive(log_mutex);

    if (out_count) *out_count = n;
    return SD_GLUCOSE_OK;
}

sd_glucose_status_t sd_glucose_read(const char *name,
                                    sd_glucose_open_fn on_open,
                                    sd_glucose_data_fn on_data,
                                    void *ctx, uint32_t *out_crc32)
{
    char upper[12];
    if (!sd_glucose_name_valid(name, upper, sizeof(upper))) return SD_GLUCOSE_BAD_NAME;
    if (!mounted_card || !log_mutex) return SD_GLUCOSE_NO_CARD;
    if (sd_suspended) return SD_GLUCOSE_SUSPENDED;

    sd_logger_flush();

    // Held across stat, read and emit: a flush part-way through would append to
    // today's file and leave the reported size short of the bytes sent.
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return SD_GLUCOSE_READ_FAILED;
    serial_capture_paused = true;

    char path[32];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", upper);

    sd_glucose_status_t status = SD_GLUCOSE_OK;
    struct stat st;
    FILE *f = NULL;
    if (stat(path, &st) != 0 || (f = fopen(path, "rb")) == NULL) {
        status = SD_GLUCOSE_NOT_FOUND;
    } else {
        if (on_open) on_open((uint32_t)st.st_size, ctx);

        uint32_t crc = 0;
        char buf[SD_GLUCOSE_CHUNK];
        size_t got;
        unsigned chunks = 0;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            crc = esp_rom_crc32_le(crc, (const uint8_t *)buf, got);
            if (on_data) on_data(buf, got, ctx);
            // The console TX path busy-waits on the UART at priority 2; without
            // a yield a file over ~55 KB starves IDLE0 past the 5 s task watchdog.
            if ((++chunks & 7) == 0) vTaskDelay(1);
        }
        if (ferror(f)) status = SD_GLUCOSE_READ_FAILED;
        fclose(f);
        if (out_crc32) *out_crc32 = crc;
    }

    serial_capture_paused = false;
    xSemaphoreGive(log_mutex);
    return status;
}

void sd_logger_shutdown(void)
{
    if (mounted_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);
        mounted_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted for shutdown");
    }
}
