# Living Home Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three ambient visual features to the CYGM home screen — glucose zone background tint, mini sparkline, and freshness arc replacing the progress bar.

**Architecture:** All three features are additive UI changes in `home_screen.c` with shared color utility functions. The existing 1Hz LVGL timer drives tint and arc updates. Sparkline redraws on glucose data arrival. No new tasks, no new allocations beyond a ~6KB canvas buffer.

**Tech Stack:** ESP-IDF v5.5.2, LVGL v8.4, C, FreeRTOS

**Baseline tag:** `Fully-Working-442026`

---

### Task 1: Add Ambient Tint Color Utility Functions

Add helper functions to `home_screen.c` that compute a blended background color based on glucose zone (using alarm thresholds) and time of day (using sunrise/sunset). These are pure functions with no side effects — they take inputs and return a color.

**Files:**
- Modify: `main/ui/home_screen.c` (add static helper functions near top of file, after existing forward declarations around line 50)
- Modify: `main/shared_state.h` (add new tint color constants after line 97)

- [ ] **Step 1: Add tint color constants to shared_state.h**

Add after the existing `COLOR_BORDER` define at line 97:

```c
// Ambient tint colors (blended onto COLOR_CARD_BG at low opacity)
#define COLOR_TINT_GREEN    0x22C55E  // In-range zone
#define COLOR_TINT_YELLOW   0xFACC15  // Warning zone
#define COLOR_TINT_RED      0xEF4444  // Alarm zone
#define COLOR_TINT_AMBER    0xF59E0B  // Golden hour warmth
#define COLOR_TINT_COOL     0x3B82F6  // Night coolness
```

- [ ] **Step 2: Add color interpolation helper to home_screen.c**

Add after the existing `anim_opa_cb` function (around line 59), before the no-data overlay section:

