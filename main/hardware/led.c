/*
 * led.c
 *
 * RGB LED control implementation
 */

#include "led.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LED";

// LED animation modes
typedef enum {
    LED_MODE_OFF,
    LED_MODE_STATIC,
    LED_MODE_WIFI_BOOT_BLINK,
    LED_MODE_DEXCOM_POLL_BLINK,
    LED_MODE_SUCCESS_FADE,
    LED_MODE_ERROR_BLINK,
    LED_MODE_ALARM_FLASH
} led_mode_t;

static led_mode_t current_led_mode = LED_MODE_OFF;
static TaskHandle_t led_task_handle = NULL;
static uint8_t static_r = 0, static_g = 0, static_b = 0;

// Helper function to set individual channel brightness
static void led_set_brightness(ledc_channel_t channel, uint8_t brightness) {
    if (brightness > 127) brightness = 127;
    uint32_t duty = 1023 - (brightness * 4);  // Active-low
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

// LED animation task
static void led_task(void *arg) {
    uint32_t tick_count = 0;
    uint8_t fade_brightness = 0;
    bool blink_state = false;

    while (1) {
        switch (current_led_mode) {
            case LED_MODE_OFF:
                led_set_color(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LED_MODE_STATIC:
                led_set_color(static_r, static_g, static_b);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LED_MODE_WIFI_BOOT_BLINK:
                // Blink green every 200ms
                blink_state = !blink_state;
                led_set_color(0, blink_state ? 80 : 0, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;

            case LED_MODE_DEXCOM_POLL_BLINK:
                // 5 double-blinks with decreasing brightness (1.5 seconds total)
                // Brightness: 100%, 85%, 70%, 60%, 50%
                {
                    uint8_t brightness_steps[5] = {127, 108, 89, 76, 64};
                    for (int i = 0; i < 5; i++) {
                        // First blink
                        led_set_color(0, 0, brightness_steps[i]);
                        vTaskDelay(pdMS_TO_TICKS(50));
                        led_set_color(0, 0, 0);
                        vTaskDelay(pdMS_TO_TICKS(50));

                        // Second blink
                        led_set_color(0, 0, brightness_steps[i]);
                        vTaskDelay(pdMS_TO_TICKS(50));
                        led_set_color(0, 0, 0);
                        vTaskDelay(pdMS_TO_TICKS(150));  // Gap before next step
                    }
                    current_led_mode = LED_MODE_OFF;
                }
                break;

            case LED_MODE_SUCCESS_FADE:
                // Steady green for 3 seconds
                led_set_color(0, 80, 0);
                vTaskDelay(pdMS_TO_TICKS(3000));

                // Fade out over 500ms
                for (fade_brightness = 80; fade_brightness > 0; fade_brightness--) {
                    led_set_color(0, fade_brightness, 0);
                    vTaskDelay(1);  // 1 tick per step (fade speed adapts to tick rate)
                }
                led_set_color(0, 0, 0);
                current_led_mode = LED_MODE_OFF;
                break;

            case LED_MODE_ERROR_BLINK:
                // Blink red, 500ms on, 500ms off
                blink_state = !blink_state;
                led_set_color(blink_state ? 80 : 0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case LED_MODE_ALARM_FLASH:
                // Alternating red/blue flash (100ms each color)
                blink_state = !blink_state;
                led_set_color(blink_state ? 127 : 0, 0, blink_state ? 0 : 127);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }

        tick_count++;
    }
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

    // Start LED animation task on Core 0
    xTaskCreatePinnedToCore(led_task, "led_task", 1024, NULL, 5, &led_task_handle, 0);
    ESP_LOGI(TAG, "LED animation task started");
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

void led_start_wifi_boot_blink(void) {
    current_led_mode = LED_MODE_WIFI_BOOT_BLINK;
    ESP_LOGI(TAG, "Started WiFi boot blink (green, 200ms)");
}

void led_stop_wifi_boot_blink(void) {
    if (current_led_mode == LED_MODE_WIFI_BOOT_BLINK) {
        current_led_mode = LED_MODE_OFF;
        ESP_LOGI(TAG, "Stopped WiFi boot blink");
    }
}

void led_blink_blue(void) {
    current_led_mode = LED_MODE_DEXCOM_POLL_BLINK;
}

void led_show_success(void) {
    current_led_mode = LED_MODE_SUCCESS_FADE;
    ESP_LOGI(TAG, "Showing success (green fade)");
}

void led_start_error_blink(void) {
    current_led_mode = LED_MODE_ERROR_BLINK;
    ESP_LOGI(TAG, "Started error blink (red, 500ms)");
}

void led_stop_error_blink(void) {
    if (current_led_mode == LED_MODE_ERROR_BLINK) {
        current_led_mode = LED_MODE_OFF;
        ESP_LOGI(TAG, "Stopped error blink");
    }
}

void led_start_alarm_flash(void) {
    current_led_mode = LED_MODE_ALARM_FLASH;
    ESP_LOGI(TAG, "Started alarm flash (red/blue alternating, 100ms)");
}

void led_stop_alarm_flash(void) {
    if (current_led_mode == LED_MODE_ALARM_FLASH) {
        current_led_mode = LED_MODE_OFF;
        ESP_LOGI(TAG, "Stopped alarm flash");
    }
}

void led_off(void) {
    current_led_mode = LED_MODE_OFF;
}
