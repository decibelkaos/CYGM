/*
 * display.c
 *
 * LCD, backlight, capacitive touch and LVGL initialization.
 */

#include "display.h"
#include "shared_state.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst820.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "DISPLAY";

#define LCD_BL_LEDC_CHANNEL   LEDC_CHANNEL_4
#define LCD_BL_LEDC_TIMER     LEDC_TIMER_2

esp_err_t display_backlight_init(void) {
    ESP_LOGI(TAG, "Initializing LCD backlight PWM");

    // Release GPIO hold from deep sleep (gpio_hold_en keeps pin low across wakeup)
    gpio_hold_dis(LCD_BL);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,  // 0-255 duty cycle
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = 5000,  // 5kHz PWM frequency
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 204,  // Start at 80% (204/255)
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "LCD backlight PWM initialized");
    return ESP_OK;
}

esp_err_t display_set_brightness(uint8_t brightness_percent) {
    // Enforce minimum 1% and maximum 100%
    if (brightness_percent < 1) {
        brightness_percent = 1;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }

    // Convert percentage to 8-bit duty cycle (3-255, where 3 = 1%)
    uint32_t duty = (brightness_percent * 255) / 100;

    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty);
    if (ret == ESP_OK) {
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Brightness set to %d%% (duty: %ld/255)", brightness_percent, duty);
        }
    }

    return ret;
}

esp_err_t display_init(void) {
    ESP_ERROR_CHECK(display_backlight_init());

    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 80 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Initializing LCD panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40000000,  // If this unit shows tearing/artifacts, step down: 32MHz, then 26MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &lcd_io_handle));

    ESP_LOGI(TAG, "Initializing LCD panel (ST7789)");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  // Changed from BGR to RGB for correct colors
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcd_io_handle, &panel_config, &lcd_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, true));  // Landscape rotation
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, false, true));  // Mirror Y for landscape
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    ESP_LOGI(TAG, "LCD initialized successfully");
    return ESP_OK;
}

// CST820 gesture status register. Read-only; holds its code for as long as the
// controller reports the gesture, so latching must be edge-triggered.
#define CST820_GESTURE_REG    0x01

static esp_err_t (*cst820_base_read_data)(esp_lcd_touch_handle_t tp) = NULL;
static portMUX_TYPE gesture_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t gesture_latched = CST820_GESTURE_NONE;
static uint8_t gesture_prev_raw = CST820_GESTURE_NONE;

// Synthetic touch state (serial "tap"/"swipe" dev commands) — consumed by the
// poll wrapper below, produced by display_inject_tap()/display_inject_swipe()
// from any task. A tap is a press whose start and end points are equal.
static portMUX_TYPE synth_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static int      synth_polls_left  = 0;   // press cycles still to emit
static int      synth_polls_total = 0;   // press cycles as issued, for interpolation
static uint16_t synth_raw_x0 = 0, synth_raw_y0 = 0;
static uint16_t synth_raw_x1 = 0, synth_raw_y1 = 0;

// Wall-clock cost of one indev read. LVGL's tick only advances in whole lvgl_port
// timer periods, so the indev read timer cannot fire until the first tick multiple
// at or past its own period: 30ms of period on a 20ms tick lands on 40ms.
#define SYNTH_LVGL_TICK_MS  20   /* keep in step with lvgl_cfg.timer_period_ms */
#define SYNTH_POLL_MS       (((LV_INDEV_DEF_READ_PERIOD + SYNTH_LVGL_TICK_MS - 1) \
                              / SYNTH_LVGL_TICK_MS) * SYNTH_LVGL_TICK_MS)
#define SYNTH_POLLS_MIN     2
#define SYNTH_POLLS_MAX     400  /* ~16s ceiling */

