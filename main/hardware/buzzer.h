/*
 * buzzer.h
 *
 * Buzzer/speaker control module
 * PWM-based tone generation for 8002A amplifier
 */

#ifndef HARDWARE_BUZZER_H
#define HARDWARE_BUZZER_H

#include <stdint.h>
#include "nvs_config.h"  // For alarm_tone_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize buzzer PWM channel
 */
void buzzer_init(void);

/**
 * Play a tone at specified frequency and volume
 * @param frequency Frequency in Hz (0 = silence)
 * @param volume Volume (0-100)
 */
void buzzer_tone(uint32_t frequency, uint8_t volume);

/**
 * Stop buzzer (silence)
 */
void buzzer_stop(void);

/**
 * Play boot sound sequence (ascending and descending beeps)
 */
void play_boot_sound(void);

/**
 * Play success sound (ascending arpeggio)
 */
void play_success_sound(void);

/**
 * Play alarm tone based on type
 * @param tone Alarm tone type (BEEP_1, BEEP_2, BEEP_3, CHIME, ALERT)
 * @param volume Volume (0-100)
 */
void play_alarm_tone(alarm_tone_t tone, uint8_t volume);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_BUZZER_H
