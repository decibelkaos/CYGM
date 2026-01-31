/*
 * battery.h
 *
 * Battery monitoring module
 * ADC-based voltage monitoring with percentage calculation
 */

#ifndef HARDWARE_BATTERY_H
#define HARDWARE_BATTERY_H

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

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_BATTERY_H
