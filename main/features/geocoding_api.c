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

    // Build API URL
    char url[512];
    snprintf(url, sizeof(url),
             "%s?name=%s&count=%d&language=en&format=json",
             GEOCODING_API_URL, encoded_query, MAX_GEOCODE_RESULTS);

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

    // Parse JSON response
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *results_array = cJSON_GetObjectItem(root, "results");
    if (!results_array || !cJSON_IsArray(results_array)) {
        ESP_LOGI(TAG, "No results found for: %s", search_query);
        cJSON_Delete(root);
        return ESP_OK;  // No results is not an error
    }

    // Parse each result
    int result_idx = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, results_array) {
        if (result_idx >= MAX_GEOCODE_RESULTS) break;

        geocode_result_t *result = &results[result_idx];

        // Extract name
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name)) {
            snprintf(result->name, sizeof(result->name), "%s", name->valuestring);
        }

        // Extract country
        cJSON *country = cJSON_GetObjectItem(item, "country");
        if (country && cJSON_IsString(country)) {
            snprintf(result->country, sizeof(result->country), "%s", country->valuestring);
        }

        // Extract admin1 (state/province)
        cJSON *admin1 = cJSON_GetObjectItem(item, "admin1");
        if (admin1 && cJSON_IsString(admin1)) {
            snprintf(result->admin1, sizeof(result->admin1), "%s", admin1->valuestring);
        } else {
            result->admin1[0] = '\0';
        }

        // Extract timezone
        cJSON *timezone = cJSON_GetObjectItem(item, "timezone");
        if (timezone && cJSON_IsString(timezone)) {
            snprintf(result->timezone, sizeof(result->timezone), "%s", timezone->valuestring);
        } else {
            result->timezone[0] = '\0';
        }

        // Extract coordinates
        cJSON *lat = cJSON_GetObjectItem(item, "latitude");
        cJSON *lon = cJSON_GetObjectItem(item, "longitude");
        if (lat && lon && cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            result->latitude = (float)lat->valuedouble;
            result->longitude = (float)lon->valuedouble;
        }

        // Extract population (optional)
        cJSON *population = cJSON_GetObjectItem(item, "population");
        if (population && cJSON_IsNumber(population)) {
            result->population = (uint32_t)population->valueint;
        } else {
            result->population = 0;
        }

        result->valid = true;
        result_idx++;
    }

    *result_count = result_idx;
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
