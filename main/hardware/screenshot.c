/*
 * Screenshot Capture — BMP to SD Card
 *
 * Approach: intercept LVGL's flush callback during lv_refr_now().
 * LVGL re-renders the full screen in 320×30 chunks (its draw buffer size).
 * Our callback converts each chunk from byte-swapped RGB565 to 24-bit BGR
 * and writes it to the BMP file. After all 8 chunks, we restore the
 * original callback and unmount the SD card.
 *
 * Memory cost: ~960 bytes static (one row conversion buffer)
 * Time: ~200-400ms for a full 320×240 capture
 */

#include "screenshot.h"
#include "shared_state.h"
#include "sd_logger.h"
#include "dexcom_api.h"
#include "main.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "hardware/battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "driver/uart.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SCREENSHOT";

// ==================== BMP File Format ====================

#pragma pack(push, 1)
typedef struct {
    uint16_t bf_type;        // 'BM' = 0x4D42
    uint32_t bf_size;        // Total file size
    uint16_t bf_reserved1;
    uint16_t bf_reserved2;
    uint32_t bf_off_bits;    // Offset to pixel data (54 for 24-bit)
} bmp_file_header_t;

typedef struct {
    uint32_t bi_size;            // Header size (40)
    int32_t  bi_width;
    int32_t  bi_height;          // Negative = top-down scanline order
    uint16_t bi_planes;          // 1
    uint16_t bi_bit_count;       // 24
    uint32_t bi_compression;     // 0 = BI_RGB
    uint32_t bi_size_image;      // Pixel data size (may be 0 for BI_RGB)
    int32_t  bi_x_pels_per_meter;
    int32_t  bi_y_pels_per_meter;
    uint32_t bi_clr_used;
    uint32_t bi_clr_important;
} bmp_info_header_t;
#pragma pack(pop)

// ==================== Screenshot Context ====================

typedef struct {
    FILE *file;
    void (*original_flush_cb)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
    void *original_user_data;
    int rows_written;
    bool error;
} screenshot_ctx_t;

// Static row conversion buffer: 320 pixels × 3 bytes = 960 bytes
static uint8_t row_buf[LCD_WIDTH * 3];

// ==================== Flush Callback Interception ====================

static void screenshot_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    screenshot_ctx_t *ctx = (screenshot_ctx_t *)drv->user_data;

    if (!ctx->error && ctx->file) {
        int width = area->x2 - area->x1 + 1;
        int height = area->y2 - area->y1 + 1;

        for (int y = 0; y < height; y++) {
            uint16_t *row = (uint16_t *)color_p + (y * width);

            for (int x = 0; x < width; x++) {
                // LV_COLOR_16_SWAP=y: bytes are big-endian for SPI
                // Swap back to native little-endian for correct extraction
                uint16_t raw = row[x];
                uint16_t pixel = (raw >> 8) | (raw << 8);

                // Extract RGB565 and scale to 8-bit
                uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
                uint8_t g = ((pixel >> 5) & 0x3F) * 255 / 63;
                uint8_t b = (pixel & 0x1F) * 255 / 31;

                // BMP stores BGR order
                row_buf[x * 3 + 0] = b;
                row_buf[x * 3 + 1] = g;
                row_buf[x * 3 + 2] = r;
            }

            size_t written = fwrite(row_buf, 1, width * 3, ctx->file);
            if (written != (size_t)(width * 3)) {
                ctx->error = true;
                break;
            }
            ctx->rows_written++;
        }
    }

    // Call original flush callback to keep display updated
    // Temporarily restore original user_data in case it uses it
    drv->user_data = ctx->original_user_data;
    ctx->original_flush_cb(drv, area, color_p);
    drv->user_data = ctx;
}

// ==================== BMP Header ====================

static bool write_bmp_header(FILE *f, int width, int height)
{
    uint32_t row_bytes = width * 3;
    // BMP rows must be 4-byte aligned (320×3 = 960, already aligned)
    uint32_t pixel_data_size = row_bytes * height;

    bmp_file_header_t fh = {
        .bf_type = 0x4D42,  // 'BM'
        .bf_size = sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t) + pixel_data_size,
        .bf_reserved1 = 0,
        .bf_reserved2 = 0,
        .bf_off_bits = sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t),
    };

    bmp_info_header_t ih = {
        .bi_size = sizeof(bmp_info_header_t),
        .bi_width = width,
        .bi_height = -height,  // Negative = top-down (matches LVGL render order)
        .bi_planes = 1,
        .bi_bit_count = 24,
        .bi_compression = 0,   // BI_RGB
        .bi_size_image = pixel_data_size,
        .bi_x_pels_per_meter = 0,
        .bi_y_pels_per_meter = 0,
        .bi_clr_used = 0,
        .bi_clr_important = 0,
    };

    if (fwrite(&fh, sizeof(fh), 1, f) != 1) return false;
    if (fwrite(&ih, sizeof(ih), 1, f) != 1) return false;
    return true;
}

