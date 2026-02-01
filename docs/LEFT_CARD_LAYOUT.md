# Left Card Layout Reference

**File**: `main/main.c`
**Function**: `create_home_screen()` (starts at line ~1295)

## Card Dimensions
- **Size**: 150x220 pixels
- **Position**: x=6, y=10
- **Color**: Dark blue-grey (0x1a2332)

---

## Element Positions (Y-coordinates)

### Time Label (line 1324)
```c
lv_obj_align(label_clock, LV_ALIGN_TOP_MID, -10, 6);  // 6px from top, -10px X (left shift for AM/PM)
```
- Font: Montserrat 48pt
- Color: White
- Clickable: Tap to toggle 12hr/24hr format
- Format: Shows time only (AM/PM in separate label)

### AM/PM Label (line 1333) - 12hr mode only
```c
lv_obj_align(label_ampm, LV_ALIGN_TOP_RIGHT, 1, 30);  // Top right, 1px from right, 30px from top
```
- Font: Montserrat 12pt (smallest available)
- Color: Gray
- Hidden in 24hr mode
- Shows: "AM" or "PM"

### Date Label (line 1345)
```c
lv_obj_align(label_date, LV_ALIGN_TOP_MID, 0, 58);  // 58px from top
```
- Font: Montserrat 14pt
- Color: Gray

### Weather Icon Canvas (line 1352)
```c
lv_obj_align(home_weather_canvas, LV_ALIGN_CENTER, 0, -5);  // 5px above center
```
- Size: 60x50 pixels
- Format: LV_IMG_CF_TRUE_COLOR_ALPHA (supports transparency)
- Background: Fully transparent (LV_OPA_TRANSP + LV_OPA_0)
- Canvas object background set to transparent
- Canvas buffer filled with transparent
- **Alpha channel enabled for transparent icon backgrounds**

### Temperature Label (line 1371)
```c
lv_obj_align(home_temp_label, LV_ALIGN_CENTER, -20, 34);  // Center-20px X, +34px Y
```
- Font: Montserrat 20pt
- Color: White
- Shows: Current temp in F or C

### Hi/Lo Label (line 1382)
```c
lv_obj_align(home_hilo_label, LV_ALIGN_CENTER, 25, 34);  // Center+25px X, +34px Y
```
- Font: Montserrat 12pt
- Color: Gray
- Shows: "Hi XX°\nLo XX°"

### Condition Label (line 1395)
```c
lv_obj_align(home_condition_label, LV_ALIGN_CENTER, 0, 54);  // 54px below center
```
- Font: Montserrat 14pt
- Color: Gray
- Shows: "Sunny", "Foggy", etc.
- **Now updates with weather changes** (was previously static)

### WiFi Status Label (line 1396)
```c
lv_obj_align(label_wifi_status, LV_ALIGN_BOTTOM_MID, 0, -8);  // 8px from bottom
```
- Font: Montserrat 14pt
- Color: Dim gray / Green / Red (based on status)

---

## Weather Icons (8 total)

Defined in `draw_weather_icon()` function (line 166):

| Code | Name | Description |
|------|------|-------------|
| 0 | Sunny | Sun with 8 rays |
| 1 | Partly Cloudy | Sun + cloud |
| 2 | Cloudy | Cloud shape |
| 3 | Rain | Cloud + rain drops |
| 4 | Snow | Cloud + snowflakes |
| 5 | Storm | Dark cloud + lightning |
| 6 | Fog | Horizontal layers |
| 7 | Ice | Ice crystals |

**All icons have transparent backgrounds** (LV_OPA_0)

---

## Quick Adjustments

To move elements:
1. **Vertical spacing**: Adjust the Y value in `lv_obj_align()`
   - Negative = move up
   - Positive = move down
   - `LV_ALIGN_TOP_*` uses absolute position from top
   - `LV_ALIGN_CENTER` uses offset from center
   - `LV_ALIGN_BOTTOM_*` uses absolute position from bottom (negative values)

2. **Horizontal spacing**: Adjust the X value in `lv_obj_align()`
   - Negative = move left
   - Positive = move right

3. **Font sizes**: Change `&lv_font_montserrat_XX` (available: 12, 14, 20, 48)
