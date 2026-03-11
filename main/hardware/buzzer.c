/*
 * buzzer.c
 *
 * Buzzer/speaker control implementation.
 *
 * IMPORTANT — LEDC spinlock safety:
 * All LEDC channels using LEDC_LOW_SPEED_MODE (buzzer, RGB LED, backlight)
 * share a single spinlock inside the ESP-IDF LEDC driver.  Acquiring that
 * spinlock disables interrupts; if another core already holds it, the
 * requesting core spins with interrupts masked, and the Interrupt WDT fires
 * after ~300 ms.
 *
 * To avoid this, the SILENCE path uses NO LEDC calls at all — it detaches
 * the GPIO from the LEDC peripheral via the GPIO matrix (a direct register
 * write, no spinlock).  This also prevents WiFi EMI from coupling through
 * the LEDC output driver into the 8002A amplifier, which would otherwise
 * cause audible buzzing/crackling when the speaker should be quiet.
 */

#include "buzzer.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/ledc_periph.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "BUZZER";
static volatile bool buzzer_active = false;

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

    // Initialize LEDC channel for buzzer (one-time setup, never reconfigured)
    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,  // Start silent
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    // Immediately disconnect GPIO from LEDC and hold low.
    // Prevents 8002A amplifier from picking up WiFi EMI noise.
    gpio_set_level(BUZZER_GPIO, 0);
    esp_rom_gpio_connect_out_signal(BUZZER_GPIO, SIG_GPIO_OUT_IDX, false, false);

    ESP_LOGI(TAG, "Buzzer PWM initialized (GPIO %d, silenced)", BUZZER_GPIO);
}

void buzzer_tone(uint32_t frequency, uint8_t volume) {
    if (frequency == 0 || volume == 0) {
        if (!buzzer_active) return;  // Already silent, skip

        // SILENCE — no LEDC calls, no spinlock, safe from any core.
        // Disconnect GPIO from LEDC output and drive pin low via GPIO register.
        // This cuts the electrical path from the LEDC peripheral to the 8002A
        // amplifier, preventing WiFi EMI from producing audible noise.
        gpio_set_level(BUZZER_GPIO, 0);
        esp_rom_gpio_connect_out_signal(BUZZER_GPIO, SIG_GPIO_OUT_IDX, false, false);
        buzzer_active = false;
        return;
    }

    // Clamp volume to 0-100
    if (volume > 100) volume = 100;

    // Configure LEDC frequency and duty BEFORE connecting the GPIO,
    // so the very first PWM cycle the amplifier sees is already correct.
    ledc_timer_resume(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency);

    // 50% duty cycle scaled by volume (0-100% -> 0-512 of 1023)
    uint32_t duty = (512 * volume) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);

    // Reconnect GPIO to LEDC output signal (fast GPIO-matrix register write).
    // Only needed when transitioning from silent to active.
    if (!buzzer_active) {
        esp_rom_gpio_connect_out_signal(BUZZER_GPIO,
            ledc_periph_signal[LEDC_LOW_SPEED_MODE].sig_out0_idx + BUZZER_CHANNEL,
            false, false);
        buzzer_active = true;
    }
}

void buzzer_stop(void) {
    buzzer_tone(0, 0);
}

