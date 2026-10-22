# Daily use

## Home screen

The main screen shows the glucose value, direction of travel, and other information intended to be readable at a glance. Stale readings are visually muted. CYGM fetches cloud data about every 90 seconds, but the sensor decides when a genuinely new measurement exists.

## Alarms

CYGM has adjustable warning and alarm thresholds, multiple sounds, quiet-hour options, and a data-gap alert. It also has urgent-low behavior that can't be turned off through the normal settings.

Check every alarm setting after installation or a significant firmware update. **CYGM's alerts are additional notifications, not replacements for your official CGM alerts.**

## Charts and statistics

Charts can show 1, 3, 6, 12, or 24 hours of readings. The **Configured Range** figure uses the warning thresholds you've selected. It is **not** the same thing as a clinical Time in Range report. Short-window GMI and CV are estimates, not substitutes for longer clinical reports.

## Brightness and night mode

CYGM includes scheduled night dimming and a larger night display. Brightness and dimming behavior can also depend on USB and battery status.

A community report found that the screen may dim on USB power after a battery finishes charging, even with **Dim when Plugged in** off. That's being tracked in [issue #3](https://github.com/decibelkaos/CYGM/issues/3), with a proposed change in [PR #4](https://github.com/decibelkaos/CYGM/pull/4). Don't assume the fix is in your installed release until the release notes say so.

## Updates and storage

Updates can be installed over Wi-Fi. Optional microSD logging can save CSV data. If you're giving a device to someone else, use **Erase Device** first to remove local account details.
