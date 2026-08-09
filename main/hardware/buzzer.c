/*
 * buzzer.c
 *
 * Buzzer/speaker control.
 *
 * EMI: the 8002A amplifier's shutdown pin is hardwired to VCC on this board, so
 * there is no software mute, and WiFi TX bursts couple noise into its input.
 * Silencing therefore stops LEDC switching, detaches the GPIO from the LEDC
 * matrix, and drives it OUTPUT LOW — the NMOS pull-down clamps coupled noise at
 * ~30 ohms, where INPUT with pull-down would leave ~45k. This briefly takes the
 * shared LEDC spinlock, but only on transitions, never in a hot loop.
 */

#include "buzzer.h"
#include "shared_state.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_gpio.h"
#include "soc/ledc_periph.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "BUZZER";
static volatile bool buzzer_active = false;

// Volume (0-100) -> LEDC duty (10-bit, 0-512 = 0-50%).
//
// A square wave's acoustic fundamental follows sin(pi * duty), which flattens as
// duty nears 50%. A linear map therefore spanned only ~3dB across volume 50-100,
// making alarm escalation inaudible. This table puts the fundamental at
// (volume/100)^1.5 of full amplitude, spreading the same range over ~9dB.
static const uint16_t volume_duty_lut[11] = {
    0, 10, 29, 54, 83, 118, 157, 204, 259, 333, 512
};

static uint32_t buzzer_volume_to_duty(uint8_t volume) {
    if (volume == 0) return 0;
    if (volume >= 100) return volume_duty_lut[10];

    uint32_t lo = volume_duty_lut[volume / 10];
    uint32_t hi = volume_duty_lut[volume / 10 + 1];
    return lo + ((hi - lo) * (volume % 10)) / 10;
}

void buzzer_init(void) {
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

    // Fully silence: stop the channel, pause the timer, then clamp the GPIO to
    // ground. OUTPUT LOW turns on the NMOS (~30 ohms), rejecting EMI coupled from
    // WiFi TX far better than a high-impedance input with pull-down (~45k).
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);
    esp_rom_gpio_connect_out_signal(BUZZER_GPIO, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0);

    ESP_LOGI(TAG, "Buzzer PWM initialized (GPIO %d, silenced, output LOW ground clamp)", BUZZER_GPIO);
}

void buzzer_tone(uint32_t frequency, uint8_t volume) {
    if (frequency == 0 || volume == 0) {
        if (!buzzer_active) return;  // Already silent, skip

        // SILENCE — stop LEDC, then clamp the GPIO to ground via OUTPUT LOW so
        // the NMOS (~30 ohms) shorts coupled EMI away.
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);

        esp_rom_gpio_connect_out_signal(BUZZER_GPIO, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(BUZZER_GPIO, 0);
        buzzer_active = false;
        return;
    }

    if (volume > 100) volume = 100;

    // GPIO is already OUTPUT from the silence path; it is reconnected to LEDC below.

    // Configure LEDC frequency and duty BEFORE connecting the GPIO,
    // so the very first PWM cycle the amplifier sees is already correct.
    ledc_timer_resume(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency);

    // Perceptual volume curve — see volume_duty_lut
    uint32_t duty = buzzer_volume_to_duty(volume);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);

    // Reconnect the GPIO to the LEDC output, needed only when leaving silence.
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
    const uint8_t volume = 80;  // Matches the pre-curve loudness of the old 50

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
    const uint8_t volume = 85;  // Matches the pre-curve loudness of the old 60

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

// ---- Extended tone set (step tables) --------------------------------------
//
// Irregular-rhythm tones are {frequency Hz, hold ms} steps, where frequency 0 is
// a silent gap. The tables are static const, so they sit in flash and cost no
// RAM. Every hold is a multiple of 10ms because the 100Hz FreeRTOS tick truncates
// anything finer — a shorter hold would not delay at all.
typedef struct {
    uint16_t freq;
    uint16_t ms;
} buzzer_step_t;

