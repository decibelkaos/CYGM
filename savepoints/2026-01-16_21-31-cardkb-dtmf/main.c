#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "CARDKB_DTMF";

#define LCD_MOSI    13
#define LCD_SCLK    14
#define LCD_CS      15
#define LCD_DC      2
#define LCD_BL      27
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

#define CARDKB_SDA      21
#define CARDKB_SCL      22
#define CARDKB_ADDR     0x5F
#define I2C_PORT        I2C_NUM_1

#define SPEAKER_PIN     26
#define LED_RED         4
#define LED_GREEN       16
#define LED_BLUE        17

static esp_lcd_panel_handle_t panel_handle = NULL;

typedef struct {
    uint16_t freq_low;
    uint16_t freq_high;
    uint16_t color;
} KeyTone;

static KeyTone get_key_tone(uint8_t key) {
    KeyTone tone = {800, 1400, 0x7BEF};
    switch (key) {
        case '1': tone = (KeyTone){697, 1209, 0xF800}; break;
        case '2': tone = (KeyTone){697, 1336, 0xF820}; break;
        case '3': tone = (KeyTone){697, 1477, 0xFE00}; break;
        case '4': tone = (KeyTone){770, 1209, 0x07E0}; break;
        case '5': tone = (KeyTone){770, 1336, 0x07FF}; break;
        case '6': tone = (KeyTone){770, 1477, 0x001F}; break;
        case '7': tone = (KeyTone){852, 1209, 0x780F}; break;
        case '8': tone = (KeyTone){852, 1336, 0xF81F}; break;
        case '9': tone = (KeyTone){852, 1477, 0xFFE0}; break;
        case '0': tone = (KeyTone){941, 1336, 0xFFFF}; break;
        case 'q': case 'Q': tone = (KeyTone){600, 1100, 0xF800}; break;
        case 'w': case 'W': tone = (KeyTone){620, 1150, 0xFA00}; break;
        case 'e': case 'E': tone = (KeyTone){640, 1200, 0xFC00}; break;
        case 'r': case 'R': tone = (KeyTone){660, 1250, 0xFE00}; break;
        case 't': case 'T': tone = (KeyTone){680, 1300, 0xFFE0}; break;
        case 'y': case 'Y': tone = (KeyTone){700, 1350, 0x87E0}; break;
        case 'u': case 'U': tone = (KeyTone){720, 1400, 0x07E0}; break;
        case 'i': case 'I': tone = (KeyTone){740, 1450, 0x07F0}; break;
        case 'o': case 'O': tone = (KeyTone){760, 1500, 0x07FF}; break;
        case 'p': case 'P': tone = (KeyTone){780, 1550, 0x041F}; break;
        case 'a': case 'A': tone = (KeyTone){800, 1200, 0x001F}; break;
        case 's': case 'S': tone = (KeyTone){820, 1250, 0x401F}; break;
        case 'd': case 'D': tone = (KeyTone){840, 1300, 0x801F}; break;
        case 'f': case 'F': tone = (KeyTone){860, 1350, 0xC01F}; break;
        case 'g': case 'G': tone = (KeyTone){880, 1400, 0xF81F}; break;
        case 'h': case 'H': tone = (KeyTone){900, 1450, 0xF817}; break;
        case 'j': case 'J': tone = (KeyTone){920, 1500, 0xF80F}; break;
        case 'k': case 'K': tone = (KeyTone){940, 1550, 0xF807}; break;
        case 'l': case 'L': tone = (KeyTone){960, 1600, 0xF803}; break;
        case 'z': case 'Z': tone = (KeyTone){500, 1000, 0x780F}; break;
        case 'x': case 'X': tone = (KeyTone){520, 1050, 0x8810}; break;
        case 'c': case 'C': tone = (KeyTone){540, 1100, 0x9812}; break;
        case 'v': case 'V': tone = (KeyTone){560, 1150, 0xA814}; break;
        case 'b': case 'B': tone = (KeyTone){580, 1200, 0xB816}; break;
        case 'n': case 'N': tone = (KeyTone){600, 1250, 0xC818}; break;
        case 'm': case 'M': tone = (KeyTone){620, 1300, 0xD81A}; break;
        case ' ':  tone = (KeyTone){941, 1209, 0x8410}; break;
        case 0x0D: tone = (KeyTone){941, 1633, 0x07E0}; break;
        case 0x08: tone = (KeyTone){400, 800, 0xF800}; break;
        case 0x1B: tone = (KeyTone){300, 600, 0xFFFF}; break;
        case '.':  tone = (KeyTone){1000, 1800, 0xFBE0}; break;
        case ',':  tone = (KeyTone){1050, 1850, 0xFBE4}; break;
        case '?':  tone = (KeyTone){1150, 1950, 0x07FF}; break;
        case '!':  tone = (KeyTone){1100, 1900, 0xF800}; break;
        case '-':  tone = (KeyTone){800, 1600, 0x8410}; break;
        case '+':  tone = (KeyTone){850, 1700, 0xC618}; break;
        case '*':  tone = (KeyTone){941, 1477, 0xFFFF}; break;
        case '/':  tone = (KeyTone){750, 1500, 0xAD55}; break;
        case '@':  tone = (KeyTone){1200, 2000, 0x001F}; break;
        case '#':  tone = (KeyTone){1250, 2050, 0x07E0}; break;
    }
    return tone;
}


