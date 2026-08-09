/*
 * alarm_screens.h - alarm configuration screens.
 */

#ifndef UI_ALARM_SCREENS_H
#define UI_ALARM_SCREENS_H

#include "nvs_config.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void create_alarm_settings_screen(void);
void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm, int y_pos, int height);
void create_tone_picker_screen(alarm_config_t *alarm);
void create_alarm_preview_screen(void);
void create_alarm_preview_card(lv_obj_t *parent, const char *title, int glucose_value, alarm_config_t *alarm, lv_color_t color_low, lv_color_t color_high);
void create_alarm_detail_editor_screen(alarm_config_t *alarm);

#ifdef __cplusplus
}
#endif

#endif // UI_ALARM_SCREENS_H
