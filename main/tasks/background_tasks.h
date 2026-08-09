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

// Glucose task stack (bytes). Lives here rather than main.h because this module
// owns the canonical creation table; WEATHER/TIME/BATTERY_STACK_SIZE are in main.h.
#define GLUCOSE_STACK_SIZE 5120

/**
 * Create every background task that is not already running, each with its
 * canonical stack, priority and core pinning. Idempotent. Also declared in
 * main.h — this module is where it is defined.
 */
void ensure_tasks_running(void);

/** UI task (Core 1): LVGL, touch input and display updates. */
void ui_task_core1(void *pvParameters);

/** WiFi task (Core 0): connectivity and health checks. */
void wifi_task_core0(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // TASKS_BACKGROUND_TASKS_H
