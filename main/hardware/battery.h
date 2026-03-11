/*
 * battery.h
 *
 * Battery monitoring module
 * ADC-based voltage monitoring with percentage calculation
 */

#ifndef HARDWARE_BATTERY_H
#define HARDWARE_BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize battery monitoring (ADC configuration)
 */
void battery_init(void);

/**
 * Battery monitoring task
 * Monitors voltage every 30 seconds, updates display, handles low battery shutdown
 * @param arg Task parameter (unused)
 */
void battery_monitor_task(void *arg);

/**
 * Update battery display widgets (icon and percentage)
 * Safe to call before UI is initialized (will return early)
 */
void update_battery_display(void);

/**
 * Get battery color for a given percentage (smooth green→red gradient)
 * 100% = green, fades through yellow/orange to red at 15%, solid red below 15%
 */
uint32_t battery_percent_to_color(int percent);

/**
 * Get darker battery color variant for gradient fills
 */
uint32_t battery_percent_to_dark_color(int percent);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_BATTERY_H
