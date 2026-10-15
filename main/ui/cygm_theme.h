/*
 * cygm_theme.h - the 1.1.0 facelift: one place for the shared look.
 *
 * Every keyboard, slider, switch and screen transition goes through these
 * helpers so the look is defined once. Palette constants stay in
 * shared_state.h; this file owns how they are applied.
 *
 * Safety contract (see main.c:1175 and MISTAKES.md):
 *   - No continuous animation. lv_anim is used only for one-shot screen
 *     fades, and only when CYGM_UI_ANIM is 1.
 *   - No shadows anywhere (shadow_width 0 on every part these helpers touch).
 *   - Custom drawing runs after the widget part it decorates is drawn.
 *   - Callers hold the LVGL lock; nothing here takes it.
 */

#ifndef CYGM_THEME_H
#define CYGM_THEME_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen fade duration in ms when CYGM_UI_ANIM is on. */
#define CYGM_SCR_FADE_MS 120

/* Surface tokens. Deep navy field, cards a step lighter and nearly opaque so
 * white text keeps its contrast; glass is a hint, not a window. On RGB565 the
 * field's 2-stop ramp needs LV_DITHER_GRADIENT or it bands into four steps. */
#define CYGM_BG_TOP      0x101C3C
#define CYGM_BG_BOTTOM   0x070D20
#define CYGM_CARD_BG     0x1B2A4E
#define CYGM_CARD_OPA    235
#define CYGM_PANEL_BG    0x16244A   /* modals: fully opaque */
#define CYGM_BTN_BG      0x25386A
#define CYGM_EDGE        0x4B66A6
#define CYGM_EDGE_OPA    140

/* Paint a 2-stop vertical navy gradient (deep navy at the top, darker at the
 * bottom) on any object: screens, modal panels, the keyboard body. */
void cygm_theme_bg_gradient(lv_obj_t *obj);

/* Restyle an lv_keyboard: phone-style key map on all four modes (letters with
 * shift and backspace on row 3, "1#" / space / return on row 4), translucent
 * keys with a faint border on the gradient body, pressed key in accent blue.
 * Replaces the copy-pasted "glass" style blocks. Call right after
 * lv_keyboard_create(); the caller still sets the textarea and its own
 * event callbacks. */
void cygm_theme_keyboard(lv_obj_t *kb);

/* Restyle an lv_slider: recessed dark channel, accent gradient indicator,
 * round brushed-metal knob (light-to-dark grey gradient, thin border) with
 * a small darker dimple drawn at its centre. `accent` colours the indicator;
 * pass lv_color_hex(COLOR_ACCENT_BLUE) for the default. Keeps whatever size
 * the caller set. */
void cygm_theme_slider(lv_obj_t *slider, lv_color_t accent);

/* Restyle an lv_switch with the same metal knob and dimple; the track turns
 * accent blue when checked. */
void cygm_theme_switch(lv_obj_t *sw);

/* ---- The 1.1 glass look: every surface is one of these three ---- */

/* Translucent glass tile over the gradient: bg 0x506EB4 @ 60, 1 px border
 * 0x7896DC @ 90, radius 10, no shadow. Content padding is left to the caller. */
void cygm_theme_glass_card(lv_obj_t *obj);

/* Modal panel: the card tokens but fully opaque and radius 12, so a sheet,
 * picker or dialog reads as its own surface over the scrim and never bleeds
 * the screen beneath it. Every overlay panel uses this, not glass_card. */
void cygm_theme_panel(lv_obj_t *obj);

/* Icon-only button: no fill, no border, pressed shows a card-coloured pad.
 * For the cog, bulb, eye, refresh and any other bare-symbol control. */
void cygm_theme_icon_btn(lv_obj_t *btn);

/* Glass button: the tile above at bg opa 90 / border opa 110, radius
 * CYGM_BTN_RADIUS, pressed = accent blue; replaces the ghost outline. Grows the
 * touch area like cygm_apply_ghost_btn (call instead of it, never both). */
void cygm_theme_glass_btn(lv_obj_t *btn);

/* Primary action button: accent blue -> accent light horizontal gradient,
 * white text, radius CYGM_BTN_RADIUS, pressed = darker. For the one confirming
 * action on a screen (Connect, Sign in, OK, Install). */
void cygm_theme_primary_btn(lv_obj_t *btn);

/* Give a card its tier colour as a 2 px border all round instead of the old
 * 3 px bar on the left, with the lower-left corner cut as a straight chamfer
 * tangent to the radius the other three corners keep. Call after the card's
 * size, radius and background are set; the caller deletes its old bar object.
 * Works for any parent background, solid or vertical gradient. */
void cygm_theme_card_edge(lv_obj_t *card, lv_color_t color);

/* Load `scr` as the active screen. With CYGM_UI_ANIM it fades in over
 * CYGM_SCR_FADE_MS and the previous screen is deleted when the fade ends;
 * otherwise it is a plain lv_scr_load followed by lv_obj_del(old).
 * `old` may be NULL (nothing to free). After the call *old_slot, if given,
 * is set to NULL so the caller's global never dangles.
 * Never pass the screen that is already active as `scr`. */
void cygm_theme_scr_load(lv_obj_t *scr, lv_obj_t *old, lv_obj_t **old_slot);

/* Instant load with no fade, after completing any fade in progress. For the
 * shutdown path and other places where the screen must be up this tick. */
void cygm_theme_scr_load_now(lv_obj_t *scr);

/* The screen the user is looking at or about to see. During a fade
 * lv_scr_act() still returns the outgoing screen until the animation's first
 * tick, so overlays parented to it and "is home showing" checks would land on
 * a screen that is about to be deleted. Use this everywhere lv_scr_act() was. */
lv_obj_t *cygm_theme_scr_act(void);

#ifdef __cplusplus
}
#endif

#endif // CYGM_THEME_H
