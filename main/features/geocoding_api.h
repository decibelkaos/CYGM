/*
 * geocoding_api.h
 *
 * Online geocoding using Open-Meteo Geocoding API (free, no API key required)
 * Worldwide coverage - searches by city name, postal code, or address
 */

#ifndef GEOCODING_API_H
#define GEOCODING_API_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_GEOCODE_RESULTS 5
#define MAX_LOCATION_NAME_LEN 64
#define MAX_COUNTRY_NAME_LEN 32
#define MAX_TIMEZONE_NAME_LEN 64

// Geocoding search result
typedef struct {
    char name[MAX_LOCATION_NAME_LEN];           // City/location name
    char country[MAX_COUNTRY_NAME_LEN];         // Country name
    char admin1[MAX_LOCATION_NAME_LEN];         // State/province/region
    char timezone[MAX_TIMEZONE_NAME_LEN];       // IANA timezone
    float latitude;
    float longitude;
    uint32_t population;                         // Population (for sorting relevance)
    bool valid;
} geocode_result_t;

/**
 * Search for locations by city name, postal code or address.
 * results must have room for MAX_GEOCODE_RESULTS; result_count receives the
 * number found (0 if none).
 */
esp_err_t geocoding_search(const char *search_query, geocode_result_t *results, uint8_t *result_count);

#ifdef __cplusplus
}
#endif

#endif // GEOCODING_API_H
