/*
 * Screenshot Capture — BMP to SD Card
 *
 * Captures the current LVGL display by intercepting the flush callback
 * during a forced re-render. Streams line-by-line to a 24-bit BMP file
 * on the SD card. RAM overhead: ~960 bytes (one row conversion buffer).
 *
 * Trigger: type "ss" or "screenshot" in the serial monitor (idf.py monitor)
 */

#pragma once

#include "esp_err.h"

/**
 * Initialize the screenshot system.
 * Starts a lightweight UART command listener task.
 * Call after sd_logger_init() during boot.
 */
esp_err_t screenshot_init(void);

/**
 * Capture the current screen and save as BMP to SD card.
 * Filename: /sdcard/SSHHMMss.BMP (timestamp-based, 8.3 FAT format)
 *
 * Holds LVGL port lock during capture (~200-400ms).
 * Holds SD card log_mutex for the duration.
 *
 * @return ESP_OK on success, error otherwise
 */
esp_err_t screenshot_take(void);
