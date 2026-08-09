/*
 * buzzer.h
 *
 * PWM tone generation for the 8002A amplifier.
 */

#ifndef HARDWARE_BUZZER_H
#define HARDWARE_BUZZER_H

#include <stdint.h>
#include "nvs_config.h"  // For alarm_tone_t

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the buzzer PWM channel. */
void buzzer_init(void);

/** Play a tone; frequency in Hz (0 = silence), volume 0-100. */
void buzzer_tone(uint32_t frequency, uint8_t volume);

/** Silence the buzzer. */
void buzzer_stop(void);

/** Boot sound: ascending then descending beeps. */
void play_boot_sound(void);

/** Success sound: ascending arpeggio. */
void play_success_sound(void);

/**
 * Play an alarm tone. Blocks for the length of the tone (0.5s - 3s depending on
 * the selection; ALARM_TONE_RANDOM varies within that range on every call).
 * Volume is 0-100.
 */
void play_alarm_tone(alarm_tone_t tone, uint8_t volume);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_BUZZER_H
