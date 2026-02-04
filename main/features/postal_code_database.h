/*
 * postal_code_database.h
 *
 * Worldwide postal code database for offline location lookup
 * Generated from GeoNames data (www.geonames.org)
 * License: Creative Commons Attribution 4.0
 *
 * NOTE: US ZIP codes are GROUPED by city to save space!
 *       Range format: "10001-10292" covers all ZIPs in that range
 */

#ifndef POSTAL_CODE_DATABASE_H
#define POSTAL_CODE_DATABASE_H

#include <stdint.h>
#include <stdbool.h>

// Postal code entry structure
typedef struct {
    const char *code;        // Postal code or RANGE (e.g., "90210" or "10001-10292")
    const char *city;        // City name
    const char *state;       // State/Province/Region
    const char *country;     // Country name
    float latitude;          // Latitude (WGS84) - center for grouped entries
    float longitude;         // Longitude (WGS84) - center for grouped entries
} postal_code_entry_t;

// Search functions
const postal_code_entry_t* find_postal_code(const char *code);
const postal_code_entry_t* find_postal_code_fuzzy(const char *code);

// NEW: Unified search - searches BOTH postal codes AND city names
const postal_code_entry_t* find_location_unified(const char *search_text);
int count_location_matches(const char *search_text);

// Get database info
int get_postal_code_count(void);
const char* get_postal_code_attribution(void);

#endif // POSTAL_CODE_DATABASE_H