static void play_dual_tone(uint16_t freq1, uint16_t freq2, uint32_t duration_ms) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq1,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);
    
    ledc_channel_config_t channel = {
        .gpio_num = SPEAKER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 128,
        .hpoint = 0
    };
    ledc_channel_config(&channel);
    
    uint32_t start = xTaskGetTickCount();
    bool use_freq1 = true;
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(duration_ms)) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, use_freq1 ? freq1 : freq2);
        use_freq1 = !use_freq1;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static esp_err_t cardkb_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CARDKB_SDA,
        .scl_io_num = CARDKB_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret != ESP_OK) return ret;
    return i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static uint8_t cardkb_read(void) {
    uint8_t data = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CARDKB_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return data;
}

static void display_init(void) {
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);
    
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 80 * 2,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle);
    
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);
    esp_lcd_panel_disp_on_off(panel_handle, true);
}

static void fill_screen(uint16_t color) {
    uint16_t *buf = heap_caps_malloc(LCD_WIDTH * 10 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) return;
    for (int i = 0; i < LCD_WIDTH * 10; i++) buf[i] = color;
    for (int y = 0; y < LCD_HEIGHT; y += 10) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_WIDTH, y + 10, buf);
    }
    free(buf);
}


// Simple 8x8 font - uppercase letters and numbers
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // !
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // "
    {0x24,0x7E,0x24,0x24,0x7E,0x24,0x00,0x00}, // #
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // $
    {0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00}, // %
    {0x30,0x48,0x30,0x56,0x88,0x88,0x76,0x00}, // &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // (
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // )
    {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00}, // *
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ,
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // .
    {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00}, // /
    {0x3C,0x46,0x4A,0x52,0x62,0x62,0x3C,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 1
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // 2
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 3
    {0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00}, // 4
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 5
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 6
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // 7
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 8
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, // 9
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // ;
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // <
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // =
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // >
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, // ?
    {0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00}, // @
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // A
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // B
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // C
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // D
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // E
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // F
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, // G
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // H
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // I
    {0x3E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, // J
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // L
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // M
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // N
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // O
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // P
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // Q
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // R
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // S
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // U
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // X
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // Y
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // Z
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // [
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, // backslash
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ]
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, // _
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // `
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // a
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // b
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // c
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // d
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // e
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // f
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, // g
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // h
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // i
    {0x3E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, // j
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // k
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // l
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // m
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // n
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // o
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // p
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // q
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // r
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // s
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // t
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // u
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // v
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // w
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // x
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // y
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // z
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // {
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // |
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // }
    {0x32,0x4C,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};


static void draw_large_char(uint8_t ch, uint16_t fg_color, uint16_t bg_color) {
    int idx = 0;
    if (ch >= ' ' && ch <= '~') idx = ch - ' ';
    
    const uint8_t *glyph = font8x8[idx];
    int scale = 12;
    int char_w = 8 * scale;
    int char_h = 8 * scale;
    int start_x = (LCD_WIDTH - char_w) / 2;
    int start_y = (LCD_HEIGHT - char_h) / 2;
    
    uint16_t *row_buf = heap_caps_malloc(char_w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!row_buf) return;
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int sy = 0; sy < scale; sy++) {
            for (int col = 0; col < 8; col++) {
                uint16_t color = (bits & (0x80 >> col)) ? fg_color : bg_color;
                for (int sx = 0; sx < scale; sx++) {
                    row_buf[col * scale + sx] = color;
                }
            }
            esp_lcd_panel_draw_bitmap(panel_handle, start_x, start_y + row * scale + sy,
                                       start_x + char_w, start_y + row * scale + sy + 1, row_buf);
        }
    }
    free(row_buf);
}

