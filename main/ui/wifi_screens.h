/*
 * wifi_screens.h
 *
 * WiFi configuration screens
 */

#ifndef UI_WIFI_SCREENS_H
#define UI_WIFI_SCREENS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create WiFi network list screen with scan results
 */
void create_wifi_list_screen(void);

/**
 * Create WiFi password entry screen
 */
void create_wifi_password_screen(void);

/**
 * WiFi scan task (background scan with UI animation)
 * @param pvParameters Task parameter (unused)
 */
void wifi_scan_task(void *pvParameters);

/**
 * Scan WiFi networks and populate UI list
 */
void wifi_scan_and_show_networks(void);

#ifdef __cplusplus
}
#endif

#endif // UI_WIFI_SCREENS_H
