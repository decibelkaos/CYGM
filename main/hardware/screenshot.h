/*
 * Screenshot Capture — BMP to SD card or base64 over serial
 *
 * Intercepts the LVGL flush callback during a forced re-render and streams the
 * frame out line by line. RAM overhead is ~960 bytes (one row buffer).
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Start the UART command listener that handles the screenshot and dev-tool
 * commands. Call after sd_logger_init() during boot.
 */
esp_err_t screenshot_init(void);

/**
 * Capture the screen to /sdcard/SSHHMMss.BMP. Holds the LVGL port lock for
 * ~200-400ms and the SD log mutex for the duration.
 */
esp_err_t screenshot_take(void);

/**
 * Capture the screen and stream it over UART as base64 rows — no SD card
 * needed. Takes ~18s at 115200 baud and holds the LVGL lock throughout, so the
 * UI freezes and the capture is one consistent frame.
 */
esp_err_t screenshot_take_serial(void);

/**
 * UI hold flag. While set, the inactivity machinery that would move the screen
 * out from under a remote session stands down: return-to-home, the idle dim and
 * self-navigating module timers. Alarms are deliberately NOT affected.
 *
 * Read it through cygm_ui_hold_active(), never directly — the flag self-expires
 * so a forgotten hold cannot strand the device.
 */
extern volatile bool cygm_ui_hold;

/**
 * True while the UI hold is in force; clears the flag once its 10-minute
 * lifetime has elapsed. Safe from any task.
 */
bool cygm_ui_hold_active(void);

/**
 * Trend-arrow demo flag. While set, the home screen's arrow is driven through a
 * random trend every 10 seconds. It touches the arrow only — the glucose value,
 * the history ring and the alarm engine are never fed synthetic data, and a real
 * reading always wins.
 *
 * Read it through cygm_demo_trend_active(), never directly — the flag
 * self-expires so a forgotten demo cannot leave a device lying about its trend.
 */
extern volatile bool cygm_demo_trend;

/**
 * True while the trend demo is in force; clears the flag once its 30-minute
 * lifetime has elapsed. Safe from any task.
 */
bool cygm_demo_trend_active(void);
