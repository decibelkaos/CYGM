/*
 * led.c
 *
 * RGB LED control implementation
 */

#include "led.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "LED";

// Helper function to set individual channel brightness
static void led_set_brightness(ledc_channel_t channel, uint8_t brightness) {
    if (brightness > 127) brightness = 127;
    uint32_t duty = 1023 - (brightness * 4);  // Active-low
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

void led_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LED_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channels[] = {
        {.gpio_num = LED_RED, .channel = LED_RED_CHANNEL, .duty = 1023},
        {.gpio_num = LED_GREEN, .channel = LED_GREEN_CHANNEL, .duty = 1023},
        {.gpio_num = LED_BLUE, .channel = LED_BLUE_CHANNEL, .duty = 1023}
    };

    for (int i = 0; i < 3; i++) {
        channels[i].speed_mode = LEDC_LOW_SPEED_MODE;
        channels[i].timer_sel = LED_TIMER;
        channels[i].hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channels[i]));
    }

    ESP_LOGI(TAG, "LED PWM initialized");
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    led_set_brightness(LED_RED_CHANNEL, r);
    led_set_brightness(LED_GREEN_CHANNEL, g);
    led_set_brightness(LED_BLUE_CHANNEL, b);
}

// LED breathing task removed - feature disabled to save power
// Color-setting function kept for potential future use
void led_set_breathing_color(uint8_t r, uint8_t g, uint8_t b) {
    led_breathe_r = r;
    led_breathe_g = g;
    led_breathe_b = b;
}