static void buzzer_play_steps(const buzzer_step_t *steps, uint8_t count, uint8_t volume) {
    for (uint8_t i = 0; i < count; i++) {
        buzzer_tone(steps[i].freq, volume);
        vTaskDelay(pdMS_TO_TICKS(steps[i].ms));
    }
    buzzer_stop();
}

#define BUZZER_PLAY_STEPS(tbl, vol) \
    buzzer_play_steps((tbl), (uint8_t)(sizeof(tbl) / sizeof((tbl)[0])), (vol))

// Low double-thump ("lub-dub") — the gentlest tone in the set.
static const buzzer_step_t tone_steps_heartbeat[] = {
    {160, 100}, {0, 50}, {210, 80}, {0, 350},
    {160, 100}, {0, 50}, {210, 80}, {0, 350},
    {160, 100}, {0, 50}, {210, 80}, {0, 150},
};

// Warm four-note run down and back up, ending an octave high.
static const buzzer_step_t tone_steps_marimba[] = {
    {784, 80}, {0, 20}, {659, 80}, {0, 20}, {523, 80}, {0, 20},
    {659, 80}, {0, 20}, {784, 80}, {0, 20}, {1047, 220},
};

// Rising arpeggio across two octaves, played legato (no gaps between notes).
static const buzzer_step_t tone_steps_harp[] = {
    {523, 70}, {659, 70}, {784, 70}, {1047, 70},
    {1319, 70}, {1568, 70}, {2093, 300},
};

// Ping with a short downward tail, then a long silent decay.
static const buzzer_step_t tone_steps_sonar[] = {
    {1568, 70}, {1319, 50}, {0, 700},
    {1568, 70}, {1319, 50}, {0, 700},
    {1568, 70}, {1319, 50}, {0, 200},
};

// Falling major third (G5 -> E5), three calls.
static const buzzer_step_t tone_steps_cuckoo[] = {
    {784, 220}, {659, 300}, {0, 250},
    {784, 220}, {659, 300}, {0, 250},
    {784, 220}, {659, 380},
};

// Morse SOS: three short, three long, three short.
static const buzzer_step_t tone_steps_sos[] = {
    {900, 100}, {0, 100}, {900, 100}, {0, 100}, {900, 100}, {0, 250},
    {900, 300}, {0, 100}, {900, 300}, {0, 100}, {900, 300}, {0, 250},
    {900, 100}, {0, 100}, {900, 100}, {0, 100}, {900, 100},
};

// Two long low blasts. The rapid pitch swap is what makes this read as a horn
// chord — one steady square wave at this pitch just sounds like a low beep.
static const buzzer_step_t tone_steps_train[] = {
    {233, 40}, {175, 40}, {233, 40}, {175, 40}, {233, 40}, {175, 40},
    {233, 40}, {175, 40}, {233, 40}, {175, 40}, {233, 40}, {175, 40},
    {0, 200},
    {233, 40}, {175, 40}, {233, 40}, {175, 40}, {233, 40}, {175, 40},
    {233, 40}, {175, 40}, {233, 40}, {175, 40}, {233, 40}, {175, 40},
    {233, 40}, {175, 40}, {233, 40}, {175, 40},
};

// Harsh dual-tone burst, four repeats.
static const buzzer_step_t tone_steps_pager[] = {
    {2400, 80}, {0, 60}, {2800, 80}, {0, 300},
    {2400, 80}, {0, 60}, {2800, 80}, {0, 300},
    {2400, 80}, {0, 60}, {2800, 80}, {0, 300},
    {2400, 80}, {0, 60}, {2800, 80}, {0, 150},
};

// Three deep tolls. Volume is fixed across the tone, so the ring-down is rhythmic
// rather than amplitude-based: a shimmering strike, then bursts that shorten as
// the gaps between them lengthen.
static const buzzer_step_t tone_steps_church_bell[] = {
    {330, 40}, {247, 40}, {330, 40}, {247, 40},
    {247, 200}, {0, 60}, {247, 120}, {0, 100}, {247, 60}, {0, 300},
    {330, 40}, {247, 40}, {330, 40}, {247, 40},
    {247, 200}, {0, 60}, {247, 120}, {0, 100}, {247, 60}, {0, 300},
    {330, 40}, {247, 40}, {330, 40}, {247, 40},
    {247, 240}, {0, 60}, {247, 140}, {0, 100}, {247, 80},
};

