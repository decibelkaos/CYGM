/*
 * led.c
 *
 * RGB LED control: subtle confirmations, unmistakable alarms.
 */

#include "led.h"
#include "main.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "LED";

// LED animation modes
typedef enum {
    LED_MODE_OFF,
    LED_MODE_BRIEF_PULSE,      // Single color flash ~150ms, auto-off
    LED_MODE_SUCCESS_FADE,     // Green: fade on → hold 2s → fade off, auto-off
    LED_MODE_ERROR_PULSE,      // Red: smooth 200ms fade on/off, continuous
    LED_MODE_WIFI_BLINK,       // Blue: 1s on, 1s off, continuous
    LED_MODE_ALARM_FLASH       // Red/blue: 500ms each, continuous
} led_mode_t;

static volatile led_mode_t current_led_mode = LED_MODE_OFF;
static TaskHandle_t led_task_handle = NULL;

// Fade parameters
#define FADE_STEPS     10
#define FADE_STEP_MS   20    // 10 steps × 20ms = 200ms fade
#define MAX_BRIGHTNESS 80

// Optional glucose-state mode: a slow red pulse while glucose is urgently out of
// range and no other mode is claiming the LED. Off unless the NVS flag says
// otherwise; there is no settings UI for it yet.
#define LED_GLUCOSE_NVS_NAMESPACE  "cygm"
#define LED_GLUCOSE_NVS_KEY        "led_glucose"
#define GLUCOSE_PULSE_PEAK     40    // Dim enough to live with overnight
#define GLUCOSE_PULSE_STEP_MS  60    // 10 steps up + 10 down = ~1.2s pulse
#define GLUCOSE_PULSE_GAP_MS  100
#define GLUCOSE_PULSE_GAP_STEPS 30   // 3s dark between pulses

static bool led_glucose_mode = false;

