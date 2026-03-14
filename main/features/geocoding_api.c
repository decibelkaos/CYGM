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

static const char *TAG = "GEOCODING";

#define GEOCODING_API_URL "http://geocoding-api.open-meteo.com/v1/search"
#define GEOCODE_API_REQUEST_COUNT 10  // Request more than we display so population sort finds big cities
#define HTTP_RESPONSE_BUFFER_SIZE 4096

// HTTP response buffer
static char http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
static int http_response_len = 0;

/**
 * @brief HTTP event handler for geocoding requests
 */
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

/**
 * @brief URL encode a string (spaces -> %20, etc.)
 */
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

/**
 * @brief Search for locations by name
 */
esp_err_t geocoding_search(const char *search_query, geocode_result_t *results, uint8_t *result_count) {
    if (!search_query || !results || !result_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *result_count = 0;

    // Initialize results
    for (int i = 0; i < MAX_GEOCODE_RESULTS; i++) {
        results[i].valid = false;
    }

    // URL encode the search query
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

    // Reset response buffer
    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    // Configure HTTP client
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

    // Perform HTTP GET request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

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

    // Parse JSON response
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response (heap: %lu, response: %d bytes)",
                 esp_get_free_heap_size(), http_response_len);
        return ESP_FAIL;
    }

    cJSON *results_array = cJSON_GetObjectItem(root, "results");
    if (!results_array || !cJSON_IsArray(results_array)) {
        ESP_LOGI(TAG, "No results found for: %s", search_query);
        cJSON_Delete(root);
        return ESP_OK;  // No results is not an error
    }

    // Parse results with streaming top-K selection by population.
    // We request GEOCODE_API_REQUEST_COUNT (10) from the API but only keep
    // the top MAX_GEOCODE_RESULTS (5) by population. This avoids allocating
    // a larger array — we parse each result into a single temp struct and
    // either add it directly or swap it with the smallest in our output.
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

    // Sort results by population descending — larger cities first.
    // The API ranks by feature_code (county seats first) which hides
    // large cities like Covington, WA (pop 19K) behind tiny county seats.
    for (int i = 0; i < *result_count - 1; i++) {
        for (int j = i + 1; j < *result_count; j++) {
            if (results[j].population > results[i].population) {
                geocode_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    // Re-log sorted order
    for (int i = 0; i < *result_count; i++) {
        ESP_LOGI(TAG, "Result %d: %s, %s, %s (%.4f, %.4f) pop=%lu [%s]",
                 i + 1, results[i].name, results[i].admin1, results[i].country,
                 results[i].latitude, results[i].longitude,
                 (unsigned long)results[i].population, results[i].timezone);
    }

    ESP_LOGI(TAG, "Found %d location(s) for: %s (sorted by population)", *result_count, search_query);
    return ESP_OK;
}
