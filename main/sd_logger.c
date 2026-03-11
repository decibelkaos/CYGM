/*
 * SD Card Logger — v2.1 (Mount-on-Write + Serial Capture)
 *
 * Key design: SPI bus stays initialized (~0.5KB), log lines buffer in static
 * RAM, flush mounts FAT temporarily (~10KB for ~200ms) then unmounts.
 *
 * Three log streams:
 *   C*.LOG — Curated system log (explicit sd_log() calls only)
 *   G*.CSV — Glucose readings (datetime, value, trend)
 *   S*.LOG — Full serial capture (ALL ESP_LOG output, raw)
 *
 * Permanent cost: ~10.5KB (2KB curated + 512B glucose + 8KB serial + mutexes)
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
static bool spi_initialized = false;
static bool card_detected = false;
static sdmmc_host_t host;
static sdspi_device_config_t slot_config;

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
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // SPI bus can't init — no SD logging possible, silently disable
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

    // Probe: mount briefly to check card presence, then unmount
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
        card_detected = true;
        spi_initialized = true;
        ESP_LOGI(TAG, "SD card: %s, %luMB — logging enabled",
                 card->cid.name,
                 (unsigned long)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024)));
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);

        // Enable serial capture — intercept ALL ESP_LOG output
        serial_mutex = xSemaphoreCreateMutex();
        if (serial_mutex) {
            serial_capture_enabled = true;
            esp_log_set_vprintf(serial_capture_vprintf);
            ESP_LOGI(TAG, "Serial capture to SD enabled (8KB buffer → S*.LOG)");
        }
    } else {
        // No card — free the SPI bus to reclaim memory (~0.5KB + DMA)
        card_detected = false;
        spi_initialized = false;
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
    if (!spi_initialized || !card_detected || !log_mutex) return;
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
    if (!spi_initialized || !card_detected) return ESP_ERR_NOT_FOUND;
    if (!log_mutex) return ESP_ERR_INVALID_STATE;

    // Pause serial capture to prevent re-entrancy from mount/write ESP_LOG calls
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

    char filepath[32];
    snprintf(filepath, sizeof(filepath), MOUNT_POINT "/C%02d%02d%02d.LOG",
             ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);

    // Mount SD card (temporary ~10KB allocation)
    // Pre-check: need ~12KB contiguous for FAT structures — skip if fragmented
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largest_block < 15000) {
        ESP_LOGW(TAG, "SD flush deferred (largest_block=%u, need ~12KB)", (unsigned)largest_block);
        // Restore serial buffer since we couldn't write it
        if (serial_flush_len > 0 && serial_mutex) {
            if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                serial_buffer_pos = serial_flush_len;
                xSemaphoreGive(serial_mutex);
            }
        }
        xSemaphoreGive(log_mutex);
        serial_capture_paused = false;
        return ESP_ERR_NO_MEM;
    }

    sdmmc_card_t *card = NULL;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 1,  // Files opened/closed sequentially — 1 at a time
        .allocation_unit_size = 0,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s (heap=%lu, largest=%u)", esp_err_to_name(ret),
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (ret == ESP_ERR_NOT_FOUND) {
            card_detected = false;  // Card physically removed — disable permanently
            log_buffer_pos = 0;
        }
        // ESP_ERR_NO_MEM: keep card_detected=true — heap will free up later.
        // Don't clear buffers — data will flush on next successful attempt.
        // Restore serial buffer since we couldn't write it
        if (serial_flush_len > 0 && serial_mutex) {
            if (xSemaphoreTake(serial_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                serial_buffer_pos = serial_flush_len;
                xSemaphoreGive(serial_mutex);
            }
        }
        xSemaphoreGive(log_mutex);
        serial_capture_paused = false;
        return ret;
    }

    // Write curated system log if buffered
    if (flush_len > 0) {
        FILE *f = fopen(filepath, "a");
        if (f) {
            size_t written = fwrite(log_buffer, 1, flush_len, f);
            fclose(f);
            if ((int)written != flush_len) {
                ESP_LOGW(TAG, "Partial write: %d/%d bytes", (int)written, flush_len);
            }
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to open %s for append", filepath);
            ret = ESP_FAIL;
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
        }
    }

    // Unmount immediately to free ~10KB
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);

    // Clear curated buffer regardless of write result
    log_buffer_pos = 0;

    xSemaphoreGive(log_mutex);
    serial_capture_paused = false;
    return ret;
}

void sd_log_glucose(int mg_dl, const char *trend)
{
    if (!spi_initialized || !card_detected || !log_mutex) return;
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
    return spi_initialized && card_detected;
}
