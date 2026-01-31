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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst820.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "DISPLAY";

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Initializing LCD backlight");
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);

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

esp_err_t cardkb_init(void) {
    ESP_LOGI(TAG, "Initializing CardKB I2C bus");

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CARDKB_I2C_NUM,
        .sda_io_num = CARDKB_SDA,
        .scl_io_num = CARDKB_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &cardkb_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CardKB I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CARDKB_ADDR,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(cardkb_i2c_bus, &dev_config, &cardkb_dev);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CardKB initialized");
    } else {
        ESP_LOGE(TAG, "CardKB device add failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

uint8_t cardkb_read(void) {
    uint8_t data = 0;
    i2c_master_receive(cardkb_dev, &data, 1, 100);
    return data;
}

esp_err_t lvgl_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL");

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 1,  // Pin LVGL task to Core 1
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Add LCD display
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = lcd_panel,
        .io_handle = lcd_io_handle,
        .buffer_size = LCD_WIDTH * 60,
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

    // Add touch input
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_display,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    ESP_LOGI(TAG, "LVGL initialized successfully");
    return ESP_OK;
}