```c
// ==================== Ambient Tint Helpers ====================

// Linear interpolation between two colors. t=0 returns c1, t=255 returns c2.
// Uses lv_color_mix which handles RGB565 format correctly.
static lv_color_t color_lerp(lv_color_t c1, lv_color_t c2, uint8_t t) {
    // lv_color_mix(c1, c2, mix) — mix=255 returns c1, mix=0 returns c2
    return lv_color_mix(c2, c1, t);
}

// Blend a tint color onto the card background at a given intensity (0-255).
// intensity=0 returns pure card bg, intensity=255 returns pure tint.
static lv_color_t blend_tint(uint32_t tint_hex, uint8_t intensity) {
    return color_lerp(lv_color_hex(COLOR_CARD_BG), lv_color_hex(tint_hex), intensity);
}

// Compute glucose zone tint color based on current glucose and alarm thresholds.
// Returns the blended card background color.
static lv_color_t compute_glucose_zone_tint(int glucose) {
    int low_alarm_val   = current_alarm_settings.low_alarm.threshold;
    int low_warn_val    = current_alarm_settings.low_warning.threshold;
    int high_warn_val   = current_alarm_settings.high_warning.threshold;
    int high_alarm_val  = current_alarm_settings.high_alarm.threshold;

    // Tint intensity: 0 = no tint (pure card bg), 18 = max subtle tint (~7%)
    const uint8_t MAX_TINT = 18;

    if (glucose <= low_alarm_val) {
        // At or below low alarm: full red tint
        return blend_tint(COLOR_TINT_RED, MAX_TINT);
    } else if (glucose <= low_warn_val) {
        // Between low alarm and low warning: red→yellow gradient
        int range = low_warn_val - low_alarm_val;
        if (range <= 0) return blend_tint(COLOR_TINT_YELLOW, MAX_TINT);
        uint8_t t = (uint8_t)((glucose - low_alarm_val) * 255 / range);
        lv_color_t zone_color = color_lerp(lv_color_hex(COLOR_TINT_RED), lv_color_hex(COLOR_TINT_YELLOW), t);
        return color_lerp(lv_color_hex(COLOR_CARD_BG), zone_color, MAX_TINT);
    } else if (glucose < high_warn_val) {
        // In range: green tint, weakest at midpoint
        int midpoint = (low_warn_val + high_warn_val) / 2;
        int half_range = (high_warn_val - low_warn_val) / 2;
        if (half_range <= 0) return blend_tint(COLOR_TINT_GREEN, MAX_TINT / 2);
        int dist = abs(glucose - midpoint);
        // Closer to midpoint = less tint (more calm), closer to edge = more tint
        uint8_t edge_factor = (uint8_t)(dist * MAX_TINT / half_range);
        uint8_t intensity = MAX_TINT / 3 + edge_factor * 2 / 3;
        if (intensity > MAX_TINT) intensity = MAX_TINT;
        return blend_tint(COLOR_TINT_GREEN, intensity);
    } else if (glucose < high_alarm_val) {
        // Between high warning and high alarm: yellow→red gradient
        int range = high_alarm_val - high_warn_val;
        if (range <= 0) return blend_tint(COLOR_TINT_YELLOW, MAX_TINT);
        uint8_t t = (uint8_t)((glucose - high_warn_val) * 255 / range);
        lv_color_t zone_color = color_lerp(lv_color_hex(COLOR_TINT_YELLOW), lv_color_hex(COLOR_TINT_RED), t);
        return color_lerp(lv_color_hex(COLOR_CARD_BG), zone_color, MAX_TINT);
    } else {
        // At or above high alarm: full red tint
        return blend_tint(COLOR_TINT_RED, MAX_TINT);
    }
}

// Compute time-of-day tint adjustment. Returns a color to blend additively.
// Uses sunrise_time and sunset_time globals from weather_system.
static lv_color_t compute_time_of_day_tint(lv_color_t base) {
    time_t now;
    time(&now);

    // If sunrise/sunset not available, return base unchanged
    if (sunrise_time == 0 || sunset_time == 0) return base;

    const int GOLDEN_MINUTES = 30;
    const uint8_t TOD_TINT = 8;  // ~3% overlay

    double diff_sunrise = difftime(now, sunrise_time) / 60.0;  // minutes from sunrise
    double diff_sunset  = difftime(now, sunset_time) / 60.0;   // minutes from sunset

    // Golden hour around sunrise (±30 min)
    if (diff_sunrise >= -GOLDEN_MINUTES && diff_sunrise <= GOLDEN_MINUTES) {
        uint8_t intensity = (uint8_t)(TOD_TINT * (GOLDEN_MINUTES - abs((int)diff_sunrise)) / GOLDEN_MINUTES);
        return color_lerp(base, lv_color_hex(COLOR_TINT_AMBER), intensity);
    }
    // Golden hour around sunset (±30 min)
    if (diff_sunset >= -GOLDEN_MINUTES && diff_sunset <= GOLDEN_MINUTES) {
        uint8_t intensity = (uint8_t)(TOD_TINT * (GOLDEN_MINUTES - abs((int)diff_sunset)) / GOLDEN_MINUTES);
        return color_lerp(base, lv_color_hex(COLOR_TINT_AMBER), intensity);
    }
    // Night (after sunset+30 or before sunrise-30)
    if (diff_sunset > GOLDEN_MINUTES || diff_sunrise < -GOLDEN_MINUTES) {
        return color_lerp(base, lv_color_hex(COLOR_TINT_COOL), TOD_TINT);
    }

    // Daytime — no change
    return base;
}

// Combined: compute final card tint from glucose + time of day
static lv_color_t compute_ambient_tint(int glucose) {
    lv_color_t zone_tint = compute_glucose_zone_tint(glucose);
    return compute_time_of_day_tint(zone_tint);
}
```

- [ ] **Step 3: Verify build compiles**

User runs: `idf.py build`
Expected: Clean compile, no errors. The functions are static and unused so far — no warnings expected since they'll be referenced in Task 4.

- [ ] **Step 4: Commit**

```
git add main/ui/home_screen.c main/shared_state.h
git commit -m "Feature: Add ambient tint color utility functions

Adds color_lerp, blend_tint, compute_glucose_zone_tint,
compute_time_of_day_tint, and compute_ambient_tint helper
functions. Glucose zone uses user alarm thresholds with smooth
gradient transitions. Time-of-day layer uses sunrise/sunset data."
```

---

### Task 2: Wire Ambient Tint Into the 1Hz Timer

Hook the tint computation into the existing `glucose_timer_update_cb` so card backgrounds update once per minute.

**Files:**
- Modify: `main/ui/home_screen.c:688-759` (the `glucose_timer_update_cb` function)

