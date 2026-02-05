/*
 * home_screen.h
 *
 * Main home screen with time, weather, and glucose display
 */

#ifndef UI_HOME_SCREEN_H
#define UI_HOME_SCREEN_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create home screen with time, date, weather, and glucose widgets
 */
void create_home_screen(void);

/**
 * Draw custom battery icon with specified color and number of bars
 * @param canvas LVGL canvas object
 * @param color Battery color (green, orange, red)
 * @param bars Number of bars to show (1-4)
 */
void draw_battery_icon(lv_obj_t *canvas, uint32_t color, int bars);

#ifdef __cplusplus
}
#endif

#endif // UI_HOME_SCREEN_H
