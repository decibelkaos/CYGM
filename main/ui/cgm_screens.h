/*
 * cgm_screens.h - CGM provider configuration screens.
 */

#ifndef UI_CGM_SCREENS_H
#define UI_CGM_SCREENS_H

#ifdef __cplusplus
extern "C" {
#endif

void create_cgm_menu_screen(void);
void create_dexcom_auth_screen(void);
void create_dexcom_login_screen(void);
void create_dexcom_username_entry_screen(void);
void create_dexcom_password_entry_screen(void);
void create_dexcom_status_screen(void);

// LibreLinkUp screens
void create_libre_auth_screen(void);
void create_libre_login_screen(void);
void create_libre_email_entry_screen(void);
void create_libre_password_entry_screen(void);
void create_libre_status_screen(void);

// Nightscout screens
void create_nightscout_auth_screen(void);
void create_nightscout_login_screen(void);
void create_nightscout_url_entry_screen(void);
void create_nightscout_token_entry_screen(void);
void create_nightscout_status_screen(void);

#ifdef __cplusplus
}
#endif

#endif // UI_CGM_SCREENS_H
