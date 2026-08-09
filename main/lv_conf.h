/**
 * LVGL Configuration for CYGM
 *
 * NOT COMPILED IN: the build sets CONFIG_LV_CONF_SKIP=y, so lv_conf_internal.h
 * takes every LVGL option from Kconfig and never includes this file. Change
 * LVGL settings in sdkconfig.defaults (CONFIG_LV_*), not here. The defines
 * below are kept only as the fallback set if LV_CONF_SKIP is ever turned off.
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