// Chained onto the driver's poll so the gesture register is sampled in the same
// cycle as the coordinates. Stays in polling mode: INT-mode gesture reports are
// unreliable on this controller family.
static esp_err_t cst820_read_data_with_gesture(esp_lcd_touch_handle_t tp) {
    if (cst820_base_read_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = cst820_base_read_data(tp);

    uint8_t raw = CST820_GESTURE_NONE;
    if (esp_lcd_panel_io_rx_param(tp->io, CST820_GESTURE_REG, &raw, 1) != ESP_OK) {
        return ret;
    }

    // Only these two codes are accepted; slide and single-tap codes overlap what
    // LVGL already handles from the coordinate stream.
    if (raw != gesture_prev_raw) {
        gesture_prev_raw = raw;
        if (raw == CST820_GESTURE_DOUBLE_TAP || raw == CST820_GESTURE_LONG_PRESS) {
            portENTER_CRITICAL(&gesture_mux);
            gesture_latched = raw;
            portEXIT_CRITICAL(&gesture_mux);
        }
    }

    // Synthetic touch: overwrite the stored (raw, pre-transform) point for the
    // remaining polls, walking start -> end so a swipe presents as a moving pressed
    // point. Once exhausted the real read reports no-touch and LVGL sees a release.
    portENTER_CRITICAL(&synth_tap_mux);
    int polls = synth_polls_left;
    int total = synth_polls_total;
    uint16_t rx0 = synth_raw_x0, ry0 = synth_raw_y0;
    uint16_t rx1 = synth_raw_x1, ry1 = synth_raw_y1;
    if (polls > 0) synth_polls_left = polls - 1;
    portEXIT_CRITICAL(&synth_tap_mux);
    if (polls > 0) {
        int step  = total - polls;                  // 0 on the first poll of the press
        int denom = (total > 1) ? (total - 1) : 1;  // last poll lands exactly on the end point
        uint16_t cx = (uint16_t)((int)rx0 + (((int)rx1 - (int)rx0) * step) / denom);
        uint16_t cy = (uint16_t)((int)ry0 + (((int)ry1 - (int)ry0) * step) / denom);
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 1;
        tp->data.coords[0].x = cx;
        tp->data.coords[0].y = cy;
        tp->data.coords[0].strength = 50;
        portEXIT_CRITICAL(&tp->data.lock);
    }

    return ret;
}

uint8_t cst820_take_gesture(void) {
    portENTER_CRITICAL(&gesture_mux);
    uint8_t gesture = gesture_latched;
    gesture_latched = CST820_GESTURE_NONE;
    portEXIT_CRITICAL(&gesture_mux);
    return gesture;
}

// ---- Synthetic touch injection (serial "tap"/"swipe" dev commands) ----
// Injected in RAW panel coordinates inside the poll wrapper above, so it rides the
// same transform path a real finger takes. Inverse of the landscape transform:
//   screen_x = y_max - raw_y   ->   raw_y = 320 - screen_x
//   screen_y = raw_x           ->   raw_x = screen_y
static void synth_press(uint16_t sx0, uint16_t sy0, uint16_t sx1, uint16_t sy1,
                        uint16_t duration_ms) {
    if (sx0 > 319) sx0 = 319;
    if (sy0 > 239) sy0 = 239;
    if (sx1 > 319) sx1 = 319;
    if (sy1 > 239) sy1 = 239;

    // Round the poll count UP: a hold-to-confirm flow that wants 1500ms must not
    // be handed 1480 and miss its threshold by half a poll.
    int polls = (duration_ms + SYNTH_POLL_MS - 1) / SYNTH_POLL_MS;
    if (polls < SYNTH_POLLS_MIN) polls = SYNTH_POLLS_MIN;
    if (polls > SYNTH_POLLS_MAX) polls = SYNTH_POLLS_MAX;

    portENTER_CRITICAL(&synth_tap_mux);
    synth_raw_x0 = sy0;
    synth_raw_y0 = (uint16_t)(320 - sx0);
    synth_raw_x1 = sy1;
    synth_raw_y1 = (uint16_t)(320 - sx1);
    synth_polls_total = polls;
    synth_polls_left  = polls;
    portEXIT_CRITICAL(&synth_tap_mux);
}

void display_inject_tap(uint16_t screen_x, uint16_t screen_y, uint16_t duration_ms) {
    synth_press(screen_x, screen_y, screen_x, screen_y, duration_ms);
}

// LVGL only latches a gesture when a press moves at least 3px between consecutive
// polls AND the run accumulates more than 50px; a slower poll resets the
// accumulator. ~150-250ms across >=60px satisfies both, while stretching the same
// distance over a second drops the per-poll step under the floor and fires nothing.
void display_inject_swipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t duration_ms) {
    synth_press(x1, y1, x2, y2, duration_ms);
}

esp_err_t touch_init_core1(void) {
    ESP_LOGI(TAG, "Initializing touch I2C bus on Core 1");

    i2c_master_bus_config_t touch_bus_config = {
        .i2c_port = TOUCH_I2C_NUM,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // GPIO 32/33 don't have internal pullups - use external
    };

    esp_err_t ret = i2c_new_master_bus(&touch_bus_config, &touch_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Initializing CST820 touch controller (polling mode)");

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = 240,  // Native touch panel width (before rotation)
        .y_max = 320,  // Native touch panel height (before rotation)
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,  // GPIO_NUM_NC for polling mode
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,      // Rotate to landscape
            .mirror_x = 0,     // From demo code
            .mirror_y = 1,     // From demo code
        },
    };

    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    touch_io_config.scl_speed_hz = 400000;

    ret = esp_lcd_new_panel_io_i2c_v2(touch_i2c_bus, &touch_io_config, &touch_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch panel IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_touch_new_i2c_cst820(touch_io_handle, &touch_cfg, &touch_handle);
    if (ret == ESP_OK) {
        cst820_base_read_data = touch_handle->read_data;
        touch_handle->read_data = cst820_read_data_with_gesture;
        ESP_LOGI(TAG, "Touch initialized successfully (Core 1, polling mode)");
    } else {
        ESP_LOGE(TAG, "Touch init failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t lvgl_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL");

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 1,  // Pin LVGL task to Core 1
        .task_max_sleep_ms = 500,
        // How often lv_timer_handler runs, which bounds the latency of LVGL's own
        // refresh/indev timers (CONFIG_LV_DISP_DEF_REFR_PERIOD / _INDEV_DEF_READ_PERIOD).
        .timer_period_ms = 20
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = lcd_panel,
        .io_handle = lcd_io_handle,
        .buffer_size = LCD_WIDTH * 10,  // 10 lines minimum — saves 25.6 KB vs 30 lines. No animations, so more render passes are fine.
        .double_buffer = true,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,    // Landscape rotation
            .mirror_x = false,
            .mirror_y = true,   // Mirror Y for landscape
        },
        .flags = {
            .buff_dma = true,
        }
    };
    lvgl_display = lvgl_port_add_disp(&disp_cfg);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_display,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    ESP_LOGI(TAG, "LVGL initialized successfully");
    return ESP_OK;
}