- [ ] **Step 1: Add tint update to the 1Hz timer callback**

In `glucose_timer_update_cb` (line 688), add a tint update block right after the progress bar update (line 706) and before the heartbeat colon toggle (line 708). Add a static counter so tint only recomputes every 60 ticks (once per minute):

```c
    // Update bar
    lv_bar_set_value(glucose_fetch_timer_bar, percentage, LV_ANIM_OFF);

    // Ambient tint: recompute once per minute (every 60 ticks of 1Hz timer)
    {
        static uint8_t tint_counter = 0;
        tint_counter++;
        if (tint_counter >= 60) {
            tint_counter = 0;
            if (glucose_data_valid && current_glucose > 0) {
                lv_color_t tint = compute_ambient_tint(current_glucose);
                if (left_card != NULL) {
                    lv_obj_set_style_bg_color(left_card, tint, 0);
                }
                if (right_card != NULL) {
                    lv_obj_set_style_bg_color(right_card, tint, 0);
                }
            }
        }
    }

    // Heartbeat: toggle colon visibility every second ...
```

- [ ] **Step 2: Add immediate tint update in update_glucose_display**

In `main.c`, in the `update_glucose_display()` function, after the glucose value label is updated and the LVGL lock is held, add:

Find the section that sets the glucose label text and color (inside the `if (glucose_data_valid)` block), and add after the trend arrow draw:

```c
    // Update ambient card tint immediately on new reading
    if (left_card != NULL && right_card != NULL && current_glucose > 0) {
        lv_color_t tint = compute_ambient_tint(current_glucose);
        lv_obj_set_style_bg_color(left_card, tint, 0);
        lv_obj_set_style_bg_color(right_card, tint, 0);
    }
```

Note: `compute_ambient_tint` is a static function in `home_screen.c`. Since `update_glucose_display` is in `main.c`, we need to either:
- (a) Make it non-static and declare it in `home_screen.h`, OR
- (b) Create a wrapper function `update_ambient_tint(void)` in `home_screen.c` that reads globals and applies the tint, exposed via `home_screen.h`.

Option (b) is cleaner. Add to `home_screen.c`:

```c
// Public: update ambient tint on both cards (call after glucose update)
void update_ambient_tint(void) {
    if (!glucose_data_valid || current_glucose <= 0) return;
    if (left_card == NULL || right_card == NULL) return;
    lv_color_t tint = compute_ambient_tint(current_glucose);
    lv_obj_set_style_bg_color(left_card, tint, 0);
    lv_obj_set_style_bg_color(right_card, tint, 0);
}
```

Add to `home_screen.h`:

```c
// Update ambient background tint on home screen cards
void update_ambient_tint(void);
```

Then in `main.c` `update_glucose_display()`, after the trend arrow is drawn (inside the LVGL lock section), add:

```c
    update_ambient_tint();
```

- [ ] **Step 3: Verify build compiles**

User runs: `idf.py build`
Expected: Clean compile.

- [ ] **Step 4: Flash and verify**

User runs: `idf.py -p COMx flash monitor`
Expected: Cards should show a very subtle green tint when glucose is in-range. The tint should shift toward yellow/red if glucose approaches warning/alarm thresholds. After sunset, a very faint cool blue overlay should be visible.

- [ ] **Step 5: Commit**

```
git add main/ui/home_screen.c main/ui/home_screen.h main/main.c
git commit -m "Feature: Wire ambient background tint into home screen

Cards subtly shift color based on glucose zone (alarm thresholds)
and time of day (sunrise/sunset). Updates once per minute via the
1Hz timer, plus immediately on new glucose readings."
```

---

### Task 3: Add Freshness Arc (Replace Progress Bar)

Replace `glucose_fetch_timer_bar` (the 3px progress bar at the bottom of the right card) with an `lv_arc` that shows reading age (0-5 min).

**Files:**
- Modify: `main/shared_state.h` (replace `glucose_fetch_timer_bar` global with `glucose_freshness_arc`)
- Modify: `main/ui/home_screen.c` (replace bar creation with arc, update timer callback, update expand/collapse)
- Modify: `main/main.c` (replace all `glucose_fetch_timer_bar` references with arc equivalents)

