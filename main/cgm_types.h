/*
 * cgm_types.h
 *
 * Provider-agnostic CGM type aliases.
 * Both Dexcom and Libre modules produce data in these types.
 * Zero-cost — pure preprocessor, no runtime overhead.
 */

#ifndef CGM_TYPES_H
#define CGM_TYPES_H

#include "dexcom_api.h"

// Type aliases so consuming code doesn't need to know which provider is active
typedef dexcom_trend_t   cgm_trend_t;
typedef dexcom_status_t  cgm_status_t;
typedef dexcom_glucose_t cgm_glucose_t;

// Function aliases for trend display helpers
#define cgm_trend_arrow       dexcom_trend_arrow
#define cgm_trend_description dexcom_trend_description

#endif // CGM_TYPES_H
