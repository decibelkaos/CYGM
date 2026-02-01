/**
 * CYGM - Full Hardware Verification
 * JC2432W328 ESP32
 * 
 * VERIFIED PINS:
 * - Display: ST7789 SPI (MOSI:13, SCLK:14, CS:15, DC:2, BL:27)
 * - Touch: CST820 I2C (SDA:33, SCL:32, RST:25, INT:21)
 * - Speaker: GPIO 26 (PWM -> 8002A amp -> P4)
 * - RGB LED: GPIO 4,16,17 (active low)
 * - Button: GPIO 0 (BOOT only)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/ledc.h"

static const char *TAG = "CYGM";

// Display
#define LCD_HOST    SPI2_HOST
#define LCD_MOSI    GPIO_NUM_13
#define LCD_SCLK    GPIO_NUM_14
#define LCD_CS      GPIO_NUM_15
#define LCD_DC      GPIO_NUM_2
#define LCD_BL      GPIO_NUM_27
#define LCD_H_RES   320
#define LCD_V_RES   240

// Touch
#define TOUCH_SDA   GPIO_NUM_33
#define TOUCH_SCL   GPIO_NUM_32
#define TOUCH_RST   GPIO_NUM_25
#define TOUCH_ADDR  0x15

// Speaker
#define SPEAKER_PIN GPIO_NUM_26

// RGB LED (active low)
#define LED_R       GPIO_NUM_4
#define LED_G       GPIO_NUM_16
#define LED_B       GPIO_NUM_17

// Button
#define BTN_BOOT    GPIO_NUM_0

// Colors
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0x07E0
#define GREEN   0x001F
#define BLUE    0xF800
#define YELLOW  0x07FF
#define CYAN    0xF81F

static esp_lcd_panel_handle_t panel = NULL;

static void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL<<LED_R)|(1ULL<<LED_G)|(1ULL<<LED_B),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&cfg);
    gpio_set_level(LED_R, 1);
    gpio_set_level(LED_G, 1);
    gpio_set_level(LED_B, 1);
}

static void led_set(int r, int g, int b) {
    gpio_set_level(LED_R, !r);
    gpio_set_level(LED_G, !g);
    gpio_set_level(LED_B, !b);
}

static void speaker_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .gpio_num = SPEAKER_PIN,
        .duty = 0
    };
    ledc_channel_config(&c);
}

static void beep(uint32_t freq, uint32_t ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 127);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void btn_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL<<BTN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&cfg);
}

static void display_init(void) {
    gpio_config_t bl = {.pin_bit_mask=(1ULL<<LCD_BL),.mode=GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(LCD_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI,
        .sclk_io_num = LCD_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * 2
    };
    spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16
    };
    esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);
}

static void fill_screen(uint16_t color) {
    uint16_t *buf = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    for (int i = 0; i < LCD_H_RES; i++) buf[i] = color;
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y+1, buf);
    }
    free(buf);
}

static void draw_rect(int x, int y, int w, int h, uint16_t color) {
    uint16_t *buf = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA);
    for (int i = 0; i < w; i++) buf[i] = color;
    for (int row = y; row < y+h && row < LCD_V_RES; row++) {
        esp_lcd_panel_draw_bitmap(panel, x, row, x+w, row+1, buf);
    }
    free(buf);
}

static void touch_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    gpio_config_t rst = {.pin_bit_mask=(1ULL<<TOUCH_RST),.mode=GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static bool touch_read(int *x, int *y) {
    uint8_t data[6], reg = 0x01;
    if (i2c_master_write_read_device(I2C_NUM_0, TOUCH_ADDR, &reg, 1, data, 6, 100) != ESP_OK)
        return false;
    if (data[1] == 0 || data[1] > 5) return false;
    int rx = ((data[2]&0x0F)<<8)|data[3];
    int ry = ((data[4]&0x0F)<<8)|data[5];
    // Calibration: swap XY, mirror Y
    *x = ry;
    *y = LCD_V_RES - 1 - rx;
    return (*x >= 0 && *x < LCD_H_RES && *y >= 0 && *y < LCD_V_RES);
}

void app_main(void) {
    ESP_LOGI(TAG, "CYGM Hardware Test");

    led_init();
    speaker_init();
    btn_init();
    display_init();
    touch_init();

    // Startup beeps
    beep(1000, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    beep(1500, 100);

    fill_screen(BLACK);

    // Draw touch buttons
    draw_rect(10, 10, 90, 50, RED);     // Red LED
    draw_rect(115, 10, 90, 50, GREEN);  // Green LED  
    draw_rect(220, 10, 90, 50, BLUE);   // Blue LED
    draw_rect(10, 80, 145, 50, WHITE);  // LED Off
    draw_rect(165, 80, 145, 50, YELLOW);// Beep
    draw_rect(10, 150, 300, 80, CYAN);  // Touch area

    ESP_LOGI(TAG, "Touch RED/GREEN/BLUE to set LED");
    ESP_LOGI(TAG, "Touch WHITE to turn LED off");
    ESP_LOGI(TAG, "Touch YELLOW to beep");
    ESP_LOGI(TAG, "Press BOOT button to beep+flash");

    int tx, ty;
    bool last_btn = false;

    while (1) {
        // Button check
        bool btn = !gpio_get_level(BTN_BOOT);
        if (btn && !last_btn) {
            ESP_LOGI(TAG, "BOOT pressed");
            led_set(1, 1, 1);
            beep(2000, 100);
            led_set(0, 0, 0);
        }
        last_btn = btn;

        // Touch check
        if (touch_read(&tx, &ty)) {
            ESP_LOGI(TAG, "Touch: %d,%d", tx, ty);
            
            if (ty < 70) {
                if (tx < 110) { led_set(1,0,0); ESP_LOGI(TAG, "LED RED"); }
                else if (tx < 215) { led_set(0,1,0); ESP_LOGI(TAG, "LED GREEN"); }
                else { led_set(0,0,1); ESP_LOGI(TAG, "LED BLUE"); }
            }
            else if (ty < 140) {
                if (tx < 160) { led_set(0,0,0); ESP_LOGI(TAG, "LED OFF"); }
                else { beep(1000, 200); ESP_LOGI(TAG, "BEEP"); }
            }

            // Draw touch marker in cyan area
            if (ty >= 150) {
                draw_rect(tx-5, ty-5, 10, 10, WHITE);
            }

            while (touch_read(&tx, &ty)) vTaskDelay(pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