// Helper function to set individual channel brightness
static void led_set_brightness(ledc_channel_t channel, uint8_t brightness) {
    if (brightness > 127) brightness = 127;
    uint32_t duty = 1023 - (brightness * 4);  // Active-low
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

// True when glucose is urgently out of range: either the alarm engine is
// holding a low/high alarm state, or the reading is at or under the
// non-disableable urgent-low floor even with that tier switched off.
static bool glucose_urgent_out_of_range(void) {
    if (!led_glucose_mode || !glucose_data_valid) return false;
    if (current_alarm_state == ALARM_STATE_LOW_ALARM ||
        current_alarm_state == ALARM_STATE_HIGH_ALARM) return true;
    return current_glucose > 0 && current_glucose <= cygm_urgent_low_threshold();
}

// Slow red pulse, LEDC duty steps only — no LVGL, no extra task. Runs inside
// the OFF branch so any real mode (alarm flash, WiFi blink, error) still wins.
static void led_glucose_pulse(void) {
    for (int i = 1; i <= FADE_STEPS && current_led_mode == LED_MODE_OFF; i++) {
        led_set_color((uint8_t)((GLUCOSE_PULSE_PEAK * i) / FADE_STEPS), 0, 0);
        vTaskDelay(pdMS_TO_TICKS(GLUCOSE_PULSE_STEP_MS));
    }
    for (int i = FADE_STEPS - 1; i >= 0 && current_led_mode == LED_MODE_OFF; i--) {
        led_set_color((uint8_t)((GLUCOSE_PULSE_PEAK * i) / FADE_STEPS), 0, 0);
        vTaskDelay(pdMS_TO_TICKS(GLUCOSE_PULSE_STEP_MS));
    }
    led_set_color(0, 0, 0);

    // Dark gap in chunks so a mode change is picked up promptly
    for (int i = 0; i < GLUCOSE_PULSE_GAP_STEPS && current_led_mode == LED_MODE_OFF; i++) {
        vTaskDelay(pdMS_TO_TICKS(GLUCOSE_PULSE_GAP_MS));
    }
}

// LED animation task
static void led_task(void *arg) {
    while (1) {
        switch (current_led_mode) {
            case LED_MODE_OFF:
                if (glucose_urgent_out_of_range()) {
                    led_glucose_pulse();
                } else {
                    led_set_color(0, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                break;

            case LED_MODE_BRIEF_PULSE:
                // Single blue flash ~150ms, then auto-off
                led_set_color(0, 0, MAX_BRIGHTNESS);
                vTaskDelay(pdMS_TO_TICKS(150));
                led_set_color(0, 0, 0);
                current_led_mode = LED_MODE_OFF;
                break;

            case LED_MODE_SUCCESS_FADE:
                // Fade green on (~200ms)
                for (int i = 1; i <= FADE_STEPS && current_led_mode == LED_MODE_SUCCESS_FADE; i++) {
                    uint8_t b = (uint8_t)((MAX_BRIGHTNESS * i) / FADE_STEPS);
                    led_set_color(0, b, 0);
                    vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
                }
                // Hold steady green for 2 seconds
                if (current_led_mode == LED_MODE_SUCCESS_FADE) {
                    led_set_color(0, MAX_BRIGHTNESS, 0);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                // Fade green off (~300ms, slightly slower for smooth tail)
                for (int i = FADE_STEPS - 1; i >= 0 && current_led_mode == LED_MODE_SUCCESS_FADE; i--) {
                    uint8_t b = (uint8_t)((MAX_BRIGHTNESS * i) / FADE_STEPS);
                    led_set_color(0, b, 0);
                    vTaskDelay(pdMS_TO_TICKS(30));  // 10 steps × 30ms = 300ms
                }
                if (current_led_mode == LED_MODE_SUCCESS_FADE) {
                    led_set_color(0, 0, 0);
                    current_led_mode = LED_MODE_OFF;
                }
                break;

            case LED_MODE_ERROR_PULSE:
                // Smooth red fade on (200ms)
                for (int i = 1; i <= FADE_STEPS && current_led_mode == LED_MODE_ERROR_PULSE; i++) {
                    uint8_t b = (uint8_t)((MAX_BRIGHTNESS * i) / FADE_STEPS);
                    led_set_color(b, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
                }
                // Smooth red fade off (200ms)
                for (int i = FADE_STEPS - 1; i >= 0 && current_led_mode == LED_MODE_ERROR_PULSE; i--) {
                    uint8_t b = (uint8_t)((MAX_BRIGHTNESS * i) / FADE_STEPS);
                    led_set_color(b, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
                }
                // Brief pause at zero before next pulse
                if (current_led_mode == LED_MODE_ERROR_PULSE) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                break;

            case LED_MODE_WIFI_BLINK:
                // Blue 1s on, 1s off
                led_set_color(0, 0, MAX_BRIGHTNESS);
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (current_led_mode == LED_MODE_WIFI_BLINK) {
                    led_set_color(0, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                break;

            case LED_MODE_ALARM_FLASH:
                // Red 500ms, then blue 500ms
                led_set_color(127, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                if (current_led_mode == LED_MODE_ALARM_FLASH) {
                    led_set_color(0, 0, 127);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                break;
        }
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

    // One NVS read for the optional glucose-state mode (default off)
    nvs_handle_t nvs;
    if (nvs_open(LED_GLUCOSE_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(nvs, LED_GLUCOSE_NVS_KEY, &enabled) == ESP_OK) {
            led_glucose_mode = (enabled != 0);
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "LED glucose-state mode: %s", led_glucose_mode ? "on" : "off");

    xTaskCreatePinnedToCore(led_task, "led_task", 1024, NULL, 5, &led_task_handle, 0);
    ESP_LOGI(TAG, "LED animation task started");
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    led_set_brightness(LED_RED_CHANNEL, r);
    led_set_brightness(LED_GREEN_CHANNEL, g);
    led_set_brightness(LED_BLUE_CHANNEL, b);
}

void led_start_wifi_boot_blink(void) {
    current_led_mode = LED_MODE_WIFI_BLINK;
    ESP_LOGI(TAG, "Started WiFi blink (blue, 1s)");
}

void led_stop_wifi_boot_blink(void) {
    if (current_led_mode == LED_MODE_WIFI_BLINK) {
        current_led_mode = LED_MODE_OFF;
        ESP_LOGI(TAG, "Stopped WiFi blink");
    }
}

void led_blink_blue(void) {
    current_led_mode = LED_MODE_BRIEF_PULSE;
}

void led_show_success(void) {
    current_led_mode = LED_MODE_SUCCESS_FADE;
    ESP_LOGI(TAG, "Showing success (green fade)");
}

void led_start_error_blink(void) {
    current_led_mode = LED_MODE_ERROR_PULSE;
    ESP_LOGI(TAG, "Started error pulse (red fade)");
}

void led_stop_error_blink(void) {
    if (current_led_mode == LED_MODE_ERROR_PULSE) {
        current_led_mode = LED_MODE_OFF;
        ESP_LOGI(TAG, "Stopped error pulse");
    }
}

void led_start_alarm_flash(void) {
    current_led_mode = LED_MODE_ALARM_FLASH;
    ESP_LOGI(TAG, "Started alarm flash (red/blue, 500ms)");
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
