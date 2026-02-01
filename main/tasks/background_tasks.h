/*
 * background_tasks.h
 *
 * FreeRTOS background tasks
 */

#ifndef TASKS_BACKGROUND_TASKS_H
#define TASKS_BACKGROUND_TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UI task - runs on Core 1
 * Handles LVGL, touch input, display updates
 * @param pvParameters Task parameter (unused)
 */
void ui_task_core1(void *pvParameters);

/**
 * WiFi task - runs on Core 0
 * Handles network connectivity and health checks
 * @param pvParameters Task parameter (unused)
 */
void wifi_task_core0(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // TASKS_BACKGROUND_TASKS_H
