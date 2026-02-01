# CGM Account Status Screen - Design Pattern

## Overview
All CGM implementations (Dexcom, LibreLinkUp, Nightscout) share the **SAME account status screen layout**. This minimizes code duplication and provides a consistent user experience across different CGM sources.

## Screen Layout (320x240)

**Vertical Layout Budget:**
- Header: Y=0-42 (42px)
- Status Card: Y=45-115 (70px)
- Info Section: Y=125-185 (60px, 2 fields @ 30px each)
- Logout Button: Y=197-235 (38px)

```
┌─────────────────────────────────────┐
│ [←]    Dexcom Account            │  ← Header (Y=10, height 20)
│                                     │
│  ╔═══════════════════════════════╗ │
│  ║  ✓   Connected                ║ │  ← Status card (Y=45, height 70)
│  ╚═══════════════════════════════╝ │
│                                     │
│  CGM Type                           │  ← Info field 1 (Y=125)
│  Dexcom Share                       │     (Label Y=125, Value Y=141)
│                                     │
│  Account                            │  ← Info field 2 (Y=160)
│  username@example.com               │     (Label Y=160, Value Y=176)
│                                     │
│        [ Logout ]                   │  ← Logout button (Y=197, 160×38)
└─────────────────────────────────────┘
```

**No Overlapping**: All elements have clear vertical boundaries with spacing.

### Detailed Pixel Layout

```
Y=0   ┌─────────────────────────────────────┐
      │                                     │
Y=5   │ [←] Back button (36×36)            │
Y=10  │         Dexcom Account              │  ← Header text
      │                                     │
Y=42  ├─────────────────────────────────────┤  ← Header ends
      │                                     │
Y=45  │  ╔═══════════════════════════════╗ │  ← Status card starts
      │  ║                                ║ │
      │  ║  ✓   Connected                 ║ │
      │  ║                                ║ │
Y=115 │  ╚═══════════════════════════════╝ │  ← Status card ends
      │                                     │
Y=125 │  CGM Type                           │  ← Info field 1 starts
Y=141 │  Dexcom Share                       │
      │                                     │
Y=160 │  Account                            │  ← Info field 2 starts
Y=176 │  username@example.com               │
Y=192 │                                     │  ← Info section ends
      │                                     │
Y=197 │        [ Logout ]                   │  ← Logout button (160×38)
Y=235 │                                     │  ← Button ends
Y=240 └─────────────────────────────────────┘  ← Screen ends
```

**Gap Analysis:**
- Gap between header and status card: 3px (42→45)
- Gap between status card and info: 10px (115→125)
- Gap between info and logout: 5px (192→197)
- Gap between logout and screen edge: 5px (235→240)

## Implementation Guidelines

### 1. Screen Creation Function

Each CGM type should have its own screen creation function, but they all follow this same layout:

- **Dexcom**: `create_dexcom_status_screen()` (line 4155 in main.c)
- **Libre**: `create_libre_status_screen()` (future)
- **Nightscout**: `create_nightscout_status_screen()` (future)

### 2. Header Title (Only Thing That Changes)

The **only visual difference** between CGM status screens is the header text:

| CGM Type | Header Text |
|----------|-------------|
| Dexcom | "Dexcom Account" |
| Libre | "Libre Account" |
| Nightscout | "Nightscout" |

**Code:**
```c
lv_obj_t *header = lv_label_create(screen);
lv_label_set_text(header, "Dexcom Account");  // ← Change this line only
lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);
```

### 3. Layout Constants

**Screen dimensions**: 320x240

**Header**:
- Y position: 10px from top
- Font: Montserrat 20
- Alignment: TOP_MID

**Back Button**:
- Size: 36x36
- Position: TOP_LEFT (5, 5)
- Symbol: LV_SYMBOL_LEFT

