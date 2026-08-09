/*
 * geocoding_api.c
 *
 * Online geocoding using Open-Meteo Geocoding API
 * API documentation: https://open-meteo.com/en/docs/geocoding-api
 */

#include "geocoding_api.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>   // strtof

static const char *TAG = "GEOCODING";

#define GEOCODING_API_URL "http://geocoding-api.open-meteo.com/v1/search"
#define GEOCODE_API_REQUEST_COUNT 10  // Request more than we display so population sort finds big cities
#define HTTP_RESPONSE_BUFFER_SIZE 4096

// HTTP response buffer
static char http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
static int http_response_len = 0;

/** HTTP event handler for geocoding requests. */
static esp_err_t geocoding_http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < HTTP_RESPONSE_BUFFER_SIZE) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/** URL-encode a string (spaces -> %20, etc.). */
static void url_encode(const char *input, char *output, size_t output_size) {
    const char *hex = "0123456789ABCDEF";
    size_t out_idx = 0;

    while (*input && out_idx < output_size - 4) {
        if ((*input >= 'A' && *input <= 'Z') ||
            (*input >= 'a' && *input <= 'z') ||
            (*input >= '0' && *input <= '9') ||
            *input == '-' || *input == '_' || *input == '.' || *input == '~') {
            output[out_idx++] = *input;
        } else if (*input == ' ') {
            output[out_idx++] = '+';
        } else {
            output[out_idx++] = '%';
            output[out_idx++] = hex[(*input >> 4) & 0x0F];
            output[out_idx++] = hex[*input & 0x0F];
        }
        input++;
    }
    output[out_idx] = '\0';
}

// ===========================================================================
// Postal-code fallback
//
// Open-Meteo's geocoding is a CITY-NAME search: it resolves US ZIPs but returns
// nothing for Canadian and most international postal codes. When the city search
// yields 0 results we detect a postal-code-shaped query, resolve it through
// Zippopotam.us (free, no key), then fetch the IANA timezone from Open-Meteo's
// timezone=auto endpoint, which Zippopotam does not provide.
//
// All plain HTTP, under the network_mutex the caller already holds, reusing the
// shared response buffer — so no new tasks and no new heap.
// ===========================================================================

// Classify a query as a postal code and produce the Zippopotam country code +
// normalized lookup token. Returns false (skip the fallback) for non-postal text.
static bool postal_code_detect(const char *query, char *cc, char *norm) {
    char compact[16];           // uppercased, spaces/hyphens removed
    char outward[8] = {0};      // chars before the first space (UK outward code)
    int n = 0, ow = 0;
    bool space_seen = false;
    for (const char *p = query; *p && n < (int)sizeof(compact) - 1; p++) {
        if (*p == ' ' || *p == '-') { space_seen = true; continue; }
        char u = (char)toupper((unsigned char)*p);
        compact[n++] = u;
        if (!space_seen && ow < (int)sizeof(outward) - 1) outward[ow++] = u;
    }
    compact[n] = '\0';
    if (n == 0) return false;

    // Canada FSA: letter-digit-letter (check BEFORE GB). Zippopotam needs the
    // 3-char FSA only ("K1G 0N1" -> "K1G"; the full code returns {}).
    if (n >= 3 && isalpha((unsigned char)compact[0]) &&
        isdigit((unsigned char)compact[1]) && isalpha((unsigned char)compact[2])) {
        strcpy(cc, "ca");
        compact[3] = '\0';
        strcpy(norm, compact);
        return true;
    }

    // US ZIP: exactly 5 digits, or ZIP+4 (9 digits) -> first 5. (Mostly a safety
    // net; Open-Meteo already resolves US ZIPs before we get here.)
    bool all_digits = true;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)compact[i])) { all_digits = false; break; }
    }
    if (all_digits && (n == 5 || n == 9)) {
        strcpy(cc, "us");
        compact[5] = '\0';
        strcpy(norm, compact);
        return true;
    }

    // UK outward code: letter+digit, or 2 letters+digit. Use the part before the
    // first space, else the leading run (capped at 4 chars).
    if (isalpha((unsigned char)compact[0]) &&
        (isdigit((unsigned char)compact[1]) ||
         (n >= 3 && isalpha((unsigned char)compact[1]) && isdigit((unsigned char)compact[2])))) {
        strcpy(cc, "gb");
        const char *src = (space_seen && ow > 0) ? outward : compact;
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%.4s", src);   // cap at 4 chars
        strcpy(norm, tmp);
        return true;
    }

    return false;
}

