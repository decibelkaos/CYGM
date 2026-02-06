/*
 * glucose_chart.h
 *
 * Glucose history chart overlay UI
 */

#ifndef UI_GLUCOSE_CHART_H
#define UI_GLUCOSE_CHART_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create and show the glucose history chart overlay
void create_glucose_chart_overlay(void);

// Close the glucose history chart overlay
void close_glucose_chart_overlay(void);

#ifdef __cplusplus
}
#endif

#endif // UI_GLUCOSE_CHART_H
