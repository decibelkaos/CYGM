/**
 * LVGL Configuration for CYGM
 * Enables larger fonts for better readability on 2.8" 320x240 display
 */

#ifndef LV_CONF_H
#define LV_CONF_H

// Use default LVGL config as base
#include "../../managed_components/lvgl__lvgl/lv_conf_template.h"

// Enable additional Montserrat fonts for better readability
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_32  1
#define LV_FONT_MONTSERRAT_48  1

#endif /* LV_CONF_H */
