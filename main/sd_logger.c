/*
 * SD Card Logger — v3.0 (Persistent Mount + Serial Capture)
 *
 * Key design: SD card mounted ONCE at boot, stays mounted permanently.
 * Flush just opens/writes/closes files — no mount/unmount heap churn.
 * This prevents heap fragmentation that kills SSL reconnection on ESP32.
 *
 * Three log streams:
 *   C*.LOG — Curated system log (explicit sd_log() calls only)
 *   G*.CSV — Glucose readings (datetime, value, trend)
 *   S*.LOG — Full serial capture (ALL ESP_LOG output, raw)
 *
 * Permanent cost: ~12KB (2KB curated + 512B glucose + 8KB serial + ~1.5KB mount)
 */

#include "sd_logger.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SD_LOG";

// Hardware pins (JC2432W328 — VSPI bus, separate from display on HSPI)
#define SD_PIN_CS    5
#define SD_PIN_MOSI  23
#define SD_PIN_MISO  19
#define SD_PIN_SCK   18
#define SD_SPI_HOST  SPI3_HOST
#define MOUNT_POINT  "/sdcard"

// Curated log buffer — explicit sd_log() calls
#define SD_LOG_BUFFER_SIZE  2048
#define SD_LOG_LINE_MAX     200

static char log_buffer[SD_LOG_BUFFER_SIZE];
static int log_buffer_pos = 0;
static SemaphoreHandle_t log_mutex = NULL;

// Glucose CSV buffer (simple: datetime,value,trend)
#define SD_GLUCOSE_BUFFER_SIZE  512
static char glucose_buffer[SD_GLUCOSE_BUFFER_SIZE];
static int glucose_buffer_pos = 0;

// Serial capture buffer — ALL ESP_LOG output
#define SD_SERIAL_BUFFER_SIZE  8192
static char serial_buffer[SD_SERIAL_BUFFER_SIZE];
static int serial_buffer_pos = 0;
static SemaphoreHandle_t serial_mutex = NULL;
static bool serial_capture_enabled = false;
static volatile bool serial_capture_paused = false;

// SPI/SD state
static bool sd_suspended = false;           // True when SD logging is suspended due to persistent failures
static int  sd_flush_fail_count = 0;        // Consecutive flush failures
#define SD_SUSPEND_AFTER_FAILURES  5        // Suspend after this many consecutive failures
static sdmmc_host_t host;
static sdspi_device_config_t slot_config;
static sdmmc_card_t *mounted_card = NULL;   // Persistent mount — stays mounted for device lifetime

