# Living Home Screen — Design Spec

**Date:** 2026-04-04
**Baseline tag:** `Fully-Working-442026`

## Summary

Three ambient visual enhancements to the CYGM home screen that make it feel alive without demanding attention. All changes are additive — no existing layout surgery.

## Feature 1: Ambient Background Tint

The two card backgrounds (`left_card`, `right_card`) subtly shift color based on glucose zone and time of day.

### Glucose Zone Layer (Primary)

Uses the user's configured alarm thresholds to create a gradient tint:

| Zone | Condition | Tint Color |
|------|-----------|------------|
| Alarm Low | glucose ≤ low_alarm | Faint red |
| Warning Low | glucose between low_alarm and low_warning | Faint yellow (gradient from red→yellow) |
| In-Range | glucose between low_warning and high_warning | Faint green |
| Warning High | glucose between high_warning and high_alarm | Faint yellow (gradient from green→yellow) |
| Alarm High | glucose ≥ high_alarm | Faint red |

- **Baseline** is the midpoint between low_warning and high_warning — maximum green, minimum intensity.
- Color interpolates linearly between zone boundaries (not hard steps).
- Tint opacity: ~5-8% blended onto the card background color (`COLOR_CARD_BG`).
- If no valid glucose reading, fall back to neutral (no tint).

### Time-of-Day Layer (Secondary)

Uses sunrise/sunset from weather data (already fetched):

| Period | Tint |
|--------|------|
| Day (sunrise+30min to sunset-30min) | Neutral (no shift) |
| Golden hour (±30min around sunrise/sunset) | Very faint warm amber |
| Night (sunset+30min to sunrise-30min) | Very faint cool blue |

- Blended additively with glucose zone tint at ~3-5% opacity.
- If sunrise/sunset data unavailable, this layer is skipped.

### Implementation

- Computed once per minute (counter in the existing 1Hz `glucose_timer_update_cb`).
- Two `lv_obj_set_style_bg_color()` calls (left card, right card).
- Color math: linear interpolation in RGB space between zone boundary colors.
- ~20 bytes RAM (color vars + tick counter).

## Feature 2: Mini Sparkline

A small (~120×25px) canvas on the right card showing the last ~2 hours of glucose as a continuous line.

### Appearance

- Thin line (1-2px) connecting recent glucose points.
- Line color follows glucose zone palette (green in-range, yellow warning, red alarm) — same thresholds as Feature 1.
- Faint horizontal dashed line at high_warning and low_warning thresholds for context.
- No axes, no numbers, no labels — pure shape.
- Background transparent (card shows through).

### Data Source

- `glucose_history_get_range(120, points, 24)` — last 2 hours, max 24 points (5-min intervals).
- X-axis: time mapped linearly across canvas width.
- Y-axis: glucose value mapped to canvas height (range: low_alarm-20 to high_alarm+20, clamped).

### Position

- On the right card, between the trend arrow and the "time ago" label.
- Centered horizontally within the card.

### Update Frequency

- Redrawn only when a new glucose reading arrives (every ~5 min of new data).
- Also redrawn on `create_home_screen()`.

### Memory

- `lv_canvas` with `LV_IMG_CF_ALPHA_8BIT` or `LV_IMG_CF_TRUE_COLOR`: 120×25×2 = ~6KB buffer.
- Allocated once on home screen creation, freed on screen deletion.

## Feature 3: Freshness Arc (Replaces Progress Bar)

An `lv_arc` widget near the glucose number that shows reading age. Replaces the existing 3px glucose timer progress bar.

### Behavior

- **Full (100%)**: Reading is 0 minutes old (just arrived).
- **Empty (0%)**: Reading is ≥5 minutes old.
- Formula: `value = max(0, 100 - (seconds_since_reading × 100 / 300))`
- Updated every 1 second in the existing 1Hz timer.

### Fetch-in-Progress Indicator

- When a glucose fetch is actively happening, the arc color shifts to the accent blue (replacing the old progress bar's role).
- When fetch completes, arc color returns to the zone-appropriate color.

### Appearance

- Thin arc (3-4px wide), positioned as a partial ring below or around the glucose number.
- Arc color: matches glucose zone tint (green/yellow/red) at ~40% brightness — visible but not loud.
- Background track: very faint (10% opacity) version of the arc color.

### Memory

- `lv_arc` built-in widget: ~200 bytes.

## Files Modified

| File | Changes |
|------|---------|
| `main/ui/home_screen.c` | Add sparkline canvas, freshness arc, tint calculation functions, remove progress bar |
| `main/ui/home_screen.h` | Expose sparkline redraw function (called from glucose update) |
| `main/shared_state.h` | New globals: sparkline canvas, freshness arc, tint state |
| `main/main.c` | Update 1Hz timer callback for tint + arc; call sparkline redraw on glucose update |
| `main/ui_config.h` | New color constants for tint palette (zone greens, yellows, reds) |

## Constraints

- **No `lv_anim_start()`** — all updates via timer callbacks + direct property sets.
- **No shadows** — LVGL shadow rendering is unsafe on this device.
- **No `lv_label_set_recolor()`** — known freeze trigger.
- **LVGL lock**: All UI updates from background tasks must use `lvgl_port_lock(1)`.
- **Total memory budget**: ~6.5KB (sparkline canvas + arc + color vars). Well within ~42KB free heap.