// Resolve a postal code to lat/lon + place via Zippopotam. Returns ESP_OK on a
// hit; ESP_FAIL on a clean miss (404/empty) or transport error.
static esp_err_t zippopotam_lookup(const char *cc, const char *norm, geocode_result_t *out) {
    char url[128];
    snprintf(url, sizeof(url), "http://api.zippopotam.us/%s/%s", cc, norm);
    ESP_LOGI(TAG, "Postal fallback lookup: %s", url);

    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = geocoding_http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGI(TAG, "Postal lookup miss (HTTP %d) for %s/%s", status, cc, norm);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) return ESP_FAIL;
    cJSON *places = cJSON_GetObjectItem(root, "places");
    if (!places || !cJSON_IsArray(places) || cJSON_GetArraySize(places) == 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *place = cJSON_GetArrayItem(places, 0);
    memset(out, 0, sizeof(*out));

    cJSON *pname = cJSON_GetObjectItem(place, "place name");
    if (pname && cJSON_IsString(pname)) {
        // Trim verbose Zippopotam names ("Downtown Toronto (CN Tower / ...)")
        snprintf(out->name, sizeof(out->name), "%s", pname->valuestring);
        char *paren = strstr(out->name, " (");
        if (paren) *paren = '\0';
    }
    cJSON *state = cJSON_GetObjectItem(place, "state");
    if (state && cJSON_IsString(state)) snprintf(out->admin1, sizeof(out->admin1), "%s", state->valuestring);
    cJSON *country = cJSON_GetObjectItem(root, "country");
    if (country && cJSON_IsString(country)) snprintf(out->country, sizeof(out->country), "%s", country->valuestring);

    // Zippopotam lat/lon are JSON STRINGS, not numbers — use strtof.
    cJSON *lat = cJSON_GetObjectItem(place, "latitude");
    cJSON *lon = cJSON_GetObjectItem(place, "longitude");
    if (lat && cJSON_IsString(lat)) out->latitude = strtof(lat->valuestring, NULL);
    if (lon && cJSON_IsString(lon)) out->longitude = strtof(lon->valuestring, NULL);
    out->population = 0;
    out->timezone[0] = '\0';
    out->valid = true;
    cJSON_Delete(root);
    return ESP_OK;
}

// Fill an IANA timezone from lat/lon via Open-Meteo timezone=auto. Best-effort:
// leaves tz_out untouched on failure (caller already cleared it).
static void fill_timezone_from_latlon(float lat, float lon, char *tz_out, size_t tz_len) {
    char url[160];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto&forecast_days=1",
             lat, lon);

    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = geocoding_http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) return;

    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) return;
    cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    if (tz && cJSON_IsString(tz)) snprintf(tz_out, tz_len, "%s", tz->valuestring);
    cJSON_Delete(root);
}

// Orchestrate the postal fallback: detect -> Zippopotam -> timezone. Fills
// results[0] and *count=1 on success; leaves *count=0 on a miss.
static void postal_code_fallback(const char *query, geocode_result_t *results, uint8_t *count) {
    char cc[4] = {0}, norm[8] = {0};
    if (!postal_code_detect(query, cc, norm) || norm[0] == '\0') return;

    geocode_result_t r;
    if (zippopotam_lookup(cc, norm, &r) != ESP_OK) return;
    fill_timezone_from_latlon(r.latitude, r.longitude, r.timezone, sizeof(r.timezone));

    results[0] = r;
    *count = 1;
    ESP_LOGI(TAG, "Postal fallback resolved '%s' -> %s, %s (%.4f, %.4f) [%s]",
             query, r.name, r.country, r.latitude, r.longitude,
             r.timezone[0] ? r.timezone : "no-tz");
}

/**
 * Search for locations by city name or postal code. Tries the Open-Meteo
 * city-name search first and falls back to a Zippopotam postal-code lookup
 * (US/CA/GB) plus an Open-Meteo timezone when that returns nothing.
 */