// ==================== Task Restart Helper ====================

static void screenshot_restart_tasks(bool restore_pause_state)
{
    // Recreate all tasks we deleted, matching the original creation parameters
    // from background_tasks.c. Singleton guards prevent double-creation.
    if (weather_task_handle == NULL) {
        xTaskCreate(weather_update_task, "weather_update", 8192, NULL, 1, &weather_task_handle);
        ESP_LOGI(TAG, "Restarted weather task");
    }
    if (glucose_task_handle == NULL) {
        xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
        ESP_LOGI(TAG, "Restarted glucose task");
    }
    if (time_task_handle == NULL) {
        xTaskCreate(time_update_task, "time_update", 4096, NULL, 1, &time_task_handle);
        ESP_LOGI(TAG, "Restarted time task");
    }
    if (battery_task_handle == NULL) {
        xTaskCreatePinnedToCore(battery_monitor_task, "battery_mon", 3072, NULL, 2, &battery_task_handle, 0);
        ESP_LOGI(TAG, "Restarted battery task");
    }

    pause_background_tasks = restore_pause_state;
    ESP_LOGI(TAG, "All tasks restarted, pause_background_tasks=%d", restore_pause_state);
}

// ==================== Screenshot Capture ====================

esp_err_t screenshot_take(void)
{
    if (!sd_logger_available()) {
        ESP_LOGW(TAG, "No SD card — cannot save screenshot");
        return ESP_ERR_NOT_FOUND;
    }

    if (!lvgl_display) {
        ESP_LOGE(TAG, "LVGL display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Capturing screenshot...");
    int64_t start_ms = esp_timer_get_time() / 1000;

    // Step 1: Aggressively free memory — same pattern as update_stop_all_tasks()
    // The FAT mount needs ~12KB+ contiguous heap. TLSF can't defragment, so we
    // must delete FreeRTOS tasks to free their stack blocks and let TLSF coalesce
    // adjacent free regions into one large contiguous block.
    bool was_paused = pause_background_tasks;
    pause_background_tasks = true;

    dexcom_close_persistent_client();

    // Fence: ensure no task is mid-SSL-operation
    if (network_mutex) {
        if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
            xSemaphoreGive(network_mutex);
        }
    }

    // Wait for tasks to reach their sleep/delay state
    vTaskDelay(pdMS_TO_TICKS(500));

    // Delete non-essential tasks to free their stack memory (~19KB total)
    // weather=8KB, glucose=4KB, time=4KB, battery=3KB
    // Unlike OTA (which reboots), we recreate these after the screenshot.
    if (weather_task_handle != NULL) {
        vTaskDelete(weather_task_handle);
        weather_task_handle = NULL;
        ESP_LOGI(TAG, "Deleted weather task (8KB)");
    }
    if (glucose_task_handle != NULL) {
        vTaskDelete(glucose_task_handle);
        glucose_task_handle = NULL;
        ESP_LOGI(TAG, "Deleted glucose task (4KB)");
    }
    if (time_task_handle != NULL) {
        vTaskDelete(time_task_handle);
        time_task_handle = NULL;
        ESP_LOGI(TAG, "Deleted time task (4KB)");
    }
    if (battery_task_handle != NULL) {
        vTaskDelete(battery_task_handle);
        battery_task_handle = NULL;
        ESP_LOGI(TAG, "Deleted battery task (3KB)");
    }

    // Yield to idle task so it frees deleted task memory (TLSF coalesces)
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Memory freed — heap: %lu, largest_block: %u",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    // Step 2: Mount SD card (acquires log_mutex, pauses serial capture)
    sdmmc_card_t *card = NULL;
    esp_err_t ret = sd_card_mount(&card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        screenshot_restart_tasks(was_paused);
        return ret;
    }

    // Step 3: Build timestamp filename (8.3 FAT: SSHHMMss.BMP)
    char filepath[32];
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year > 100) {
        snprintf(filepath, sizeof(filepath), SD_MOUNT_POINT "/SS%02d%02d%02d.BMP",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        // RTC not synced — use boot time in seconds
        int boot_sec = (int)(esp_timer_get_time() / 1000000);
        snprintf(filepath, sizeof(filepath), SD_MOUNT_POINT "/SS%06d.BMP", boot_sec % 1000000);
    }

    // Step 4: Open file and write BMP header
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", filepath);
        sd_card_unmount(card);
        screenshot_restart_tasks(was_paused);
        return ESP_FAIL;
    }

    if (!write_bmp_header(f, LCD_WIDTH, LCD_HEIGHT)) {
        ESP_LOGE(TAG, "Failed to write BMP header");
        fclose(f);
        sd_card_unmount(card);
        screenshot_restart_tasks(was_paused);
        return ESP_FAIL;
    }

    // Step 5: Lock LVGL for exclusive access
    if (!lvgl_port_lock(5000)) {
        ESP_LOGE(TAG, "LVGL lock timeout");
        fclose(f);
        sd_card_unmount(card);
        screenshot_restart_tasks(was_paused);
        return ESP_ERR_TIMEOUT;
    }

    // Step 6: Intercept flush callback
    screenshot_ctx_t ctx = {
        .file = f,
        .original_flush_cb = lvgl_display->driver->flush_cb,
        .original_user_data = lvgl_display->driver->user_data,
        .rows_written = 0,
        .error = false,
    };
    lvgl_display->driver->flush_cb = screenshot_flush_cb;
    lvgl_display->driver->user_data = &ctx;

    // Step 7: Force full-screen re-render
    // Invalidate all layers to capture overlays too
    lv_obj_invalidate(lv_scr_act());
    lv_obj_t *top_layer = lv_disp_get_layer_top(lvgl_display);
    lv_obj_t *sys_layer = lv_disp_get_layer_sys(lvgl_display);
    if (top_layer) lv_obj_invalidate(top_layer);
    if (sys_layer) lv_obj_invalidate(sys_layer);

    lv_refr_now(lvgl_display);

    // Step 8: Restore original flush callback
    lvgl_display->driver->flush_cb = ctx.original_flush_cb;
    lvgl_display->driver->user_data = ctx.original_user_data;

    lvgl_port_unlock();

    // Step 9: Close file and unmount
    fclose(f);
    sd_card_unmount(card);

    // Step 10: Recreate deleted tasks and resume
    screenshot_restart_tasks(was_paused);

    int64_t elapsed = (esp_timer_get_time() / 1000) - start_ms;

    if (ctx.error || ctx.rows_written != LCD_HEIGHT) {
        ESP_LOGE(TAG, "Screenshot incomplete: %d/%d rows written", ctx.rows_written, LCD_HEIGHT);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Screenshot saved: %s (%d rows, %lldms)", filepath, ctx.rows_written, elapsed);
    return ESP_OK;
}

