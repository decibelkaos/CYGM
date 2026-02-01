/*
 * menu_screen.c  
 *
 * Main menu screen implementation
 */

#include "menu_screen.h"
#include "shared_state.h"
#include "main.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "MENU_SCREEN";

void create_menu_screen(void) {
    screen_menu = lv_obj_create(NULL);
    lv_obj_add_style(screen_menu, &style_bg, 0);
    
    // Header
    lv_obj_t *header = lv_label_create(screen_menu);
    lv_label_set_text(header, "Settings");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_menu);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);

    // Attach event handler
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Menu list container
    lv_obj_t *list = lv_obj_create(screen_menu);
    lv_obj_set_size(list, 300, 180);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 12, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_top(list, 5, 0);  // Add top padding to center items vertically
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    
    // Menu items
    const char *items[] = {"CGM Settings", "WiFi Setup", "Time/Date/Weather", "Alarm"};
    const char *icons[] = {LV_SYMBOL_EYE_OPEN, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS, LV_SYMBOL_BELL};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, 300, 36);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 15, 0);
        lv_obj_set_style_pad_right(row, 15, 0);
        
        // Icon
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        
        // Label
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, items[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 30, 0);
        
        // Arrow
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

        // Add click handler with index as user data
        lv_obj_add_event_cb(row, menu_item_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}
