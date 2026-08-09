/*
 * cgm_types.h
 *
 * Provider-agnostic CGM type aliases plus the versioned alarm-extension blob.
 * The aliases are pure preprocessor — zero runtime cost.
 */

#ifndef CGM_TYPES_H
#define CGM_TYPES_H

#include <stdint.h>
#include "dexcom_api.h"

// Type aliases so consuming code doesn't need to know which provider is active
typedef dexcom_trend_t   cgm_trend_t;
typedef dexcom_status_t  cgm_status_t;
typedef dexcom_glucose_t cgm_glucose_t;

// Function aliases for trend display helpers
#define cgm_trend_arrow       dexcom_trend_arrow
#define cgm_trend_description dexcom_trend_description

// Active CGM provider
typedef enum {
    CGM_PROVIDER_DEXCOM = 0,
    CGM_PROVIDER_LIBRE,
    CGM_PROVIDER_NIGHTSCOUT,
} cgm_provider_t;

// ==================== Versioned Alarm Extension Blob ====================
//
// Quiet hours, escalating volume, the data-gap watchdog and the predictive/rate
// alerts. Kept in their OWN NVS blob rather than inside cgm_alarms_t, because
// that loader rejects any size mismatch and falls back to factory defaults.
//
// LAYOUT IS FROZEN. Fields may only be APPENDED (consume `reserved` from the
// front); never reorder, resize or remove one. Packed and byte-sized behind a
// leading uint16 pair so the on-flash layout cannot drift between builds.
//
// Loader contract (nvs_config.c): start from cygm_alarm_ext_defaults(), then
// memcpy min(stored_size, sizeof) bytes over them, so fields an older or newer
// blob doesn't carry keep their default and the rest are always honoured. Never
// wipe the blob — a firmware downgrade must not lose settings. Finally restamp
// .version/.size and clamp every field to the range documented beside it.
#define CYGM_ALARM_EXT_VERSION   1
#define CYGM_ALARM_EXT_NVS_KEY   "alarm_ext"   // <= 15 chars (NVS key limit)

typedef struct __attribute__((packed)) {
    uint16_t version;              // +0   CYGM_ALARM_EXT_VERSION at write time
    uint16_t size;                 // +2   sizeof(cygm_alarm_ext_t) at write time
    /* ---- v1 fields — FROZEN ---- */
    uint8_t  quiet_enabled;        // +4   0/1            default 0 (opt-in)
    uint8_t  quiet_start_hour;     // +5   0-23           default 22
    uint8_t  quiet_start_min;      // +6   0-59           default 0
    uint8_t  quiet_end_hour;       // +7   0-23           default 7
    uint8_t  quiet_end_min;        // +8   0-59           default 0
    uint8_t  escalate_enabled;     // +9   0/1            default 1
    uint8_t  escalate_step_min;    // +10  1-30 minutes   default 3
    uint8_t  escalate_max_volume;  // +11  10-100 percent default 100
    uint8_t  gap_enabled;          // +12  0/1            default 1
    uint8_t  gap_minutes;          // +13  10-60 minutes  default 20
    uint8_t  gap_tone;             // +14  alarm_tone_t   default 9 (ascending)
    uint8_t  gap_volume;           // +15  0-100 percent  default 70
    uint8_t  predict_enabled;      // +16  0/1            default 1
    uint8_t  predict_horizon_min;  // +17  5-45 minutes   default 20
    uint8_t  rate_enabled;         // +18  0/1            default 0 (opt-in)
    uint8_t  rate_threshold_x10;   // +19  10-60 = mg/dL per minute x10, default 30
    uint8_t  suppress_min;         // +20  5-120 minutes  default 30
    uint8_t  snooze_default_min;   // +21  5-120 minutes  default 30
    uint8_t  urgent_low_floor;     // +22  40-90 mg/dL    default 55
    uint8_t  reserved[9];          // +23..+31 zero-filled — consume from the front
} cygm_alarm_ext_t;                // 32 bytes

_Static_assert(sizeof(cygm_alarm_ext_t) == 32,
               "cygm_alarm_ext_t layout is frozen at 32 bytes — append into reserved[]");

// Factory defaults. Defined in main.c (the alarm engine owns the policy) so
// nvs_config.c and the settings UI share one source of truth.
void cygm_alarm_ext_defaults(cygm_alarm_ext_t *ext);

// Persistence, implemented in nvs_config.c per the loader contract above.
// Declared here so the frozen struct and the only two functions allowed to
// touch its bytes live side by side. nvs_load_alarm_ext returns ESP_OK when a
// stored blob was applied, ESP_ERR_NVS_NOT_FOUND when defaults were kept.
esp_err_t nvs_save_alarm_ext(const cygm_alarm_ext_t *ext);
esp_err_t nvs_load_alarm_ext(cygm_alarm_ext_t *ext);

#endif // CGM_TYPES_H
