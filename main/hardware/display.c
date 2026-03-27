/*
 * display.c
 *
 * Display and input initialization implementation
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

    // Configure LEDC timer for backlight
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,  // 0-255 duty cycle
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = 5000,  // 5kHz PWM frequency
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Configure LEDC channel for backlight
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
    // Initialize backlight with PWM
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
    // Use global lcd_io_handle
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 26000000,
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
        .timer_period_ms = 30  // Increased from 5ms to 30ms (no animations, reduces memory churn)
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Add LCD display
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = lcd_panel,
        .io_handle = lcd_io_handle,
        .buffer_size = LCD_WIDTH * 30,  // Reduced from 60 to 30 lines (1/8 screen) to reduce memory fragmentation (saves ~38KB)
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

    // Add touch input (skip if touch init failed — device works without touch)
    if (touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lvgl_display,
            .handle = touch_handle,
        };
        lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "Touch not available — LVGL running without touch input");
        lvgl_touch_indev = NULL;
    }

    ESP_LOGI(TAG, "LVGL initialized successfully");
    return ESP_OK;
}
