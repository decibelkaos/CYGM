/*
 * menu_screen.h
 *
 * Main menu screen with navigation to all settings
 */

#ifndef UI_MENU_SCREEN_H
#define UI_MENU_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create main menu screen with navigation buttons
 * Settings: Time/Weather, WiFi, CGM, Alarms
 */
void create_menu_screen(void);

#ifdef __cplusplus
}
#endif

#endif // UI_MENU_SCREEN_H
