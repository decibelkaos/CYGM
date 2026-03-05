/*
 * wifi_screens.h
 *
 * WiFi configuration screens
 */

#ifndef UI_WIFI_SCREENS_H
#define UI_WIFI_SCREENS_H

#include <stdbool.h>

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

/**
 * Show WiFi scanning overlay (with animation)
 */
void show_wifi_scanning_overlay(void);

/**
 * Hide WiFi scanning overlay
 */
void hide_wifi_scanning_overlay(void);

/**
 * Show WiFi removal confirmation overlay with slider
 * @param ssid SSID of network to remove
 */
void show_wifi_removal_overlay(const char *ssid);

/**
 * Update WiFi status icon on home screen
 * @param connected true = green WiFi icon, false = gray with warning
 */
void update_wifi_status_display(bool connected);

#ifdef __cplusplus
}
#endif

#endif // UI_WIFI_SCREENS_H