- [ ] **Step 1: Replace the global in shared_state.h**

Find line 264:
```c
extern lv_obj_t *glucose_fetch_timer_bar;  // Progress bar showing time until next glucose fetch
```
Replace with:
```c
extern lv_obj_t *glucose_freshness_arc;    // Arc showing glucose reading age (0-5 min)
extern bool glucose_fetch_active;          // True while a glucose fetch is in progress
```

- [ ] **Step 2: Replace the global definition in main.c**

Find line 283:
```c
lv_obj_t *glucose_fetch_timer_bar = NULL;  // Progress bar showing time until next glucose fetch
```
Replace with:
```c
lv_obj_t *glucose_freshness_arc = NULL;    // Arc showing glucose reading age (0-5 min)
bool glucose_fetch_active = false;         // True while a glucose fetch is in progress
```

- [ ] **Step 3: Replace bar creation with arc in home_screen.c create_home_screen()**

Find lines 1591-1600 (the progress bar creation block):
```c
    // Glucose fetch countdown bar (subtle indicator at bottom of card)
    glucose_fetch_timer_bar = lv_bar_create(right_card);
    lv_obj_set_size(glucose_fetch_timer_bar, 100, 3);
    lv_obj_align(glucose_fetch_timer_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(glucose_fetch_timer_bar, 0, 100);
    lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(glucose_fetch_timer_bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(glucose_fetch_timer_bar, 1, 0);
```

Replace with:
```c
    // Freshness arc: shows glucose reading age (full=fresh, empty=5min old)
    glucose_freshness_arc = lv_arc_create(right_card);
    lv_obj_set_size(glucose_freshness_arc, 100, 100);
    lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 14);
    lv_arc_set_rotation(glucose_freshness_arc, 200);     // Start from bottom-left
    lv_arc_set_bg_angles(glucose_freshness_arc, 0, 140); // 140° sweep
    lv_arc_set_range(glucose_freshness_arc, 0, 100);
    lv_arc_set_value(glucose_freshness_arc, 100);
    lv_obj_remove_style(glucose_freshness_arc, NULL, LV_PART_KNOB);  // No knob
    lv_obj_clear_flag(glucose_freshness_arc, LV_OBJ_FLAG_CLICKABLE); // Not interactive

    // Arc indicator (the filled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);

    // Arc background track (the unfilled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_10, LV_PART_MAIN);
```

- [ ] **Step 4: Replace timer callback progress bar logic with arc logic**

In `glucose_timer_update_cb` (home_screen.c), replace the entire progress bar section (lines 688-706) with freshness arc logic. The function signature and guard stay the same:

```c
static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_freshness_arc == NULL) {
        return;
    }

    // Freshness arc: depletes from 100→0 over 300 seconds (5 minutes)
    if (glucose_timestamp > 0) {
        time_t now;
        time(&now);
        int seconds_old = (int)difftime(now, glucose_timestamp);
        int arc_value = 100 - (seconds_old * 100 / 300);
        if (arc_value < 0) arc_value = 0;
        if (arc_value > 100) arc_value = 100;
        lv_arc_set_value(glucose_freshness_arc, arc_value);
    }

    // Fetch-in-progress: shift arc color to accent blue
    if (glucose_fetch_active) {
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    } else if (glucose_data_valid && current_glucose > 0) {
        // Color matches glucose zone
        int glu = current_glucose;
        int low_a = current_alarm_settings.low_alarm.threshold;
        int low_w = current_alarm_settings.low_warning.threshold;
        int high_w = current_alarm_settings.high_warning.threshold;
        int high_a = current_alarm_settings.high_alarm.threshold;
        uint32_t arc_color;
        if (glu <= low_a || glu >= high_a) arc_color = COLOR_RED;
        else if (glu <= low_w || glu >= high_w) arc_color = COLOR_YELLOW;
        else arc_color = COLOR_GREEN;
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);
    }

    // Ambient tint: recompute once per minute ...
```

Keep the rest of the function (ambient tint block, heartbeat colon, watchdog) unchanged.

- [ ] **Step 5: Update expand/collapse to reference the arc**

In `expand_cgm_card()`, find:
```c
    if (glucose_fetch_timer_bar != NULL) {
        lv_obj_set_width(glucose_fetch_timer_bar, 260);
    }
```
Replace with:
```c
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, 200, 200);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 40);
    }
```