// Bright five-note run up the octave (C6 D6 E6 G6 C7), played twice with the
// top note held. The short gaps are what give each note a struck edge.
static const buzzer_step_t tone_steps_xylophone[] = {
    {1047, 90}, {0, 30}, {1175, 90}, {0, 30}, {1319, 90}, {0, 30},
    {1568, 90}, {0, 30}, {2093, 260}, {0, 180},
    {1047, 90}, {0, 30}, {1175, 90}, {0, 30}, {1319, 90}, {0, 30},
    {1568, 90}, {0, 30}, {2093, 320},
};

// Three hard bursts and a rising kicker, three times over. Every pitch sits in
// the speaker's resonance band, so this is the loudest tone in the set.
static const buzzer_step_t tone_steps_alert_triple[] = {
    {2500, 70}, {0, 50}, {2500, 70}, {0, 50}, {2500, 70}, {0, 50},
    {2700, 90}, {0, 250},
    {2500, 70}, {0, 50}, {2500, 70}, {0, 50}, {2500, 70}, {0, 50},
    {2700, 90}, {0, 250},
    {2500, 70}, {0, 50}, {2500, 70}, {0, 50}, {2500, 70}, {0, 50},
    {2700, 200},
};

// Randomized burst, regenerated on every play so the ear never learns it and it
// keeps its startle value where a fixed pattern fades into background noise.
//
// Deliberately has no step table: steps are drawn from the hardware RNG one at a
// time and played straight out, costing one uint32_t of stack and no heap. Every
// pitch lands in the 1500-3000Hz band where this speaker is loudest.
#define RANDOM_TONE_MIN_STEPS 6
#define RANDOM_TONE_MAX_STEPS 10

static void buzzer_play_random(uint8_t volume) {
    uint32_t seed = esp_random();
    uint8_t steps = RANDOM_TONE_MIN_STEPS +
                    (uint8_t)(seed % (RANDOM_TONE_MAX_STEPS - RANDOM_TONE_MIN_STEPS + 1));

    for (uint8_t i = 0; i < steps; i++) {
        uint32_t r = esp_random();

        uint32_t freq = 1500 + (r % 1501);                 // 1500-3000 Hz
        uint32_t ms   = 60 + ((r >> 11) % 13) * 10;        // 60-180 ms, tick-aligned

        buzzer_tone(freq, volume);
        vTaskDelay(pdMS_TO_TICKS(ms));

        // Roughly one step in three is followed by a silence, so the rhythm never
        // settles into a pattern either.
        if (((r >> 24) % 3) == 0) {
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(40 + ((r >> 26) % 5) * 10));   // 40-80 ms
        }
    }

    buzzer_stop();
}

// ---- Alarm pitch and loudness -------------------------------------------
//
// Duty already sits at the square wave's 50% ceiling (see volume_duty_lut), so
// amplitude has nothing left to give — pitch does. This speaker driven by the
// 8002A peaks around 2.2-2.8kHz, several dB above the 800Hz-1kHz the alarm tones
// originally used, so the URGENT tones have their pitch centres moved into that
// band with their rhythm untouched. Melodic and gentle tones keep their original
// pitches, where character matters more than the last few dB.
#define ALARM_BEEP_HZ 2400   // Beep 1/2/3 — was 800Hz, moved into the loud band

