# LED Visual Feedback System

## Overview
**Version**: 1.3.2
**Date**: January 31, 2026
**Module**: `main/hardware/led.c/h`

The LED visual feedback system provides real-time status indicators using the RGB LED, allowing users to understand device state without looking at the screen.

---

## LED Patterns

### 1. WiFi Boot Blink (Green)
**When**: During boot sequence after WiFi connects
**Pattern**: Blink every 200ms
**Color**: Green (brightness 80/127)
**Duration**: Until home screen loads
**Purpose**: Indicates WiFi is connected and system is initializing

**Code Location**: `background_tasks.c:109` (start), `background_tasks.c:68` (stop)

---

### 2. Dexcom Poll Indicator (Blue)
**When**: Just before Dexcom API glucose fetch
**Pattern**: 5-step double-blink sequence with decreasing brightness
**Color**: Blue
**Duration**: 1.5 seconds total

**Brightness Steps**:
- Step 1: 100% brightness (127/127) - 2 blinks
- Step 2: 85% brightness (108/127) - 2 blinks
- Step 3: 70% brightness (89/127) - 2 blinks
- Step 4: 60% brightness (76/127) - 2 blinks
- Step 5: 50% brightness (64/127) - 2 blinks

**Timing per Step** (300ms each):
- Blink 1: 50ms on, 50ms off
- Blink 2: 50ms on, 150ms gap

**Purpose**: Creates distinctive "ramping down" effect to signal upcoming data poll

**Code Location**: `main.c:1290`, `led.c:64-84`

---

### 3. Success Indicator (Green)
**When**: After successful Dexcom glucose fetch
**Pattern**: Steady green, then fade-out
**Color**: Green (brightness 80/127)
**Duration**:
- 3 seconds steady
- 500ms fade-out (80 steps @ 6ms each)

**Purpose**: Confirms successful data retrieval

**Code Location**: `main.c:1307`, `led.c:86-98`

---

### 4. Error Indicator (Red)
**When**: WiFi connection failure or Dexcom fetch error
**Pattern**: Blink 500ms on, 500ms off
**Color**: Red (brightness 80/127)
**Duration**:
- WiFi error: Continuous until resolved
- Dexcom error: 5 seconds

**Purpose**: Alerts user to connection or data fetch problems

**Code Locations**:
- WiFi error: `background_tasks.c:251`
- Dexcom error: `main.c:1333`
- Pattern: `led.c:100-104`

---

### 5. Alarm Flash (Bright Red)
**When**: Active glucose alarm (high/low alarm or warning)
**Pattern**: Fast flash 100ms on, 100ms off
**Color**: Bright red (brightness 127/127 - maximum)
**Duration**: Until alarm is acknowledged/snoozed

**Purpose**: Urgent visual alert for out-of-range glucose

**Code Locations**:
- Start: `main.c:1111`
- Stop: `main.c:1163`
- Pattern: `led.c:106-110`

---

## Architecture

### Hardware
- **LED Type**: RGB LED with active-low PWM control
- **Control Method**: ESP32 LEDC peripheral
- **Resolution**: 10-bit PWM (0-1023 duty cycle)
- **Frequency**: 1000 Hz
- **GPIO Pins**:
  - Red: GPIO 4
  - Green: GPIO 16
  - Blue: GPIO 17

### Software
- **Task**: Dedicated FreeRTOS task (`led_task`)
- **Core**: Core 0
- **Priority**: 5
- **Stack Size**: 2048 bytes
- **Task Function**: `led.c:40-103`

### State Machine
```c
typedef enum {
    LED_MODE_OFF,
    LED_MODE_STATIC,
    LED_MODE_WIFI_BOOT_BLINK,
    LED_MODE_DEXCOM_POLL_BLINK,
    LED_MODE_SUCCESS_FADE,
    LED_MODE_ERROR_BLINK,
    LED_MODE_ALARM_FLASH
} led_mode_t;
```

### API Functions

```c
// Basic control
void led_init(void);                    // Initialize LED PWM
void led_set_color(uint8_t r, uint8_t g, uint8_t b);  // Set RGB color
void led_off(void);                     // Turn off LED

// WiFi boot
void led_start_wifi_boot_blink(void);   // Start green boot blink
void led_stop_wifi_boot_blink(void);    // Stop boot blink

// Dexcom poll
void led_blink_blue(void);              // Trigger 5-step blue ramp

// Success
void led_show_success(void);            // Steady green + fade

// Error
void led_start_error_blink(void);       // Start red error blink
void led_stop_error_blink(void);        // Stop error blink

// Alarm
void led_start_alarm_flash(void);       // Start bright red flash
void led_stop_alarm_flash(void);        // Stop alarm flash
```

---

## Usage Examples

### Boot Sequence
```c
// When WiFi connects during boot
led_start_wifi_boot_blink();

// When home screen loads
led_stop_wifi_boot_blink();
```

### Glucose Fetch
```c
// Before API call
led_blink_blue();

// Wait 1.5 seconds for animation to complete
// Then make API call

if (fetch_success) {
    led_show_success();  // Green fade
} else {
    led_start_error_blink();
    vTaskDelay(pdMS_TO_TICKS(5000));
    led_stop_error_blink();
}
```

### Alarm Trigger
```c
// When alarm triggers
led_start_alarm_flash();

// When alarm is acknowledged/snoozed
led_stop_alarm_flash();
```

---

## Design Rationale

### Why Different Patterns?
- **Color coding**: Intuitive (green=good, red=bad, blue=activity)
- **Brightness ramping**: Makes Dexcom poll distinctive and noticeable
- **Duration**: Long enough to be seen, short enough to not annoy
- **Double-blink**: More attention-grabbing than single blink

### Power Consumption
- LEDs are active-low, only on during specific events
- Most patterns are brief (1.5-3 seconds)
- Alarm flash is the only continuous pattern (critical alert)

### User Experience
- Non-intrusive feedback that doesn't require screen attention
- Patterns are distinctive enough to identify at a glance
- Critical states (alarms) use maximum brightness

---

## Memory Usage
- Task stack: 2048 bytes
- Global variables: ~20 bytes (mode state, handles)
- Code size: ~800 bytes

---

## Future Enhancements
- [ ] Configurable brightness levels
- [ ] Custom patterns per alarm type
- [ ] Battery level indicator (color-coded)
- [ ] WiFi signal strength indicator

---

## Related Documentation
- [Alarm System](ALARM_SYSTEM.md)
- [Hardware Reference](../hardware/HARDWARE_REFERENCE.md)
- [Project Status](../development/PROJECT_STATUS.md)
