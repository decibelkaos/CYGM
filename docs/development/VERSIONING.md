# CYGM Versioning Guide

## Current Version
**v1.3.0 Beta** (as of January 31, 2026)

## Version Format
`MAJOR.MINOR.PATCH STAGE`

Example: `v1.3.0 Beta`

## Version Location
The version is defined in `main/main.c`:
```c
#define CYGM_VERSION "1.3.0"
#define CYGM_STAGE "Beta"  // "Alpha", "Beta", or "Release"
```

This appears on the boot screen below the email address.

## Version Stages

### Alpha
- **Status**: Early development, experimental features
- **Stability**: Expect bugs and crashes
- **Testing**: Internal testing only
- **Features**: Core functionality being implemented
- **Changes**: Frequent breaking changes
- **Example**: v0.5.0 Alpha

### Beta
- **Status**: Feature-complete for current milestone, undergoing testing
- **Stability**: Generally stable, minor bugs expected
- **Testing**: Field testing with real users
- **Features**: All major features implemented and working
- **Changes**: Bug fixes and minor improvements only
- **Example**: v1.3.0 Beta ⬅️ **Current Stage**

### Release
- **Status**: Production-ready, stable release
- **Stability**: Thoroughly tested, no known critical bugs
- **Testing**: Passed extensive field testing
- **Features**: Fully functional and documented
- **Changes**: Only critical bug fixes
- **Example**: v1.0.0 Release

## Semantic Versioning Rules

### MAJOR version (X.0.0)
Increment when making **incompatible changes**:
- Complete redesign or rewrite
- Breaking changes to APIs or data formats
- Major architectural changes
- NVS storage format changes (requires factory reset)

**Example**: v1.x.x → v2.0.0

### MINOR version (1.X.0)
Increment when adding **new features** in a backward-compatible manner:
- New alarm features
- Additional CGM source support (Nightscout, LibreLink)
- New display screens or UI elements
- Battery monitoring system
- Weather integration

**Example**: v1.2.0 → v1.3.0

### PATCH version (1.3.X)
Increment when making **bug fixes**:
- Crash fixes
- Memory leak fixes
- UI glitches
- Incorrect calculations
- Performance improvements

**Example**: v1.3.0 → v1.3.1

## Version History

### v1.3.0 Beta (2026-01-31) - Current
**Added**:
- Battery monitoring system (GPIO 39)
- Battery voltage display with percentage
- Low battery warnings and automatic shutdown
- Green glow on stable trend arrows
- Orange glow on single up/down arrows
- Red glow on double up/down arrows
- Larger trend arrows (80x80 canvas)

**Changed**:
- Alarm UI reorganization (Visual Alert before Repeat toggle)
- Slider UX improvements with snap-back
- Disarm button now snoozes for 3 hours

**Fixed**:
- Slider deletion crash
- LED breathing disabled to save power

### v1.2.0 Beta (2026-01-30)
**Added**:
- Visual alarm system (pulsing white overlay)
- Audio alarm repeat functionality
- Snooze/dismiss screen (5/10/15/30 min)
- Alarm detail editor improvements

**Fixed**:
- LoadProhibited crash on alarm settings back button
- Snooze timer re-triggering
- White screen pulsing opacity issue

### v1.1.0 Alpha (2026-01-29)
**Added**:
- SD card logging system for debugging

### v1.0.0 Alpha (2026-01-28)
**Added**:
- Boot sound and success confirmation audio
- Memory time-sharing for Dexcom and Weather

**Fixed**:
- Status preservation bug with audio feedback
- SSL memory conflicts

### v0.9.0 Alpha (2026-01-27)
**Added**:
- 4 configurable glucose alarms
- Alarm configuration UI
- Tone playback system (PWM buzzer)
- Alarm logic with state tracking

**Fixed**:
- Dexcom SSL handshake failures
- Memory optimization for HTTPS connections

## Promotion Criteria

### Alpha → Beta
- [ ] All core features implemented
- [ ] No known crash bugs
- [ ] Basic testing completed
- [ ] Ready for field testing

**Status**: ✅ **Achieved at v1.3.0**

### Beta → Release 1.0
- [ ] Extended field testing (2+ weeks)
- [ ] No critical bugs reported
- [ ] All features documented
- [ ] User feedback incorporated
- [ ] Battery life verified
- [ ] Multiple CGM sources tested (Dexcom + 1 other)

**Status**: ❌ **Not Yet Achieved**
- Still gathering field test data
- Need Nightscout/LibreLink implementation
- Requires extended battery life testing

## How to Update Version

### 1. Update Version Defines
Edit `main/main.c`:
```c
#define CYGM_VERSION "1.4.0"  // Update version number
#define CYGM_STAGE "Beta"     // Update stage if needed
```

### 2. Update Documentation
- `README.md` - Update version badge at top
- `docs/development/PROJECT_STATUS.md` - Update "Current Version"
- `CHANGELOG.md` - Add new entry with changes
- `docs/development/VERSIONING.md` - Add version history entry

### 3. Commit Changes
```bash
git add .
git commit -m "Bump version to vX.Y.Z Stage"
git tag -a vX.Y.Z -m "Version X.Y.Z - Brief description"
git push origin main --tags
```

## Next Milestones

### v1.4.0 Beta - Planned
- Nightscout API integration
- Historical glucose graph (3 hours)
- Settings import/export

### v2.0.0 Release - Future
- Multiple CGM account support
- Cloud sync
- OTA updates
- Web configuration interface

---

**Last Updated**: January 31, 2026
