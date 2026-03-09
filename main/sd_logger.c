/*
 * SD Card Logger — v2 (Mount-on-Write)
 *
 * Key design: SPI bus stays initialized (~0.5KB), log lines buffer in static
 * 2KB RAM, flush mounts FAT temporarily (~10KB for ~200ms) then unmounts.
 * Total permanent cost: ~2.5KB vs old v1's 13KB that broke SSL.
 */

#include "sd_logger.h"
#include "esp_log.h"
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

// Log buffer — static allocation, no malloc needed
#define SD_LOG_BUFFER_SIZE  2048
#define SD_LOG_LINE_MAX     200

static char log_buffer[SD_LOG_BUFFER_SIZE];
static int log_buffer_pos = 0;
static SemaphoreHandle_t log_mutex = NULL;

// Glucose CSV buffer (simple: datetime,value,trend)
#define SD_GLUCOSE_BUFFER_SIZE  512
static char glucose_buffer[SD_GLUCOSE_BUFFER_SIZE];
static int glucose_buffer_pos = 0;

// SPI/SD state
static bool spi_initialized = false;
static bool card_detected = false;
static sdmmc_host_t host;
static sdspi_device_config_t slot_config;

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
        ESP_LOGI(TAG, "SD card: %s, %lluMB — logging enabled",
                 card->cid.name,
                 ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
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
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (log_buffer_pos == 0 && glucose_buffer_pos == 0) {
        xSemaphoreGive(log_mutex);
        return ESP_OK;
    }

    // Null-terminate buffer
    log_buffer[log_buffer_pos] = '\0';
    int flush_len = log_buffer_pos;

    // Build daily filename (8.3 FAT format: C260308.LOG = Mar 8, 2026)
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    // Skip flush if RTC hasn't synced yet — buffer retains data for next flush
    if (ti.tm_year <= 100) {
        xSemaphoreGive(log_mutex);
        return ESP_OK;
    }

    char filepath[32];
    snprintf(filepath, sizeof(filepath), MOUNT_POINT "/C%02d%02d%02d.LOG",
             ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);

    // Mount SD card (temporary ~10KB allocation)
    sdmmc_card_t *card = NULL;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,  // System log + glucose CSV
        .allocation_unit_size = 0,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_NO_MEM) {
            card_detected = false;  // Card removed or no memory
        }
        log_buffer_pos = 0;  // Clear buffer to prevent infinite retries
        xSemaphoreGive(log_mutex);
        return ret;
    }

    // Write system log if buffered
    if (flush_len > 0) {
        FILE *f = fopen(filepath, "a");
        if (f) {
            size_t written = fwrite(log_buffer, 1, flush_len, f);
            fclose(f);
            if ((int)written == flush_len) {
                ESP_LOGI(TAG, "Flushed %d bytes → %s", flush_len, filepath);
            } else {
                ESP_LOGW(TAG, "Partial write: %d/%d bytes", (int)written, flush_len);
            }
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to open %s for append", filepath);
            ret = ESP_FAIL;
        }
    }

    // Write glucose CSV if buffered (same mount, no extra I/O overhead)
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

    // Unmount immediately to free ~10KB
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);

    // Clear buffer regardless of write result
    log_buffer_pos = 0;

    xSemaphoreGive(log_mutex);
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