In `collapse_cgm_card()`, find:
```c
    if (glucose_fetch_timer_bar != NULL) {
        lv_obj_set_width(glucose_fetch_timer_bar, 100);
    }
```
Replace with:
```c
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, 100, 100);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 14);
    }
```

- [ ] **Step 6: Update all glucose_fetch_timer_bar references in main.c**

Search for all `glucose_fetch_timer_bar` references in `main.c`. These are in the glucose update task where fetch results are processed. Each one sets a bar color and resets the bar value. Replace these with `glucose_fetch_active` flag sets and arc value resets:

For **successful fetch** (around line 2130):
Replace:
```c
if (glucose_fetch_timer_bar != NULL) {
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
}
```
With:
```c
if (glucose_freshness_arc != NULL) {
    lv_arc_set_value(glucose_freshness_arc, 100);  // Fresh reading
}
```

For **duplicate reading** (around line 2207):
Replace:
```c
if (glucose_fetch_timer_bar != NULL) {
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
    lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
}
```
With:
```c
// Duplicate: arc stays at current value (reading age unchanged)
```

For **error/failure** (around line 2273):
Replace:
```c
if (glucose_fetch_timer_bar != NULL) {
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
    lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_shadow_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
}
```
With:
```c
// Error: arc color will be updated by timer callback
```

Also set `glucose_fetch_active = true` before the fetch starts and `glucose_fetch_active = false` after it completes (in both success and error paths). Find the line where the fetch begins (around `DEXCOM: Fetching glucose data`) and set it there.

- [ ] **Step 7: Update wifi_screens.c reference if any**

Check `main/ui/wifi_screens.c` for `glucose_fetch_timer_bar` references and remove/update them.

- [ ] **Step 8: Verify build compiles**

User runs: `idf.py build`
Expected: Clean compile, no references to the old `glucose_fetch_timer_bar` symbol.

- [ ] **Step 9: Flash and verify**

User runs: `idf.py -p COMx flash monitor`
Expected: Bottom of right card shows a thin arc that starts full on fresh reading and gradually depletes over 5 minutes. Arc color matches glucose zone (green in-range). During fetch, arc turns blue briefly.

- [ ] **Step 10: Commit**

```
git add main/shared_state.h main/ui/home_screen.c main/main.c main/ui/wifi_screens.c main/tasks/background_tasks.c
git commit -m "Feature: Replace progress bar with freshness arc

Glucose reading age shown as a depleting arc (full=fresh, empty=5min).
Arc color follows glucose zone. Turns blue during active fetch.
Replaces the old 3px countdown bar."
```

---

### Task 4: Add Mini Sparkline Canvas

Add a small canvas to the right card that draws the last 2 hours of glucose readings as a connected line.

**Files:**
- Modify: `main/ui/home_screen.c` (add canvas creation + draw function)
- Modify: `main/ui/home_screen.h` (expose `redraw_sparkline` for main.c to call)
- Modify: `main/shared_state.h` (add `sparkline_canvas` global)
- Modify: `main/main.c` (call `redraw_sparkline` after glucose update)

- [ ] **Step 1: Add sparkline global to shared_state.h**

Add near the other home screen widget globals (around line 264):

```c
extern lv_obj_t *sparkline_canvas;         // Mini sparkline on right card
```

- [ ] **Step 2: Add sparkline global definition to main.c**

Add near the other widget definitions (around line 283):

```c
lv_obj_t *sparkline_canvas = NULL;         // Mini sparkline on right card
```

- [ ] **Step 3: Add sparkline canvas buffer and draw function to home_screen.c**

Add after the `compute_ambient_tint` function (end of the tint helpers section):

