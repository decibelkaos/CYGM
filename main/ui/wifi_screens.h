/*
 * wifi_screens.h - WiFi configuration screens.
 */

#ifndef UI_WIFI_SCREENS_H
#define UI_WIFI_SCREENS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void create_wifi_list_screen(void);
void create_wifi_password_screen(void);

// Background scan task; drives the scanning overlay.
void wifi_scan_task(void *pvParameters);
void wifi_scan_and_show_networks(void);

void show_wifi_scanning_overlay(void);
void hide_wifi_scanning_overlay(void);

// Confirm-to-forget overlay for a saved network.
void show_wifi_removal_overlay(const char *ssid);

// Home-screen WiFi icon: green when connected, gray with warning otherwise.
void update_wifi_status_display(bool connected);

#ifdef __cplusplus
}
#endif

#endif // UI_WIFI_SCREENS_H