**Status Card**:
- Size: 280x70
- Position: TOP_MID (0, 45)
- Background: Dark green (#1a3d1a) for connected state
- Icon: LV_SYMBOL_OK (28pt) at LEFT_MID, X=18
- Text: "Connected" (18pt) at CENTER, X=12

**Info Section**:
- Field 1 (CGM Type):
  - Label Y: 125px
  - Value Y: 141px (16px below label)
- Field 2 (Account):
  - Label Y: 160px
  - Value Y: 176px (16px below label)
- Left margin: 25px for all
- Label font: Montserrat 12 (gray #808080)
- Value font: Montserrat 16 (white #FFFFFF)

**Logout Button**:
- Size: 160x38 (smaller, more compact)
- Position: TOP_MID (0, 197) — absolute positioning
- Background: Red (#FF3333)
- Font: Montserrat 16

### 4. Information Fields

All CGM types display these two fields:

**Field 1: CGM Type**
- Label: "CGM Type"
- Value examples:
  - Dexcom: "Dexcom Share"
  - Libre: "LibreLinkUp"
  - Nightscout: "Nightscout"

**Field 2: Account/Identifier**
- Label: "Account"
- Value examples:
  - Dexcom: username
  - Libre: email address
  - Nightscout: server URL

### 5. Event Handlers

Each CGM needs its own event handlers for back button and logout:

| CGM Type | Back Handler | Logout Handler |
|----------|--------------|----------------|
| Dexcom | `dexcom_back_event_cb` | `dexcom_logout_btn_event_cb` |
| Libre | `libre_back_event_cb` | `libre_logout_btn_event_cb` |
| Nightscout | `nightscout_back_event_cb` | `nightscout_logout_btn_event_cb` |

**Back Handler Logic** (same for all):
```c
static void cgm_back_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Back from CGM status screen");
    lv_scr_load_anim(screen_cgm_settings, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}
```

**Logout Handler Logic** (CGM-specific):
```c
static void cgm_logout_btn_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Logout from CGM");

    // Clear credentials from NVS
    nvs_clear_cgm_credentials();  // CGM-specific function

    // Clear local buffers
    memset(cgm_username_buf, 0, sizeof(cgm_username_buf));
    memset(cgm_password_buf, 0, sizeof(cgm_password_buf));

    // Mark as not authenticated
    cgm_api_set_authenticated(false);  // CGM-specific function

    ESP_LOGI(TAG, "CGM credentials cleared");

    // Go back to CGM settings screen
    lv_scr_load_anim(screen_cgm_settings, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
}
```

---

## Implementation Checklist for New CGM Types

When adding a new CGM source (e.g., LibreLinkUp), follow these steps:

### Step 1: Create Screen Variable
```c
static lv_obj_t *screen_libre_status = NULL;
```

### Step 2: Copy the Status Screen Function
```c
static void create_libre_status_screen(void) {
    screen_libre_status = lv_obj_create(NULL);
    // ... copy exact layout from create_dexcom_status_screen()
    // Only change header text to "Libre Account"
    // And change event handler function names
}
```

### Step 3: Change Header Text
```c
lv_label_set_text(header, "Libre Account");  // ← Only change this
```

### Step 4: Update CGM Type Value
```c
lv_label_set_text(cgm_value, "LibreLinkUp");  // ← Change this
```

### Step 5: Update Account Value
```c
lv_label_set_text(user_value, libre_email_buf);  // ← Use Libre credential buffer
```

### Step 6: Update Logout Button
```c
// Logout button - smaller, positioned absolutely to avoid overlap
lv_obj_t *logout_btn = lv_btn_create(screen);
lv_obj_set_size(logout_btn, 160, 38);  // Smaller than default
lv_obj_align(logout_btn, LV_ALIGN_TOP_MID, 0, 197);  // Absolute Y position
lv_obj_add_style(logout_btn, &style_card, 0);
lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), 0);
```

### Step 7: Change Event Handlers
```c
lv_obj_add_event_cb(back_btn, libre_back_event_cb, LV_EVENT_CLICKED, NULL);
lv_obj_add_event_cb(logout_btn, libre_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
```

### Step 8: Create Event Handler Functions
```c
static void libre_back_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Back from Libre status screen");
    lv_scr_load_anim(screen_cgm_settings, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void libre_logout_btn_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Libre logout button clicked");
    nvs_clear_libre_credentials();
    memset(libre_email_buf, 0, sizeof(libre_email_buf));
    memset(libre_password_buf, 0, sizeof(libre_password_buf));
    libre_api_set_authenticated(false);
    ESP_LOGI(TAG, "Libre credentials cleared");
    lv_scr_load_anim(screen_cgm_settings, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
}
```

---

## Why This Pattern?

### Benefits

1. **Consistency**: All CGM types look and feel the same
2. **Less Code**: No need to design unique layouts for each CGM
3. **Easier Maintenance**: Fix layout once, affects all CGM types
4. **Less Memory**: Fewer screens loaded at once
5. **User Experience**: Users know what to expect regardless of CGM source

### Trade-offs

**What we lose**: Custom branding per CGM (e.g., Dexcom blue theme, Libre orange theme)
**What we gain**: Simpler codebase, faster development, consistent UX

### When to Deviate

Only create a **custom layout** for a CGM type if:
- The CGM requires significantly different information (e.g., sensor serial number, expiration date)
- The authentication flow is completely different (e.g., OAuth tokens instead of username/password)
- The CGM provides real-time status that needs prominent display (e.g., sensor warmup countdown)

For basic username/password or email/password authentication, **always use this standard pattern**.

---

## Current Status

| CGM Type | Status Screen | Implemented |
|----------|---------------|-------------|
| Dexcom | ✅ | Yes (line 4155 in main.c) |
| Libre | ❌ | No (future) |
| Nightscout | ❌ | No (future) |

---

## Related Files

- **Screen Implementation**: `main/main.c` (line 4155)
- **Dexcom API**: `main/dexcom_api.c`
- **NVS Storage**: `main/nvs_config.c`
- **Project Status**: `PROJECT_STATUS.md`

---

## Example: Adding LibreLinkUp Support

When implementing LibreLinkUp, you would:

1. Create `libre_api.c` and `libre_api.h` (similar to `dexcom_api.c`)
2. Add NVS functions: `nvs_set_libre_credentials()`, `nvs_get_libre_credentials()`, `nvs_clear_libre_credentials()`
3. Copy `create_dexcom_status_screen()` → `create_libre_status_screen()`
4. Change **only** the header text: `"Dexcom Account"` → `"Libre Account"`
5. Change CGM type value: `"Dexcom Share"` → `"LibreLinkUp"`
6. Update credential buffer: `dexcom_username_buf` → `libre_email_buf`
7. Create event handlers: `libre_back_event_cb()`, `libre_logout_btn_event_cb()`

**That's it!** No need to redesign the entire screen layout.

---

✅ **This pattern is now the standard for all CGM account status screens.**