```c
// ==================== Mini Sparkline ====================

#define SPARKLINE_W 120
#define SPARKLINE_H 25

static lv_color_t sparkline_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(SPARKLINE_W, SPARKLINE_H)];

// Draw the sparkline — called on new glucose readings and screen create
void redraw_sparkline(void) {
    if (sparkline_canvas == NULL) return;

    // Clear canvas to transparent (card shows through)
    lv_canvas_fill_bg(sparkline_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_TRANSP);

    // Get last 2 hours of data
    glucose_history_point_t points[24];
    int count = glucose_history_get_range(120, points, 24);
    if (count < 2) return;  // Need at least 2 points to draw a line

    // Determine Y-axis range from alarm thresholds (with padding)
    int y_min = current_alarm_settings.low_alarm.threshold - 20;
    int y_max = current_alarm_settings.high_alarm.threshold + 20;
    if (y_min < 40) y_min = 40;
    if (y_max > 400) y_max = 400;
    int y_range = y_max - y_min;
    if (y_range <= 0) y_range = 1;

    // Alarm threshold lines as Y pixel positions
    int low_warn_y  = SPARKLINE_H - 1 - ((current_alarm_settings.low_warning.threshold - y_min) * (SPARKLINE_H - 1) / y_range);
    int high_warn_y = SPARKLINE_H - 1 - ((current_alarm_settings.high_warning.threshold - y_min) * (SPARKLINE_H - 1) / y_range);

    // Draw faint dashed threshold lines
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(COLOR_TEXT_DIM);
    line_dsc.opa = LV_OPA_30;
    line_dsc.width = 1;

    // Low warning threshold dashes
    if (low_warn_y >= 0 && low_warn_y < SPARKLINE_H) {
        for (int dx = 0; dx < SPARKLINE_W; dx += 6) {
            lv_point_t dash_pts[2] = {{dx, low_warn_y}, {dx + 3 < SPARKLINE_W ? dx + 3 : SPARKLINE_W - 1, low_warn_y}};
            lv_canvas_draw_line(sparkline_canvas, dash_pts, 2, &line_dsc);
        }
    }
    // High warning threshold dashes
    if (high_warn_y >= 0 && high_warn_y < SPARKLINE_H) {
        for (int dx = 0; dx < SPARKLINE_W; dx += 6) {
            lv_point_t dash_pts[2] = {{dx, high_warn_y}, {dx + 3 < SPARKLINE_W ? dx + 3 : SPARKLINE_W - 1, high_warn_y}};
            lv_canvas_draw_line(sparkline_canvas, dash_pts, 2, &line_dsc);
        }
    }

    // Map points to pixel coordinates
    // X: evenly spaced across canvas width
    // Y: glucose mapped to canvas height (inverted: high glucose = top)
    lv_draw_line_dsc_t glu_line_dsc;
    lv_draw_line_dsc_init(&glu_line_dsc);
    glu_line_dsc.width = 2;

    for (int i = 0; i < count - 1; i++) {
        if (!points[i].valid || !points[i + 1].valid) continue;

        int x1 = i * (SPARKLINE_W - 1) / (count - 1);
        int x2 = (i + 1) * (SPARKLINE_W - 1) / (count - 1);

        int g1 = points[i].glucose;
        int g2 = points[i + 1].glucose;

        // Clamp to range
        if (g1 < y_min) g1 = y_min; if (g1 > y_max) g1 = y_max;
        if (g2 < y_min) g2 = y_min; if (g2 > y_max) g2 = y_max;

        int y1 = SPARKLINE_H - 1 - ((g1 - y_min) * (SPARKLINE_H - 1) / y_range);
        int y2 = SPARKLINE_H - 1 - ((g2 - y_min) * (SPARKLINE_H - 1) / y_range);

        // Color segment by midpoint glucose
        int mid_glu = (points[i].glucose + points[i + 1].glucose) / 2;
        int low_a  = current_alarm_settings.low_alarm.threshold;
        int low_w  = current_alarm_settings.low_warning.threshold;
        int high_w = current_alarm_settings.high_warning.threshold;
        int high_a = current_alarm_settings.high_alarm.threshold;

        if (mid_glu <= low_a || mid_glu >= high_a) {
            glu_line_dsc.color = lv_color_hex(COLOR_RED);
        } else if (mid_glu <= low_w || mid_glu >= high_w) {
            glu_line_dsc.color = lv_color_hex(COLOR_YELLOW);
        } else {
            glu_line_dsc.color = lv_color_hex(COLOR_GREEN);
        }
        glu_line_dsc.opa = LV_OPA_80;

        lv_point_t seg[2] = {{x1, y1}, {x2, y2}};
        lv_canvas_draw_line(sparkline_canvas, seg, 2, &glu_line_dsc);
    }
}
```