void play_alarm_tone(alarm_tone_t tone, uint8_t volume) {
    switch (tone) {
        case ALARM_TONE_BEEP_1:
            // Single beep, 200ms
            buzzer_tone(ALARM_BEEP_HZ, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;

        case ALARM_TONE_BEEP_2:
            // Double beep
            buzzer_tone(ALARM_BEEP_HZ, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            buzzer_tone(ALARM_BEEP_HZ, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;

        case ALARM_TONE_BEEP_3:
            // Triple beep
            for (int i = 0; i < 3; i++) {
                buzzer_tone(ALARM_BEEP_HZ, volume);
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
            // Emergency siren - alternating high/low tones, both in the band
            for (int i = 0; i < 4; i++) {
                buzzer_tone(2600, volume);  // high
                vTaskDelay(pdMS_TO_TICKS(150));
                buzzer_tone(2200, volume);  // low
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

        case ALARM_TONE_HEARTBEAT:
            BUZZER_PLAY_STEPS(tone_steps_heartbeat, volume);
            break;

        case ALARM_TONE_MARIMBA:
            BUZZER_PLAY_STEPS(tone_steps_marimba, volume);
            break;

        case ALARM_TONE_HARP:
            BUZZER_PLAY_STEPS(tone_steps_harp, volume);
            break;

        case ALARM_TONE_SONAR:
            BUZZER_PLAY_STEPS(tone_steps_sonar, volume);
            break;

        case ALARM_TONE_CUCKOO:
            BUZZER_PLAY_STEPS(tone_steps_cuckoo, volume);
            break;

        case ALARM_TONE_TRILL:
            // Fast two-note alternation, resolving onto the lower note
            for (int i = 0; i < 6; i++) {
                buzzer_tone(880, volume);   // A5
                vTaskDelay(pdMS_TO_TICKS(60));
                buzzer_tone(1047, volume);  // C6
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            buzzer_tone(880, volume);
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_stop();
            break;

        case ALARM_TONE_SOS:
            BUZZER_PLAY_STEPS(tone_steps_sos, volume);
            break;

        case ALARM_TONE_TRAIN:
            BUZZER_PLAY_STEPS(tone_steps_train, volume);
            break;

        case ALARM_TONE_PAGER:
            BUZZER_PLAY_STEPS(tone_steps_pager, volume);
            break;

        case ALARM_TONE_SWEEP: {
            // Rising-then-falling siren sweep. LEDC only retunes in discrete steps,
            // so this is a frequency ladder rather than a glide, pitched so its top
            // half sits in the loud band.
            static const uint32_t sweep[] = {
                1100, 1300, 1550, 1850, 2200, 2600, 2800, 2600, 2200, 1850, 1550, 1300
            };
            for (int cycle = 0; cycle < 3; cycle++) {
                for (int i = 0; i < 12; i++) {
                    buzzer_tone(sweep[i], volume);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            buzzer_stop();
            break;
        }

        case ALARM_TONE_KLAXON:
            // Rasping honk — the fast pitch swap supplies the rasp. Pitched up into
            // the loud band with the interval widened to keep the rasp harsh at the
            // higher centre, which trades some warmth for being audible next door.
            for (int honk = 0; honk < 3; honk++) {
                for (int i = 0; i < 6; i++) {
                    buzzer_tone(2800, volume);
                    vTaskDelay(pdMS_TO_TICKS(40));
                    buzzer_tone(2100, volume);
                    vTaskDelay(pdMS_TO_TICKS(40));
                }
                buzzer_stop();
                vTaskDelay(pdMS_TO_TICKS(180));
            }
            break;

        case ALARM_TONE_RAPID_PULSE:
            // Most urgent of the set — twelve hard pulses, no let-up
            for (int i = 0; i < 12; i++) {
                buzzer_tone(2600, volume);
                vTaskDelay(pdMS_TO_TICKS(60));
                buzzer_stop();
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            break;

        case ALARM_TONE_RANDOM:
            buzzer_play_random(volume);
            break;

        case ALARM_TONE_CHURCH_BELL:
            BUZZER_PLAY_STEPS(tone_steps_church_bell, volume);
            break;

        case ALARM_TONE_XYLOPHONE:
            BUZZER_PLAY_STEPS(tone_steps_xylophone, volume);
            break;

        case ALARM_TONE_ALERT_TRIPLE:
            BUZZER_PLAY_STEPS(tone_steps_alert_triple, volume);
            break;

        default:
            ESP_LOGW(TAG, "Unknown alarm tone: %d", tone);
            buzzer_tone(ALARM_BEEP_HZ, volume);
            vTaskDelay(pdMS_TO_TICKS(200));
            buzzer_stop();
            break;
    }
}