// Play boot-up sound: quick two-tone beep (low -> high)
void play_boot_sound(void) {
    const uint8_t volume = 50;

    // Low tone (C5 = 523 Hz), 100ms
    buzzer_tone(523, volume);
    vTaskDelay(pdMS_TO_TICKS(100));

    // High tone (G5 = 784 Hz), 100ms
    buzzer_tone(784, volume);
    vTaskDelay(pdMS_TO_TICKS(100));

    buzzer_tone(0, 0);
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

        case ALARM_TONE_TWINKLE:
            // Twinkle Twinkle Little Star: C C G G A A G
            buzzer_tone(523, volume);  // C
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(523, volume);  // C
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(784, volume);  // G
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(784, volume);  // G
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(880, volume);  // A
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(880, volume);  // A
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(784, volume);  // G
            vTaskDelay(pdMS_TO_TICKS(600));
            buzzer_stop();
            break;

        case ALARM_TONE_FUR_ELISE:
            // Fur Elise (Beethoven) - Opening phrase: E D# E D# E B D C A
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(622, volume);  // D#5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(622, volume);  // D#5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(494, volume);  // B4
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(587, volume);  // D5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(523, volume);  // C5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(440, volume);  // A4
            vTaskDelay(pdMS_TO_TICKS(400));
            buzzer_stop();
            break;

        case ALARM_TONE_DIXIE_HORN:
            // Dixie Horn (Dukes of Hazzard General Lee) - From bass tab
            // Sequence: C# B A-A-A B C# D E-E-E C#
            buzzer_tone(554, volume);  // C#5 (opening)
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(494, volume);  // B4
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(440, volume);  // A4 (triplet start)
            vTaskDelay(pdMS_TO_TICKS(180));
            buzzer_tone(440, volume);  // A4
            vTaskDelay(pdMS_TO_TICKS(180));
            buzzer_tone(440, volume);  // A4
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(494, volume);  // B4
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(554, volume);  // C#5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(587, volume);  // D5 (quick transition)
            vTaskDelay(pdMS_TO_TICKS(180));
            buzzer_tone(659, volume);  // E5 (triplet start)
            vTaskDelay(pdMS_TO_TICKS(180));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(180));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_tone(554, volume);  // C#5 (final note, held)
            vTaskDelay(pdMS_TO_TICKS(500));
            buzzer_stop();
            break;

        case ALARM_TONE_SAINTS:
            // When the Saints Go Marching In: C E F G - C E F G
            buzzer_tone(523, volume);  // C5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(698, volume);  // F5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(784, volume);  // G5
            vTaskDelay(pdMS_TO_TICKS(600));
            buzzer_tone(523, volume);  // C5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(659, volume);  // E5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(698, volume);  // F5
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_tone(784, volume);  // G5
            vTaskDelay(pdMS_TO_TICKS(600));
            buzzer_stop();
            break;

        case ALARM_TONE_SIREN:
            // Emergency siren - alternating high/low tones
            for (int i = 0; i < 4; i++) {
                buzzer_tone(880, volume);   // A5 (high)
                vTaskDelay(pdMS_TO_TICKS(150));
                buzzer_tone(660, volume);   // E5 (low)
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            buzzer_stop();
            break;

        case ALARM_TONE_ASCENDING: {
            // Ascending scale: C5 D5 E5 F5 G5 A5 B5 C6
            const uint32_t scale[] = {523, 587, 659, 698, 784, 880, 988, 1047};
            for (int i = 0; i < 8; i++) {
                buzzer_tone(scale[i], volume);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            buzzer_stop();
            break;
        }

        case ALARM_TONE_DOORBELL:
            // Classic ding-dong
            buzzer_tone(988, volume);   // B5 (ding)
            vTaskDelay(pdMS_TO_TICKS(400));
            buzzer_tone(784, volume);   // G5 (dong)
            vTaskDelay(pdMS_TO_TICKS(600));
            buzzer_stop();
            break;

        case ALARM_TONE_BIG_BEN:
            // Westminster Quarters (Big Ben) - first quarter chime
            buzzer_tone(330, volume);   // E4
            vTaskDelay(pdMS_TO_TICKS(500));
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(50));
            buzzer_tone(415, volume);   // G#4
            vTaskDelay(pdMS_TO_TICKS(500));
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(50));
            buzzer_tone(370, volume);   // F#4
            vTaskDelay(pdMS_TO_TICKS(500));
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(50));
            buzzer_tone(247, volume);   // B3
            vTaskDelay(pdMS_TO_TICKS(750));
            buzzer_stop();
            break;

        default:
            ESP_LOGW(TAG, "Unknown alarm tone: %d", tone);
            buzzer_tone(800, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;
    }
}
