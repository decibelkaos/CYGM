/*
 * battery.h
 *
 * ADC-based battery voltage monitoring and percentage estimation.
 */

#ifndef HARDWARE_BATTERY_H
#define HARDWARE_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the battery ADC. */
void battery_init(void);

/** Battery task: samples voltage, updates the display, handles low-battery shutdown. */
void battery_monitor_task(void *arg);

/** Refresh the battery icon and percentage. Returns early if the UI is not up yet. */
void update_battery_display(void);

/**
 * Battery color for a percentage: green at 100%, fading through yellow and
 * orange, solid red below 15%.
 */
uint32_t battery_percent_to_color(int percent);

/** Darker variant of battery_percent_to_color() for gradient fills. */
uint32_t battery_percent_to_dark_color(int percent);

/**
 * True once trend-based charger detection has confirmed charging. Detection
 * lags a plug-in by one battery-task cycle (5-60s).
 */
bool battery_is_charging(void);

/**
 * Bench console command, reached as "bat <args>".
 *   bat mv <500-5000>  report this voltage instead of the ADC
 *   bat mv off         go back to the real reading
 * While a value is forced the critical shutdown logs what it would do and
 * stays awake, because deep sleep arms no wake source. Cleared by a reboot.
 */
void battery_command(const char *args);

/** False once the ADC has shown no cell fitted (USB-only build). */
bool battery_is_present(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_BATTERY_H