esp_err_t geocoding_search(const char *search_query, geocode_result_t *results, uint8_t *result_count) {
    if (!search_query || !results || !result_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *result_count = 0;

    for (int i = 0; i < MAX_GEOCODE_RESULTS; i++) {
        results[i].valid = false;
    }

    char encoded_query[256];
    url_encode(search_query, encoded_query, sizeof(encoded_query));

    // Build API URL — request more results than we display so we can
    // pick the top 5 by population (API ranks by feature_code, not size)
    char url[512];
    snprintf(url, sizeof(url),
             "%s?name=%s&count=%d&language=en&format=json",
             GEOCODING_API_URL, encoded_query, GEOCODE_API_REQUEST_COUNT);

    ESP_LOGI(TAG, "Geocoding search for: %s", search_query);
    ESP_LOGI(TAG, "API URL: %s", url);

    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = geocoding_http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET returned status %d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Geocoding response (%d bytes): %s", http_response_len, http_response_buffer);
    ESP_LOGI(TAG, "Free heap before JSON parse: %lu bytes", esp_get_free_heap_size());

    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response (heap: %lu, response: %d bytes)",
                 esp_get_free_heap_size(), http_response_len);
        return ESP_FAIL;
    }

    cJSON *results_array = cJSON_GetObjectItem(root, "results");
    if (!results_array || !cJSON_IsArray(results_array)) {
        ESP_LOGI(TAG, "No city results for: %s — trying postal-code fallback", search_query);
        cJSON_Delete(root);
        postal_code_fallback(search_query, results, result_count);  // sets count=1 on hit
        return ESP_OK;  // No results (even after fallback) is not an error
    }

    // Streaming top-K selection by population: the API is asked for
    // GEOCODE_API_REQUEST_COUNT but only MAX_GEOCODE_RESULTS are kept, so each
    // result is parsed into one temp struct and either added or swapped in.
    int result_idx = 0;
    int min_pop_idx = 0;
    uint32_t min_pop = 0;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, results_array) {
        // Parse into temp struct (224 bytes on stack)
        geocode_result_t temp = {0};

        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name))
            snprintf(temp.name, sizeof(temp.name), "%s", name->valuestring);

        cJSON *country = cJSON_GetObjectItem(item, "country");
        if (country && cJSON_IsString(country))
            snprintf(temp.country, sizeof(temp.country), "%s", country->valuestring);

        cJSON *admin1 = cJSON_GetObjectItem(item, "admin1");
        if (admin1 && cJSON_IsString(admin1))
            snprintf(temp.admin1, sizeof(temp.admin1), "%s", admin1->valuestring);

        cJSON *tz = cJSON_GetObjectItem(item, "timezone");
        if (tz && cJSON_IsString(tz))
            snprintf(temp.timezone, sizeof(temp.timezone), "%s", tz->valuestring);

        cJSON *lat = cJSON_GetObjectItem(item, "latitude");
        cJSON *lon = cJSON_GetObjectItem(item, "longitude");
        if (lat && lon && cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            temp.latitude = (float)lat->valuedouble;
            temp.longitude = (float)lon->valuedouble;
        }

        cJSON *population = cJSON_GetObjectItem(item, "population");
        if (population && cJSON_IsNumber(population))
            temp.population = (uint32_t)population->valueint;

        temp.valid = true;

        if (result_idx < MAX_GEOCODE_RESULTS) {
            // Still filling slots — add directly
            results[result_idx] = temp;
            result_idx++;

            // Once full, find the entry with smallest population
            if (result_idx == MAX_GEOCODE_RESULTS) {
                min_pop = results[0].population;
                min_pop_idx = 0;
                for (int i = 1; i < MAX_GEOCODE_RESULTS; i++) {
                    if (results[i].population < min_pop) {
                        min_pop = results[i].population;
                        min_pop_idx = i;
                    }
                }
            }
        } else if (temp.population > min_pop) {
            // Replace the smallest entry with this larger-population city
            results[min_pop_idx] = temp;

            // Re-find the new minimum
            min_pop = results[0].population;
            min_pop_idx = 0;
            for (int i = 1; i < MAX_GEOCODE_RESULTS; i++) {
                if (results[i].population < min_pop) {
                    min_pop = results[i].population;
                    min_pop_idx = i;
                }
            }
        }
    }

    *result_count = result_idx < MAX_GEOCODE_RESULTS ? result_idx : MAX_GEOCODE_RESULTS;
    cJSON_Delete(root);

    // Sort by population descending. The API ranks by feature_code, which puts
    // county seats ahead of far larger cities.
    for (int i = 0; i < *result_count - 1; i++) {
        for (int j = i + 1; j < *result_count; j++) {
            if (results[j].population > results[i].population) {
                geocode_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    for (int i = 0; i < *result_count; i++) {
        ESP_LOGI(TAG, "Result %d: %s, %s, %s (%.4f, %.4f) pop=%lu [%s]",
                 i + 1, results[i].name, results[i].admin1, results[i].country,
                 results[i].latitude, results[i].longitude,
                 (unsigned long)results[i].population, results[i].timezone);
    }

    ESP_LOGI(TAG, "Found %d location(s) for: %s (sorted by population)", *result_count, search_query);
    return ESP_OK;
}
