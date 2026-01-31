/*
 * buzzer.c
 *
 * Buzzer/speaker control implementation
 */

#include "buzzer.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUZZER";

void buzzer_init(void) {
    // Initialize LEDC timer for buzzer
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 1000,  // Will be changed when playing tones
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    // Initialize LEDC channel for buzzer
    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,  // Start silent
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    ESP_LOGI(TAG, "Buzzer PWM initialized (GPIO %d)", BUZZER_GPIO);
}

void buzzer_tone(uint32_t frequency, uint8_t volume) {
    if (frequency == 0 || volume == 0) {
        // Silence - set duty to 0
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
        return;
    }

    // Clamp volume to 0-100
    if (volume > 100) volume = 100;

    // Set frequency
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency);

    // Set duty cycle based on volume (0-100% -> 0-1023 duty)
    // Use 50% duty cycle for clearest tone, scaled by volume
    uint32_t duty = (512 * volume) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

void buzzer_stop(void) {
    buzzer_tone(0, 0);
}

// Play boot-up sequence: 5 ascending beeps, 5 descending beeps
void play_boot_sound(void) {
    const uint8_t volume = 50;  // Medium volume
    const int beep_duration = 80;  // Short beeps
    const int pause_duration = 40;

    // Ascending beeps (C5, D5, E5, F5, G5)
    const uint32_t ascending[] = {523, 587, 659, 698, 784};
    for (int i = 0; i < 5; i++) {
        buzzer_tone(ascending[i], volume);
        vTaskDelay(pdMS_TO_TICKS(beep_duration));
        buzzer_tone(0, 0);  // Silence between beeps
        vTaskDelay(pdMS_TO_TICKS(pause_duration));
    }

    // Short pause between sequences
    vTaskDelay(pdMS_TO_TICKS(100));

    // Descending beeps (G5, F5, E5, D5, C5)
    for (int i = 4; i >= 0; i--) {
        buzzer_tone(ascending[i], volume);
        vTaskDelay(pdMS_TO_TICKS(beep_duration));
        buzzer_tone(0, 0);  // Silence between beeps
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(pause_duration));
        }
    }

    buzzer_tone(0, 0);  // Ensure buzzer is off
}

// Play success sound when Dexcom connects and first glucose reading arrives
void play_success_sound(void) {
    const uint8_t volume = 60;  // Medium-high volume

    // Happy ascending arpeggio: C5, E5, G5, C6
    buzzer_tone(523, volume);  // C5
    vTaskDelay(pdMS_TO_TICKS(100));
    buzzer_tone(659, volume);  // E5
    vTaskDelay(pdMS_TO_TICKS(100));
    buzzer_tone(784, volume);  // G5
    vTaskDelay(pdMS_TO_TICKS(100));
    buzzer_tone(1047, volume);  // C6
    vTaskDelay(pdMS_TO_TICKS(200));

    buzzer_tone(0, 0);  // Silence
}

void play_alarm_tone(alarm_tone_t tone, uint8_t volume) {
    switch (tone) {
        case ALARM_TONE_BEEP_1:
            // Single 800Hz beep, 200ms
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;

        case ALARM_TONE_BEEP_2:
            // Double beep
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;

        case ALARM_TONE_BEEP_3:
            // Triple beep
            for (int i = 0; i < 3; i++) {
                buzzer_tone(800, volume);
                vTaskDelay(pdMS_TO_TICKS(200));
                buzzer_stop();
                if (i < 2) vTaskDelay(pdMS_TO_TICKS(100));
            }
            break;

        case ALARM_TONE_CHIME:
            // Rising chime: 600Hz -> 800Hz
            buzzer_tone(600, volume);
            vTaskDelay(pdMS_TO_TICKS(150));
            buzzer_tone(700, volume);
            vTaskDelay(pdMS_TO_TICKS(150));
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(150));
            buzzer_stop();
            break;

        case ALARM_TONE_ALERT:
            // Rapid pulsing alert: 1000Hz, 3 pulses
            for (int i = 0; i < 3; i++) {
                buzzer_tone(1000, volume);
                vTaskDelay(pdMS_TO_TICKS(100));
                buzzer_stop();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown alarm tone: %d", tone);
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;
    }
}