- [ ] **Step 4: Add sparkline canvas creation in create_home_screen()**

Add right after the `label_time_ago` creation (line 1589) and before the freshness arc creation:

```c
    // Mini sparkline: last 2 hours of glucose readings
    sparkline_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(sparkline_canvas, sparkline_buf, SPARKLINE_W, SPARKLINE_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(sparkline_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_TRANSP);
    lv_obj_align(sparkline_canvas, LV_ALIGN_BOTTOM_MID, 0, -30);
    redraw_sparkline();  // Initial draw from loaded history
```

- [ ] **Step 5: Handle sparkline in expand/collapse**

In `expand_cgm_card()`, add after the freshness arc resize:
```c
    if (sparkline_canvas != NULL) {
        lv_obj_add_flag(sparkline_canvas, LV_OBJ_FLAG_HIDDEN);  // Hide in expanded mode
    }
```

In `collapse_cgm_card()`, add after the freshness arc restore:
```c
    if (sparkline_canvas != NULL) {
        lv_obj_clear_flag(sparkline_canvas, LV_OBJ_FLAG_HIDDEN);  // Show in collapsed mode
        lv_obj_align(sparkline_canvas, LV_ALIGN_BOTTOM_MID, 0, -30);
    }
```

- [ ] **Step 6: Declare redraw_sparkline in home_screen.h**

Add to `home_screen.h`:

```c
// Redraw the mini sparkline on the home screen (call after new glucose data)
void redraw_sparkline(void);
```

- [ ] **Step 7: Call redraw_sparkline from main.c on glucose update**

In `main.c`, in the glucose update success path (around line 2138, after `update_glucose_display()` is called), add:

```c
    redraw_sparkline();
```

This should be called inside the LVGL lock section, or with its own `lvgl_port_lock(1)` check.

- [ ] **Step 8: Verify build compiles**

User runs: `idf.py build`
Expected: Clean compile.

- [ ] **Step 9: Flash and verify**

User runs: `idf.py -p COMx flash monitor`
Expected: A small line chart appears on the right card between the trend arrow and the "time ago" text. The line shows recent glucose history with zone coloring. Faint dashed lines mark the warning thresholds. The sparkline hides when the CGM card is expanded.

- [ ] **Step 10: Commit**

```
git add main/ui/home_screen.c main/ui/home_screen.h main/shared_state.h main/main.c
git commit -m "Feature: Add mini sparkline to home screen

Shows last 2 hours of glucose as a small line chart on the right card.
Color-coded by glucose zone. Faint dashed threshold lines at warning
levels. Hidden in expanded mode. Redrawn on new glucose readings."
```

---

### Task 5: Integration Testing and Polish

Verify all three features work together, check memory impact, and adjust any visual tuning.

**Files:**
- Potentially tweak: `main/ui/home_screen.c` (spacing, opacity values, arc size)

- [ ] **Step 1: Verify memory impact**

After flashing, watch the serial log for the `MEM` lines during glucose fetches. Compare against baseline:
- Baseline: `free=42848 largest=32768 frag=24%`
- Expected: free heap should be ~36-37KB (6KB less due to sparkline canvas)
- `largest_block` should remain ≥32KB
- If `largest_block` drops below 30KB, reduce sparkline size to 100x20

- [ ] **Step 2: Verify no freeze or watchdog issues**

Let the device run for 10+ minutes. Watch for:
- No `LVGL lock timeout` increases (occasional is OK, as before)
- No task watchdog triggers
- No TLS/SSL failures
- Glucose fetches still succeed normally

- [ ] **Step 3: Visual tuning**

Check on the physical device:
- Is the ambient tint visible but subtle? If too strong, reduce `MAX_TINT` from 18 to 12.
- Is the sparkline readable? If too cramped, increase height from 25 to 30.
- Is the freshness arc noticeable? If too faint, increase indicator opacity from `LV_OPA_70` to `LV_OPA_90`.
- Does the arc position look good relative to the glucose number?
- Does the sparkline position between trend arrow and time-ago look balanced?

Apply any tweaks.

- [ ] **Step 4: Final commit**

```
git add -A
git commit -m "Style: Living home screen visual tuning

Adjust ambient tint intensity, sparkline dimensions, and freshness
arc opacity based on on-device testing."
```
