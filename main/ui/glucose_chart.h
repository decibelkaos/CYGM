/*
 * glucose_chart.h - glucose history chart overlay.
 */

#ifndef UI_GLUCOSE_CHART_H
#define UI_GLUCOSE_CHART_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void create_glucose_chart_overlay(void);
void close_glucose_chart_overlay(void);

#ifdef __cplusplus
}
#endif

#endif // UI_GLUCOSE_CHART_H
