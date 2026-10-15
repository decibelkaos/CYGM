/*
 * cygm_theme.c - shared facelift helpers. See cygm_theme.h for the contract.
 */

#include "cygm_theme.h"
#include "shared_state.h"
#include <string.h>

#define KEY(width) ((lv_btnmatrix_ctrl_t)(width))
#define SPECIAL_KEY(width) ((lv_btnmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BTN_FLAGS | (width)))

static const char *keyboard_map_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", ".", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t keyboard_ctrl_lower[] = {
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    SPECIAL_KEY(2), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), SPECIAL_KEY(2),
    SPECIAL_KEY(2), KEY(6), KEY(1), SPECIAL_KEY(2)
};

static const char *keyboard_map_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", ".", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t keyboard_ctrl_upper[] = {
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    SPECIAL_KEY(2), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), SPECIAL_KEY(2),
    SPECIAL_KEY(2), KEY(6), KEY(1), SPECIAL_KEY(2)
};

/* Two symbol pages: the stock handler only knows "1#" (-> SPECIAL), so the
 * NUMBER mode is borrowed as page two and MORE_KEY is routed to it by
 * keyboard_mode_event_cb. Between them they cover every printable ASCII
 * character a WiFi or CGM password can contain. */
#define MORE_KEY "#+="

static const char *keyboard_map_special[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "%", "&", "*", "-", "+", "=", "\n",
    "abc", "(", ")", "!", "?", "/", "_", ":", LV_SYMBOL_BACKSPACE, "\n",
    MORE_KEY, " ", ".", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t keyboard_ctrl_special[] = {
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    SPECIAL_KEY(2), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), SPECIAL_KEY(2),
    SPECIAL_KEY(2), KEY(6), KEY(1), SPECIAL_KEY(2)
};

static const char *keyboard_map_number[] = {
    "<", ">", "[", "]", "{", "}", "\\", "|", "^", "~", "\n",
    "\"", "'", ",", ";", "`", "!", "?", ":", "*", "\n",
    "abc", "(", ")", "/", "_", "-", "+", "=", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", ".", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t keyboard_ctrl_number[] = {
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1),
    SPECIAL_KEY(2), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), KEY(1), SPECIAL_KEY(2),
    SPECIAL_KEY(2), KEY(6), KEY(1), SPECIAL_KEY(2)
};

/* Stands in for lv_keyboard_def_event_cb so MORE_KEY switches pages instead
 * of being typed; everything else falls through to the stock handler. */
static void keyboard_mode_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(kb);
    if (id != LV_BTNMATRIX_BTN_NONE) {
        const char *txt = lv_btnmatrix_get_btn_text(kb, id);
        if (txt != NULL && strcmp(txt, MORE_KEY) == 0) {
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
            return;
        }
    }
    lv_keyboard_def_event_cb(e);
}

static void style_metal_knob(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xE6EAF0), LV_PART_KNOB);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x8A94A6), LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x5A6475), LV_PART_KNOB);
    lv_obj_set_style_border_width(obj, 1, LV_PART_KNOB);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_KNOB);
}