// Custom vprintf handler — captures ALL ESP_LOG output to serial buffer
static int serial_capture_vprintf(const char *fmt, va_list args)
{
    // Copy args before consuming — va_list can only be used once
    va_list args_copy;
    va_copy(args_copy, args);

    // Always output to UART serial (original behavior)
    int ret = vprintf(fmt, args);

    // Buffer to SD if enabled, not paused, and not in ISR context
    if (serial_capture_enabled && !serial_capture_paused && serial_mutex) {
        // Non-blocking mutex take — never delay the logging caller
        if (xSemaphoreTake(serial_mutex, 0) == pdTRUE) {
            int remaining = SD_SERIAL_BUFFER_SIZE - serial_buffer_pos;
            if (remaining > 2) {
                int written = vsnprintf(serial_buffer + serial_buffer_pos, remaining, fmt, args_copy);
                if (written > 0) {
                    if (written >= remaining) written = remaining - 1;
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

    // Initialize SPI3 bus for SD card
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

    // Configure SDSPI host and device slot
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

    // Restore log levels
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

        // Enable serial capture — intercept ALL ESP_LOG output
        serial_mutex = xSemaphoreCreateMutex();
        if (serial_mutex) {
            serial_capture_enabled = true;
            esp_log_set_vprintf(serial_capture_vprintf);
            ESP_LOGI(TAG, "Serial capture to SD enabled (8KB buffer → S*.LOG)");
        }
    } else {
        // No card — free the SPI bus to reclaim memory (~0.5KB + DMA)
        mounted_card = NULL;
        spi_bus_free(SD_SPI_HOST);
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
        ESP_LOGI(TAG, "No SD card — logging disabled (0 bytes used)");
    }

    log_buffer_pos = 0;
    return ESP_OK;  // Always succeed — missing card is not fatal
}

void sd_log(const char *tag, const char *fmt, ...)
{
    if (!mounted_card || !log_mutex || sd_suspended) return;
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    int remaining = SD_LOG_BUFFER_SIZE - log_buffer_pos - 2;  // room for \n\0
    if (remaining < SD_LOG_LINE_MAX) {
        // Buffer nearly full — drop this line (will be flushed soon)
        xSemaphoreGive(log_mutex);
        return;
    }

    // Timestamp
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    int written = snprintf(log_buffer + log_buffer_pos, remaining,
                           "[%04d-%02d-%02d %02d:%02d:%02d] %s: ",
                           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                           ti.tm_hour, ti.tm_min, ti.tm_sec,
                           tag ? tag : "LOG");
    if (written <= 0 || written >= remaining) {
        xSemaphoreGive(log_mutex);
        return;
    }
    log_buffer_pos += written;
    remaining -= written;

    // Message
    va_list args;
    va_start(args, fmt);
    written = vsnprintf(log_buffer + log_buffer_pos, remaining, fmt, args);
    va_end(args);

    if (written > 0) {
        if (written >= remaining) written = remaining - 1;
        log_buffer_pos += written;
    }

    // Newline
    if (log_buffer_pos < SD_LOG_BUFFER_SIZE - 1) {
        log_buffer[log_buffer_pos++] = '\n';
    }

    xSemaphoreGive(log_mutex);
}

esp_err_t sd_logger_flush(void)
{
    if (!mounted_card || sd_suspended) return ESP_ERR_NOT_FOUND;
    if (!log_mutex) return ESP_ERR_INVALID_STATE;

    // Pause serial capture to prevent re-entrancy from file I/O ESP_LOG calls
    serial_capture_paused = true;

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        serial_capture_paused = false;
        return ESP_ERR_TIMEOUT;
    }

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

    if (log_buffer_pos == 0 && glucose_buffer_pos == 0 && serial_flush_len == 0) {
        xSemaphoreGive(log_mutex);
        serial_capture_paused = false;
        return ESP_OK;
    }

    // Null-terminate curated buffer
    log_buffer[log_buffer_pos] = '\0';
    int flush_len = log_buffer_pos;

    // Build daily filename (8.3 FAT format: C260308.LOG = Mar 8, 2026)
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
        xSemaphoreGive(log_mutex);
        serial_capture_paused = false;
        return ESP_OK;
    }

    // Card is permanently mounted — no mount needed, no heap guard needed.
    // fopen only allocates ~512 bytes (FIL.buf with DYN_BUFFERS + SECTOR_512).

    char filepath[32];
    snprintf(filepath, sizeof(filepath), MOUNT_POINT "/C%02d%02d%02d.LOG",
             ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);

    esp_err_t ret = ESP_OK;
    bool any_write_failed = false;

    // Write curated system log if buffered
    if (flush_len > 0) {
        FILE *f = fopen(filepath, "a");
        if (f) {
            size_t written = fwrite(log_buffer, 1, flush_len, f);
            fclose(f);
            if ((int)written != flush_len) {
                ESP_LOGW(TAG, "Partial write: %d/%d bytes", (int)written, flush_len);
            }
        } else {
            any_write_failed = true;
        }
    }

    // Write glucose CSV if buffered
    if (glucose_buffer_pos > 0) {
        glucose_buffer[glucose_buffer_pos] = '\0';
        char gpath[32];
        snprintf(gpath, sizeof(gpath), MOUNT_POINT "/G%02d%02d%02d.CSV",
                 ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);
        FILE *gf = fopen(gpath, "a");
        if (gf) {
            fwrite(glucose_buffer, 1, glucose_buffer_pos, gf);
            fclose(gf);
        } else {
            any_write_failed = true;
        }
        glucose_buffer_pos = 0;
    }

    // Write serial capture log if buffered
    if (serial_flush_len > 0) {
        char spath[32];
        snprintf(spath, sizeof(spath), MOUNT_POINT "/S%02d%02d%02d.LOG",
                 ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);
        FILE *sf = fopen(spath, "a");
        if (sf) {
            fwrite(serial_buffer, 1, serial_flush_len, sf);
            fclose(sf);
        } else {
            any_write_failed = true;
        }
    }

    // Track consecutive failures (card removal detection)
    if (any_write_failed) {
        sd_flush_fail_count++;
        if (sd_flush_fail_count >= SD_SUSPEND_AFTER_FAILURES) {
            sd_suspended = true;
            serial_capture_enabled = false;
            log_buffer_pos = 0;
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
    }

    // No unmount — card stays mounted permanently

    // Clear curated buffer regardless of write result
    log_buffer_pos = 0;

    xSemaphoreGive(log_mutex);
    serial_capture_paused = false;
    return ret;
}

void sd_log_glucose(int mg_dl, const char *trend)
{
    if (!mounted_card || !log_mutex || sd_suspended) return;
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    // Skip if RTC not synced
    if (ti.tm_year <= 100) {
        xSemaphoreGive(log_mutex);
        return;
    }

    int remaining = SD_GLUCOSE_BUFFER_SIZE - glucose_buffer_pos - 2;
    if (remaining < 60) {
        xSemaphoreGive(log_mutex);
        return;
    }

    int written = snprintf(glucose_buffer + glucose_buffer_pos, remaining,
                           "%04d-%02d-%02d %02d:%02d:%02d,%d,%s\n",
                           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                           ti.tm_hour, ti.tm_min, ti.tm_sec,
                           mg_dl, trend ? trend : "?");
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
    if (!mounted_card) return;
    if (!sd_suspended) return;

    sd_suspended = false;
    sd_flush_fail_count = 0;
    serial_capture_enabled = true;

    ESP_LOGI(TAG, "SD logging RESUMED (suspension cleared)");
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
    serial_capture_enabled = enabled;
    ESP_LOGI(TAG, "Serial capture %s", enabled ? "ON" : "OFF");
}

bool sd_serial_capture_get(void)
{
    return serial_capture_enabled;
}

void sd_logger_shutdown(void)
{
    if (mounted_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);
        mounted_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted for shutdown");
    }
}