static void draw_text(const char *text, uint16_t fg, uint16_t bg) {
    fill_screen(bg);
    int len = strlen(text);
    int scale = 8;
    int char_w = 8 * scale;
    int total_w = len * char_w;
    int start_x = (LCD_WIDTH - total_w) / 2;
    int start_y = (LCD_HEIGHT - 8 * scale) / 2;
    
    uint16_t *row_buf = heap_caps_malloc(char_w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!row_buf) return;
    
    for (int c = 0; c < len; c++) {
        int idx = 0;
        if (text[c] >= ' ' && text[c] <= '~') idx = text[c] - ' ';
        const uint8_t *glyph = font8x8[idx];
        
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int sy = 0; sy < scale; sy++) {
                for (int col = 0; col < 8; col++) {
                    uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
                    for (int sx = 0; sx < scale; sx++) {
                        row_buf[col * scale + sx] = color;
                    }
                }
                esp_lcd_panel_draw_bitmap(panel_handle, 
                    start_x + c * char_w, start_y + row * scale + sy,
                    start_x + c * char_w + char_w, start_y + row * scale + sy + 1, row_buf);
            }
        }
    }
    free(row_buf);
}

void app_main(void) {
    ESP_LOGI(TAG, "CardKB DTMF Test - SDA=%d SCL=%d", CARDKB_SDA, CARDKB_SCL);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_RED) | (1ULL << LED_GREEN) | (1ULL << LED_BLUE),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_RED, 1);
    gpio_set_level(LED_GREEN, 1);
    gpio_set_level(LED_BLUE, 1);
    
    display_init();
    fill_screen(0x0000);
    
    esp_err_t ret = cardkb_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CardKB init failed");
        return;
    }
    
    play_dual_tone(697, 1209, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    play_dual_tone(770, 1336, 100);
    
    gpio_set_level(LED_GREEN, 0);
    ESP_LOGI(TAG, "Ready!");
    
    while (1) {
        uint8_t key = cardkb_read();
        
        if (key != 0) {
            KeyTone tone = get_key_tone(key);
            
            ESP_LOGI(TAG, "Key: 0x%02X '%c' -> %dHz+%dHz color=0x%04X", 
                     key, (key >= 32 && key < 127) ? key : '?',
                     tone.freq_low, tone.freq_high, tone.color);
            
            uint16_t text_color = (tone.color == 0xFFFF || tone.color == 0xFFE0 || 
                                   tone.color == 0x07E0 || tone.color == 0x07FF) ? 0x0000 : 0xFFFF;
            
            if (key == 0x0D) {
                draw_text("ENT", text_color, tone.color);
            } else if (key == 0x08) {
                draw_text("DEL", text_color, tone.color);
            } else if (key == 0x1B) {
                draw_text("ESC", text_color, tone.color);
            } else if (key == ' ') {
                draw_text("SPC", text_color, tone.color);
            } else if (key >= 32 && key < 127) {
                fill_screen(tone.color);
                draw_large_char(key, text_color, tone.color);
            }
            
            play_dual_tone(tone.freq_low, tone.freq_high, 80);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