void cygm_theme_bg_gradient(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(CYGM_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(CYGM_BG_BOTTOM), LV_PART_MAIN);
    /* A flat field needs no gradient pass (and no dither) on every redraw */
    lv_obj_set_style_bg_grad_dir(obj, CYGM_BG_TOP == CYGM_BG_BOTTOM ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER,
                                 LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

void cygm_theme_keyboard(lv_obj_t *kb)
{
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER,
                        keyboard_map_lower, keyboard_ctrl_lower);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER,
                        keyboard_map_upper, keyboard_ctrl_upper);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL,
                        keyboard_map_special, keyboard_ctrl_special);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER,
                        keyboard_map_number, keyboard_ctrl_number);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, keyboard_mode_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    cygm_theme_bg_gradient(kb);
    lv_obj_set_style_pad_all(kb, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(kb, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(kb, lv_color_hex(CYGM_BTN_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(CYGM_EDGE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(kb, CYGM_EDGE_OPA, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(kb, lv_color_hex(COLOR_ACCENT_BLUE),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(kb, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(kb, 0,
                                  LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(kb, lv_color_hex(COLOR_ACCENT_LIGHT),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, 140,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(kb, 0,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
}

void cygm_theme_slider(lv_obj_t *slider, lv_color_t accent)
{
    /* Knob sits 2 px proud of the channel on each side: a fader cap, not a
     * marble. The 12 px ext_click_area the callers set keeps it easy to grab. */
    lv_coord_t pad = 2;

    lv_obj_set_style_bg_color(slider, lv_color_hex(0x0D1520), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, lv_color_hex(0x1E2D3D), LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(slider, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider, lv_color_lighten(accent, 60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_INDICATOR);

    style_metal_knob(slider);
    lv_obj_set_style_pad_all(slider, pad, LV_PART_KNOB);
}

void cygm_theme_switch(lv_obj_t *sw)
{
    lv_obj_set_style_anim_time(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x0D1520), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(0x1E2D3D), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_ACCENT_BLUE),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_color(sw, lv_color_hex(COLOR_ACCENT_LIGHT),
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_dir(sw, LV_GRAD_DIR_HOR,
                                 LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_INDICATOR);

    style_metal_knob(sw);
}

#define EDGE_W 1

/* The card draws no LVGL border of its own; this paints the whole edge so
 * nothing behind the card ever has to be repainted (the backdrop may be a
 * gradient, a scrim or another glass card). Three corners are arcs, the
 * lower-left is a straight chamfer meeting the edges r*(2-sqrt 2) from the
 * corner: the tangent of the arc it replaces. The only fill is the sliver
 * between the card's rounded background and the chamfer, at the card's own
 * opacity, so it blends exactly like the rest of the card. */
static void card_edge_draw_cb(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_color_t edge = *(lv_color_t *)lv_event_get_user_data(e);
    lv_coord_t r = lv_obj_get_style_radius(card, LV_PART_MAIN);
    if (r < 4) return;
    lv_coord_t d = (r * 150 + 128) / 256;  /* (2 - sqrt 2) * 256 = 150 */
    if (d < 4) d = 4;

    lv_coord_t x1 = card->coords.x1, y1 = card->coords.y1;
    lv_coord_t x2 = card->coords.x2, y2 = card->coords.y2;

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.border_width = 0;
    fill.radius = 0;
    fill.bg_opa = lv_obj_get_style_bg_opa(card, LV_PART_MAIN);
    fill.bg_color = lv_obj_get_style_bg_color(card, LV_PART_MAIN);
    if (fill.bg_opa > LV_OPA_MIN) {
        lv_coord_t cx = x1 + r, cy = y2 - r;
        lv_point_t sliver[9] = {{x1, y2 - r}, {x1, y2 - d}, {x1 + d, y2}, {x1 + r, y2}};
        /* back along the arc, 6 o'clock to 9 o'clock */
        static const int16_t angles[5] = {105, 120, 135, 150, 165};
        for (int i = 0; i < 5; i++) {
            sliver[4 + i].x = cx + ((lv_trigo_sin(angles[i] + 90) * r) >> LV_TRIGO_SHIFT);
            sliver[4 + i].y = cy + ((lv_trigo_sin(angles[i]) * r) >> LV_TRIGO_SHIFT);
        }
        lv_draw_polygon(ctx, &fill, sliver, 9);
    }

    fill.bg_opa = LV_OPA_COVER;
    fill.bg_color = edge;
    lv_area_t top = {x1 + r, y1, x2 - r, y1 + EDGE_W - 1};
    lv_area_t right = {x2 - EDGE_W + 1, y1 + r, x2, y2 - r};
    lv_area_t left = {x1, y1 + r, x1 + EDGE_W - 1, y2 - d};
    lv_area_t bottom = {x1 + d, y2 - EDGE_W + 1, x2 - r, y2};
    lv_draw_rect(ctx, &fill, &top);
    lv_draw_rect(ctx, &fill, &right);
    lv_draw_rect(ctx, &fill, &left);
    lv_draw_rect(ctx, &fill, &bottom);
    /* EDGE_W + 1 along each axis gives the 45-degree band a perpendicular
     * width just over EDGE_W, which antialiasing renders as the same weight */
    lv_point_t band[4] = {
        {x1, y2 - d}, {x1 + d, y2}, {x1 + d, y2 - EDGE_W - 1}, {x1 + EDGE_W + 1, y2 - d}
    };
    lv_draw_polygon(ctx, &fill, band, 4);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = edge;
    arc.width = EDGE_W;
    arc.opa = LV_OPA_COVER;
    arc.rounded = 0;
    lv_point_t tl = {x1 + r, y1 + r}, tr = {x2 - r, y1 + r}, br = {x2 - r, y2 - r};
    lv_draw_arc(ctx, &arc, &tl, r, 180, 270);
    lv_draw_arc(ctx, &arc, &tr, r, 270, 360);
    lv_draw_arc(ctx, &arc, &br, r, 0, 90);
}

void cygm_theme_glass_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(CYGM_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, CYGM_CARD_OPA, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(CYGM_EDGE), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, CYGM_EDGE_OPA, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

void cygm_theme_panel(lv_obj_t *obj)
{
    cygm_theme_glass_card(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(CYGM_PANEL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, 160, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
}

void cygm_theme_icon_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    /* No press highlight on bare icons; the action itself is the feedback.
     * The default theme also darkens pressed buttons through a colour filter. */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, CYGM_BTN_RADIUS, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, CYGM_BTN_EXT_CLICK);
}

void cygm_theme_glass_btn(lv_obj_t *btn)
{
    cygm_theme_glass_card(btn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(CYGM_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, 150, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, CYGM_BTN_RADIUS, LV_PART_MAIN);
    /* Press feedback is one explicit shade darker, never a colour change; the
     * default theme's pressed colour filter is switched off so it is not doubled */
    lv_obj_set_style_color_filter_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x182747), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_PRESSED);

    lv_coord_t ext = CYGM_BTN_EXT_CLICK;
    lv_coord_t w = lv_obj_get_width(btn), h = lv_obj_get_height(btn);
    if (w > 0 && w < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - w + 1) / 2;
        if (need > ext) ext = need;
    }
    if (h > 0 && h < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - h + 1) / 2;
        if (need > ext) ext = need;
    }
    lv_obj_set_ext_click_area(btn, ext);
}

void cygm_theme_primary_btn(lv_obj_t *btn)
{
    cygm_theme_glass_btn(btn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(COLOR_ACCENT_LIGHT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4ED8), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);
}

static void card_edge_delete_cb(lv_event_t *e)
{
    lv_mem_free(lv_event_get_user_data(e));
}

void cygm_theme_card_edge(lv_obj_t *card, lv_color_t color)
{
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);

    lv_color_t *keep = lv_mem_alloc(sizeof(lv_color_t));
    if (keep == NULL) return;
    *keep = color;
    lv_obj_add_event_cb(card, card_edge_draw_cb, LV_EVENT_DRAW_MAIN_END, keep);
    lv_obj_add_event_cb(card, card_edge_delete_cb, LV_EVENT_DELETE, keep);
}

lv_obj_t *cygm_theme_scr_act(void)
{
    lv_disp_t *d = lv_disp_get_default();
    if (d != NULL && d->scr_to_load != NULL) return d->scr_to_load;
    return lv_scr_act();
}

/* Complete an in-flight fade synchronously. lv_scr_load_anim's own handling of
 * this case (lv_disp.c) deletes lv_scr_act(), which after the fade's first
 * tick is the screen fading IN, so a second load within CYGM_SCR_FADE_MS
 * would free a live screen. Returns the screen that was deleted, if any, so
 * the caller can drop a stale pointer to it. */
static lv_obj_t *finish_pending_fade(void)
{
    lv_disp_t *d = lv_disp_get_default();
    if (d == NULL || d->scr_to_load == NULL) return NULL;

    lv_obj_t *incoming = d->scr_to_load;
    lv_obj_t *outgoing = d->prev_scr;
    if (outgoing == NULL && d->act_scr != incoming) outgoing = d->act_scr;

    lv_anim_del(incoming, NULL);
    if (outgoing != NULL) lv_anim_del(outgoing, NULL);
    lv_obj_remove_local_style_prop(incoming, LV_STYLE_OPA, 0);
    lv_obj_set_pos(incoming, 0, 0);

    lv_obj_t *deleted = NULL;
    if (outgoing != NULL && outgoing != incoming) {
        lv_event_send(outgoing, LV_EVENT_SCREEN_UNLOADED, NULL);
        if (d->del_prev) {
            lv_obj_del(outgoing);
            deleted = outgoing;
        }
    }
    d->act_scr = incoming;
    d->prev_scr = NULL;
    d->scr_to_load = NULL;
    d->draw_prev_over_act = 0;
    d->del_prev = 0;
    lv_event_send(incoming, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_invalidate(incoming);
    return deleted;
}

void cygm_theme_scr_load_now(lv_obj_t *scr)
{
    finish_pending_fade();
    lv_scr_load(scr);
}

void cygm_theme_scr_load(lv_obj_t *scr, lv_obj_t *old, lv_obj_t **old_slot)
{
    lv_obj_t *gone = finish_pending_fade();
    if (old != NULL && old == gone) old = NULL;

    if (old == scr) {
        if (old_slot != NULL) *old_slot = NULL;
        return;
    }

    bool delete_old = old != NULL;

#if CYGM_UI_ANIM
    bool old_is_active = delete_old && old == lv_scr_act();
    if (delete_old && !old_is_active) lv_obj_del(old);
    if (lv_scr_act() != scr) {
        lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, CYGM_SCR_FADE_MS, 0,
                         old_is_active);
    }
#else
    lv_scr_load(scr);
    if (delete_old) lv_obj_del(old);
#endif

    if (old_slot != NULL) *old_slot = NULL;
}
