/*
 * Screenshot Capture — BMP to SD Card
 *
 * Intercepts LVGL's flush callback during lv_refr_now(). LVGL re-renders the full
 * screen in 320x30 chunks; the callback converts each from byte-swapped RGB565 to
 * 24-bit BGR and writes it to the BMP, then the original callback is restored.
 * Costs ~960 bytes of static row buffer and ~200-400ms per capture.
 */

#include "screenshot.h"
#include "shared_state.h"
#include "sd_logger.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "hardware/display.h"
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
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SCREENSHOT";

// Synthetic-touch defaults for the "tap"/"swipe" commands. The ceiling is above
// display.c's own poll clamp, so an over-long request is trimmed there rather
// than rejected here.
#define TAP_DEFAULT_MS     120
#define SWIPE_DEFAULT_MS   300
#define INJECT_MAX_MS    20000

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

// See s_serial_ctx below: drv->user_data must stay the port's own context
// because the SPI flush-done interrupt dereferences it.
static screenshot_ctx_t *s_sd_ctx = NULL;

static void screenshot_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    screenshot_ctx_t *ctx = s_sd_ctx;
    if (ctx == NULL) {
        return;
    }

    if (!ctx->error && ctx->file) {
        int width = area->x2 - area->x1 + 1;
        int height = area->y2 - area->y1 + 1;

        for (int y = 0; y < height; y++) {
            uint16_t *row = (uint16_t *)color_p + (y * width);

            for (int x = 0; x < width; x++) {
                // LV_COLOR_16_SWAP=y leaves bytes big-endian for SPI; swap back.
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
    ctx->original_flush_cb(drv, area, color_p);
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
    // Canonical creation table in background_tasks.c owns stack/priority/core,
    // so a screenshot can never resurrect a task with drifted parameters.
    ensure_tasks_running();

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

    // Step 1: free memory aggressively, as update_stop_all_tasks() does. The FAT
    // mount needs ~12KB contiguous and TLSF cannot defragment, so task stacks have
    // to be deleted for their blocks to coalesce into one large region.
    bool was_paused = pause_background_tasks;
    pause_background_tasks = true;

    dexcom_close_persistent_client();
    libre_close_persistent_client();

    // Fence: ensure no task is mid-SSL-operation
    if (network_mutex) {
        if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
            xSemaphoreGive(network_mutex);
        }
    }

    // Wait for tasks to reach their sleep/delay state
    vTaskDelay(pdMS_TO_TICKS(500));

    // Delete non-essential tasks to free ~19KB of stacks (weather 8K, glucose 4K,
    // time 4K, battery 3K). Unlike OTA, which reboots, these are recreated after.
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

    // Step 5: Lock LVGL for exclusive access (retry ~1s — screenshot is interactive)
    bool locked = false;
    for (int retry = 0; retry < 100 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) {
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
    s_sd_ctx = &ctx;
    lvgl_display->driver->flush_cb = screenshot_flush_cb;

    // Step 7: force a full re-render, invalidating all layers so overlays are caught
    lv_obj_invalidate(lv_scr_act());
    lv_obj_t *top_layer = lv_disp_get_layer_top(lvgl_display);
    lv_obj_t *sys_layer = lv_disp_get_layer_sys(lvgl_display);
    if (top_layer) lv_obj_invalidate(top_layer);
    if (sys_layer) lv_obj_invalidate(sys_layer);

    lv_refr_now(lvgl_display);

    // Step 8: Restore original flush callback
    lvgl_display->driver->flush_cb = ctx.original_flush_cb;
    s_sd_ctx = NULL;

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

// ==================== Serial (base64) Screenshot ====================
// Streams the framebuffer over UART — no SD card needed. ~18s at 115200 baud, with
// the LVGL lock held throughout so the capture is one consistent frame. Decode with
// scripts/decode_screenshot.py against a monitor log.

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// One row: 320 px x 2 B = 640 B raw -> 856 B base64 (+NUL)
static char b64_line_buf[(LCD_WIDTH * 2 + 2) / 3 * 4 + 4];

typedef struct {
    void (*original_flush_cb)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
    void *original_user_data;
    int rows_emitted;
} sshot_serial_ctx_t;

static void b64_encode_row(const uint8_t *src, int len, char *dst)
{
    int di = 0;
    int i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[di++] = b64_alphabet[(n >> 18) & 63];
        dst[di++] = b64_alphabet[(n >> 12) & 63];
        dst[di++] = b64_alphabet[(n >> 6) & 63];
        dst[di++] = b64_alphabet[n & 63];
    }
    int rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)src[i] << 16;
        dst[di++] = b64_alphabet[(n >> 18) & 63];
        dst[di++] = b64_alphabet[(n >> 12) & 63];
        dst[di++] = '=';
        dst[di++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[di++] = b64_alphabet[(n >> 18) & 63];
        dst[di++] = b64_alphabet[(n >> 12) & 63];
        dst[di++] = b64_alphabet[(n >> 6) & 63];
        dst[di++] = '=';
    }
    dst[di] = '\0';
}

// Active capture context. NEVER carry this in drv->user_data: esp_lvgl_port's SPI
// flush-done INTERRUPT dereferences drv->user_data, so any window where it points
// at our context instead of the port's own is a LoadProhibited panic. One capture
// at a time (ss_cmd serializes).
static sshot_serial_ctx_t *s_serial_ctx = NULL;

static void sshot_serial_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    sshot_serial_ctx_t *ctx = s_serial_ctx;
    if (ctx == NULL) {
        return;  // Cannot happen while installed; fail closed rather than deref
    }
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;

    // The console TX path busy-waits on the UART FIFO, so a chunk of solid
    // printf starves IDLE0 and trips the task watchdog (its dump then lands
    // mid-line inside the capture). One tick of sleep per chunk feeds it.
    vTaskDelay(pdMS_TO_TICKS(10));

    // Chunk header carries the area so the decoder places rows even if LVGL
    // ever renders regions out of order.
    printf("SC:%d:%d:%d:%d\n", (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = (const uint8_t *)((uint16_t *)color_p + y * width);
        b64_encode_row(row, width * 2, b64_line_buf);
        printf("S:%s\n", b64_line_buf);
        ctx->rows_emitted++;
    }

    ctx->original_flush_cb(drv, area, color_p);
}

esp_err_t screenshot_take_serial(void)
{
    if (!lvgl_display) {
        ESP_LOGE(TAG, "LVGL display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    bool locked = false;
    for (int retry = 0; retry < 100 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) {
        ESP_LOGE(TAG, "LVGL lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    // Silence all logging for the capture window so other tasks' log lines
    // cannot interleave into (or split) the base64 stream.
    esp_log_level_t prev_level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("\nSSHOT-BEGIN %dx%d RGB565SWAP B64ROW\n", LCD_WIDTH, LCD_HEIGHT);

    sshot_serial_ctx_t ctx = {
        .original_flush_cb = lvgl_display->driver->flush_cb,
        .original_user_data = lvgl_display->driver->user_data,
        .rows_emitted = 0,
    };
    s_serial_ctx = &ctx;
    lvgl_display->driver->flush_cb = sshot_serial_flush_cb;

    lv_obj_invalidate(lv_scr_act());
    lv_obj_t *top_layer = lv_disp_get_layer_top(lvgl_display);
    lv_obj_t *sys_layer = lv_disp_get_layer_sys(lvgl_display);
    if (top_layer) lv_obj_invalidate(top_layer);
    if (sys_layer) lv_obj_invalidate(sys_layer);

    lv_refr_now(lvgl_display);

    lvgl_display->driver->flush_cb = ctx.original_flush_cb;
    s_serial_ctx = NULL;

    printf("SSHOT-END rows=%d\n", ctx.rows_emitted);
    esp_log_level_set("*", prev_level);
    lvgl_port_unlock();

    if (ctx.rows_emitted < LCD_HEIGHT) {
        ESP_LOGW(TAG, "Serial screenshot emitted %d rows (expected %d)",
                 ctx.rows_emitted, LCD_HEIGHT);
    }
    return ESP_OK;
}

// ==================== UI Hold (remote driving sessions) ====================
// A remote session leaves gaps of many seconds between commands, which the
// inactivity machinery reads as an abandoned screen; the hold flag stands it down.
// It self-expires so a session that ends without "hold off" cannot leave the device
// permanently unable to return home or dim.

#define UI_HOLD_LIFETIME_US  (10LL * 60 * 1000000)  // 10 minutes

volatile bool cygm_ui_hold = false;
static volatile int64_t hold_started_us = 0;

bool cygm_ui_hold_active(void)
{
    if (!cygm_ui_hold) return false;
    if (esp_timer_get_time() - hold_started_us >= UI_HOLD_LIFETIME_US) {
        cygm_ui_hold = false;
        ESP_LOGI(TAG, "UI hold expired after 10 min — released");
        return false;
    }
    return true;
}

static void ui_hold_set(bool on)
{
    if (on) {
        hold_started_us = esp_timer_get_time();
        cygm_ui_hold = true;
        ESP_LOGI(TAG, "UI hold: ON (inactivity return-to-home and dim paused, expires in 10 min)");
    } else {
        cygm_ui_hold = false;
        ESP_LOGI(TAG, "UI hold: OFF");
    }
}

// ==================== Trend Demo (arrow showcase) ====================
// Same shape as the UI hold above, and self-expiring for the same reason: this one
// makes the device show a trend that is not the sensor's, so walking away from the
// terminal must not leave it on.

#define DEMO_TREND_LIFETIME_US  (30LL * 60 * 1000000)  // 30 minutes

volatile bool cygm_demo_trend = false;
static volatile int64_t demo_started_us = 0;

bool cygm_demo_trend_active(void)
{
    if (!cygm_demo_trend) return false;
    if (esp_timer_get_time() - demo_started_us >= DEMO_TREND_LIFETIME_US) {
        cygm_demo_trend = false;
        ESP_LOGI(TAG, "Trend demo expired after 30 min — released");
        return false;
    }
    return true;
}

static void demo_trend_set(bool on)
{
    if (on) {
        demo_started_us = esp_timer_get_time();
        cygm_demo_trend = true;
        ESP_LOGW(TAG, "Trend demo: ON — the arrow is NOT the sensor's trend "
                      "(random every 10s, expires in 30 min)");
    } else {
        cygm_demo_trend = false;
        ESP_LOGI(TAG, "Trend demo: OFF (next reading restores the real trend)");
    }
    // main.c owns the LVGL-side ticker; it starts or stops it to match the flag.
    cygm_demo_trend_sync();
}

// ==================== Status Report ====================
// Printed a line at a time on purpose: the ss_cmd task has a 4KB stack and one wide
// snprintf buffer would eat a meaningful slice of it. Every line is prefixed STATUS:
// so the PC-side tool can pick them out of the log stream.

static void status_report(void)
{
    printf("STATUS: fw=%s\n", CYGM_VERSION_STRING);
    printf("STATUS: uptime_s=%lu\n", (unsigned long)(esp_timer_get_time() / 1000000));

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char ssid[33];
        memcpy(ssid, ap.ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        printf("STATUS: wifi=%s rssi=%d\n", ssid, (int)ap.rssi);
    } else {
        printf("STATUS: wifi=disconnected\n");
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        printf("STATUS: ip=" IPSTR "\n", IP2STR(&ip_info.ip));
    } else {
        printf("STATUS: ip=none\n");
    }

    printf("STATUS: heap_free=%lu heap_largest=%u\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    // lv_mem_monitor walks the LVGL pool's block list, so it must run under the
    // port lock. A wedged LVGL task must never wedge the status report too:
    // short non-blocking retry, then report the skip and move on.
    bool locked = false;
    for (int retry = 0; retry < 20 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (locked) {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        lvgl_port_unlock();
        printf("STATUS: lvgl_free=%u lvgl_biggest=%u lvgl_frag=%u%% lvgl_used=%u%%\n",
               (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
               (unsigned)mon.frag_pct, (unsigned)mon.used_pct);
    } else {
        printf("STATUS: lvgl=unavailable (port lock busy)\n");
    }

    printf("STATUS: glucose_valid=%d value=%d\n", glucose_data_valid ? 1 : 0, current_glucose);
    printf("STATUS: hold=%s\n", cygm_ui_hold_active() ? "on" : "off");
    printf("STATUS: demo=%s\n", cygm_demo_trend_active() ? "on" : "off");
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

    ESP_LOGI(TAG, "Screenshot command listener ready (type 'help' + Enter)");

    // Must hold the longest command line: "swipe 319 239 319 239 12345" (27).
    char cmd_buf[48];
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
                } else if (strcmp(cmd_buf, "sss") == 0) {
                    screenshot_take_serial();
                } else if (strncmp(cmd_buf, "tap ", 4) == 0) {
                    int tx = -1, ty = -1, tms = TAP_DEFAULT_MS;
                    int n = sscanf(cmd_buf + 4, "%d %d %d", &tx, &ty, &tms);
                    if (n >= 2 && tx >= 0 && tx < LCD_WIDTH && ty >= 0 && ty < LCD_HEIGHT &&
                        tms > 0 && tms <= INJECT_MAX_MS) {
                        display_inject_tap((uint16_t)tx, (uint16_t)ty, (uint16_t)tms);
                        ESP_LOGI(TAG, "Injected tap at (%d, %d) for %dms", tx, ty, tms);
                    } else {
                        ESP_LOGW(TAG, "Usage: tap <x 0-319> <y 0-239> [ms]");
                    }
                } else if (strncmp(cmd_buf, "swipe ", 6) == 0) {
                    int x1 = -1, y1 = -1, x2 = -1, y2 = -1, sms = SWIPE_DEFAULT_MS;
                    int n = sscanf(cmd_buf + 6, "%d %d %d %d %d", &x1, &y1, &x2, &y2, &sms);
                    if (n >= 4 &&
                        x1 >= 0 && x1 < LCD_WIDTH && y1 >= 0 && y1 < LCD_HEIGHT &&
                        x2 >= 0 && x2 < LCD_WIDTH && y2 >= 0 && y2 < LCD_HEIGHT &&
                        sms > 0 && sms <= INJECT_MAX_MS) {
                        display_inject_swipe((uint16_t)x1, (uint16_t)y1,
                                             (uint16_t)x2, (uint16_t)y2, (uint16_t)sms);
                        ESP_LOGI(TAG, "Injected swipe (%d, %d) -> (%d, %d) over %dms",
                                 x1, y1, x2, y2, sms);
                    } else {
                        ESP_LOGW(TAG, "Usage: swipe <x1> <y1> <x2> <y2> [ms]");
                    }
                } else if (strcmp(cmd_buf, "status") == 0) {
                    status_report();
                } else if (strcmp(cmd_buf, "reboot") == 0) {
                    ESP_LOGW(TAG, "Reboot requested over serial — restarting now");
                    vTaskDelay(pdMS_TO_TICKS(100));  // Let the ack drain out of the UART
                    esp_restart();
                } else if (strcmp(cmd_buf, "hold on") == 0) {
                    ui_hold_set(true);
                } else if (strcmp(cmd_buf, "hold off") == 0) {
                    ui_hold_set(false);
                } else if (strcmp(cmd_buf, "demo on") == 0) {
                    demo_trend_set(true);
                } else if (strcmp(cmd_buf, "demo off") == 0) {
                    demo_trend_set(false);
                } else if (strcmp(cmd_buf, "ship") == 0) {
                    ESP_LOGW(TAG, "=== SHIP MODE ===");
                    ESP_LOGW(TAG, "This will ERASE all data and power off.");
                    ESP_LOGW(TAG, "Type 'confirm' within 10 seconds to proceed.");

                    char confirm_buf[16];
                    int confirm_pos = 0;
                    bool confirmed = false;
                    int64_t deadline = esp_timer_get_time() + 10000000; // 10 seconds

                    while (esp_timer_get_time() < deadline) {
                        int clen = uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(200));
                        if (clen <= 0) continue;
                        if (byte == '\n' || byte == '\r') {
                            if (confirm_pos > 0) {
                                confirm_buf[confirm_pos] = '\0';
                                if (strcmp(confirm_buf, "confirm") == 0) {
                                    confirmed = true;
                                }
                                break;
                            }
                        } else if (confirm_pos < (int)sizeof(confirm_buf) - 1) {
                            confirm_buf[confirm_pos++] = (char)byte;
                        }
                    }

                    if (!confirmed) {
                        ESP_LOGI(TAG, "Ship mode cancelled.");
                    } else {
                        ESP_LOGW(TAG, "Erasing NVS...");
                        nvs_factory_reset();

                        ESP_LOGW(TAG, "Flushing SD...");
                        sd_logger_flush();

                        ESP_LOGW(TAG, "Stopping WiFi...");
                        esp_wifi_stop();

                        ESP_LOGW(TAG, "Powering off...");
                        if (lcd_panel != NULL) {
                            esp_lcd_panel_disp_on_off(lcd_panel, false);
                        }
                        // Stop LEDs (active-low: idle=1 means off)
                        ledc_stop(LEDC_LOW_SPEED_MODE, LED_RED_CHANNEL, 1);
                        ledc_stop(LEDC_LOW_SPEED_MODE, LED_GREEN_CHANNEL, 1);
                        ledc_stop(LEDC_LOW_SPEED_MODE, LED_BLUE_CHANNEL, 1);
                        ledc_stop(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
                        gpio_reset_pin(LED_RED);
                        gpio_reset_pin(LED_GREEN);
                        gpio_reset_pin(LED_BLUE);
                        gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
                        gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
                        gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
                        gpio_set_level(LED_RED, 1);
                        gpio_set_level(LED_GREEN, 1);
                        gpio_set_level(LED_BLUE, 1);
                        gpio_hold_en(LED_RED);
                        gpio_reset_pin(LCD_BL);
                        gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
                        gpio_set_level(LCD_BL, 0);
                        gpio_hold_en(LCD_BL);
                        // Deep sleep — wake on boot button
                        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
                        ESP_LOGW(TAG, "Device shipped. Goodbye.");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_deep_sleep_start();
                    }
                } else if (strcmp(cmd_buf, "log on") == 0) {
                    sd_serial_capture_set(true);
                    nvs_save_sd_serial_capture(true);
                    ESP_LOGI(TAG, "Serial capture: ON (saved — persists across reboots)");
                } else if (strcmp(cmd_buf, "log off") == 0) {
                    sd_serial_capture_set(false);
                    nvs_save_sd_serial_capture(false);
                    ESP_LOGI(TAG, "Serial capture: OFF (saved — persists across reboots)");
                } else if (strcmp(cmd_buf, "help") == 0) {
                    ESP_LOGI(TAG, "Commands: ss (SD) | sss (serial b64) | tap x y [ms] | "
                                  "swipe x1 y1 x2 y2 [ms] | status | reboot | "
                                  "hold on | hold off | demo on | demo off | "
                                  "log on | log off | ship | help");
                    ESP_LOGI(TAG, "  demo on = cycle the trend arrow through a random "
                                  "direction every 10s (arrow only, expires in 30 min)");
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
    // Always start the command listener — "ship" command works without SD card.
    // Screenshot and log commands gracefully handle missing SD.
    BaseType_t ret = xTaskCreatePinnedToCore(
        screenshot_cmd_task,
        "ss_cmd",
        4096,   // screenshot_take() runs inline: SD mount, fopen, LVGL render need deep stack
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