// ==================== Serial Command Listener ====================

static void screenshot_cmd_task(void *arg)
{
    // Install UART driver for reading if not already installed
    esp_err_t ret = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART driver failed: %s — command listener disabled", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Screenshot command listener ready (type 'ss' + Enter)");

    char cmd_buf[16];
    int cmd_pos = 0;
    uint8_t byte;

    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(200));
        if (len <= 0) continue;

        if (byte == '\n' || byte == '\r') {
            if (cmd_pos > 0) {
                cmd_buf[cmd_pos] = '\0';

                if (strcmp(cmd_buf, "ss") == 0 || strcmp(cmd_buf, "screenshot") == 0) {
                    screenshot_take();
                } else if (strcmp(cmd_buf, "help") == 0) {
                    ESP_LOGI(TAG, "Commands: ss | screenshot | help");
                }

                cmd_pos = 0;
            }
        } else if (cmd_pos < (int)sizeof(cmd_buf) - 1) {
            cmd_buf[cmd_pos++] = (char)byte;
        }
    }
}

// ==================== Init ====================

esp_err_t screenshot_init(void)
{
    if (!sd_logger_available()) {
        ESP_LOGI(TAG, "No SD card — screenshot disabled");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        screenshot_cmd_task,
        "ss_cmd",
        4096,
        NULL,
        2,    // Low priority — just listening for commands
        NULL,
        0     // Core 0 — keeps Core 1 free for LVGL
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create command listener task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
