# Battery Monitoring Implementation
**Date**: January 31, 2026
**Version**: 1.3.0

## Overview
Implemented comprehensive battery monitoring system for the CYGM device, providing voltage display, percentage calculation, low battery warnings, and automatic shutdown protection.

## Hardware Configuration

### GPIO Assignment
- **GPIO 39** (ADC1_CH3) - Battery voltage monitoring
- **Voltage Divider**: 1.46x (measured under load conditions)

### Why GPIO 39?
Through systematic ADC scanning of all available GPIOs, GPIO 39 was identified as the battery monitoring pin:
- GPIO 32, 33: Reserved for touch I2C (SDA/SCL)
- GPIO 34: Light sensor (photoresistor with 2x1MΩ bridge)
- GPIO 35, 36: Incorrect voltage readings
- **GPIO 39**: Confirmed battery voltage (2.86V ADC = 4.08V battery)

## Implementation Details

### Voltage Reading
```c
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3   // GPIO 39
#define BATTERY_VOLTAGE_DIVIDER 1.46        // Calibrated under load
```

**Features**:
- 10-sample averaging for precision (reduces noise)
- ADC calibration using esp_adc library
- 3 decimal place voltage display (e.g., 4.106V)
- 30-second update interval

### Percentage Mapping
Calibrated for **under-load voltage** (realistic usage conditions):

| Voltage | Percentage |
|---------|-----------|
| ≥4.0V   | 100%      |
| 3.9V    | 90%       |
| 3.8V    | 80%       |
| 3.7V    | 65%       |
| 3.6V    | 40%       |
| 3.5V    | 20%       |
| 3.4V    | 10%       |
| 3.2V    | 0% (shutdown) |

**Why 4.0V = 100%?**
- Provides buffer for voltage sag during WiFi/SSL activity
- Battery reads ~4.1V when fully charged but drops to ~4.0V under load
- User experience: Battery shows 100% longer, more realistic representation

### Display Integration
**Home Screen**:
- Battery icon: Top left of CGM card
- Percentage label: 4px spacing to right of icon
- Color-coded icons:
  - **Green**: 90-100% (LV_SYMBOL_BATTERY_FULL)
  - **White**: 60-89% (LV_SYMBOL_BATTERY_3)
  - **Orange**: 30-59% (LV_SYMBOL_BATTERY_2), 10-29% (LV_SYMBOL_BATTERY_1)
  - **Red flashing**: <10% (LV_SYMBOL_BATTERY_EMPTY)

### Low Battery Protection
**10% Warning**:
- 3 beeps at 800 Hz (250ms apart)
- Red flashing battery icon (500ms toggle)
- Warning flag prevents repeated alerts

**Critical Shutdown (3.2V)**:
- Full-screen warning message
- Shutdown tone (400 Hz, 80% volume)
- 3-second delay for user to see message
- Deep sleep mode (prevents battery damage)
- Wake on USB reconnect

## Technical Challenges & Solutions

### Challenge 1: Incorrect GPIO
**Problem**: Initial implementation used GPIO 34, reading 0.28V regardless of battery voltage
**Root Cause**: GPIO 34 is connected to light sensor (photoresistor), not battery
**Solution**: Created systematic GPIO scanner to test all ADC pins

### Challenge 2: Boot Loop - LVGL Assertion
**Problem**: Device crashed with "lvgl_port_init must be called first"
**Root Cause**: Battery task started in `app_main()` before LVGL initialized
**Solution**: Moved task creation to `ui_task_core1()` after LVGL fully initialized

### Challenge 3: I2C Conflict During GPIO Scan
**Problem**: Display stopped updating after GPIO scanner ran
**Root Cause**: Scanner reconfigured GPIO 32/33 as ADC, breaking touch I2C
**Solution**: Removed GPIO scanner after confirming GPIO 39 works

### Challenge 4: Voltage Calibration
**Problem**: Reading 3.98V while multimeter showed 4.107V
**Root Cause**: Initial divider calibrated when charging (higher voltage)
**Solution**:
- Calibrated using unplugged battery voltage
- Recognized voltage sag under load is normal and useful
- Set 4.0V = 100% to provide buffer

## Code Structure

### Key Functions
- `battery_init()` - Initialize ADC and calibration
- `battery_read_voltage()` - 10-sample averaging with calibration
- `battery_voltage_to_percent()` - Percentage mapping
- `update_battery_display()` - Update UI elements
- `battery_monitor_task()` - 30-second monitoring loop
- `battery_low_alert()` - 3 beeps warning
- `battery_flash_timer_cb()` - Red flashing at <10%

### Memory Usage
- ADC handle: Minimal overhead (~100 bytes)
- 10 ADC samples: 40 bytes stack
- Task stack: 3KB
- Total impact: ~3.2KB RAM

## Testing Results

### Voltage Accuracy
- Multimeter (unplugged): 4.10V
- Device reading: 4.110V
- **Error: <10mV** ✓

### Under-Load Behavior
- Idle: 4.11V
- WiFi/SSL activity: 4.10V → 4.06V
- Voltage recovers after activity
- This is **normal and expected** (internal resistance)

### Battery Life Observations
- Full charge (4.1V) → ~100% display
- Normal usage: Gradual discharge
- Critical shutdown: Prevents over-discharge damage

## Documentation Updates
- ✅ CHANGELOG.md - Version 1.3.0 entry
- ✅ PROJECT_STATUS.md - Battery monitoring section added
- ✅ README.md - Updated to Beta stage, v1.3.0

## Alarm System Improvements (v1.3.0)

### Slider UX Enhancements
- **Text**: "Slide to disarm" moved inside slider (white color)
- **Height**: Increased to 40px to fit label
- **Snap-back**: If released before 100%, animates back to 0
- **Event handling**: Added LV_EVENT_RELEASED for snap-back

### Disarm Button Change
- **Old**: "Dismiss Alarm" (permanent)
- **New**: "Disarm All Alarms (3 Hours)" (180-minute snooze)
- Provides extended quiet period for sleeping/meetings

### Bug Fix
**Critical crash fix**: Slider deletion panic
- **Problem**: Deleting slider label that was already auto-deleted with parent
- **Solution**: Set label pointer to NULL before deleting slider
- Label is child of slider, auto-deleted when parent deleted

### LED Breathing Disabled
- Disabled `led_breathing_task` to reduce power consumption
- LED now off by default: `led_set_color(0, 0, 0)`
- Can be re-enabled later if needed

## Status
✅ **Feature Complete**
✅ **Tested and Working**
✅ **Ready for Beta Testing**

## Next Steps
1. Field testing with real-world battery usage
2. Monitor battery life over multiple charge cycles
3. Consider adding charging status detection (if needed)
4. Potentially add battery usage statistics (optional)

---

**Result**: Battery monitoring system is fully functional and provides users with clear, accurate battery status information with protection against over-discharge.
