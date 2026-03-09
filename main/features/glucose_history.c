/*
 * glucose_history.c
 *
 * Glucose reading history management implementation
 */

#include "glucose_history.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "GLUCOSE_HISTORY";

// NVS key for glucose history
#define NVS_NAMESPACE "storage"
#define NVS_HISTORY_KEY "glucose_hist"

// Global history buffer
static glucose_history_t g_history = {0};
static bool g_initialized = false;

esp_err_t glucose_history_init(void) {
    if (g_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Clear the buffer
    memset(&g_history, 0, sizeof(glucose_history_t));

    // Try to load from NVS
    esp_err_t ret = glucose_history_load_from_nvs();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %d glucose readings from NVS", g_history.count);
    } else {
        ESP_LOGI(TAG, "No history found in NVS, starting fresh");
    }

    g_initialized = true;
    return ESP_OK;
}

void glucose_history_add(int glucose_value, int64_t timestamp) {
    if (!g_initialized) {
        ESP_LOGW(TAG, "Not initialized, initializing now");
        glucose_history_init();
    }

    // Add to circular buffer at head position
    g_history.readings[g_history.head].timestamp = timestamp;
    g_history.readings[g_history.head].glucose = glucose_value;
    g_history.readings[g_history.head].valid = true;

    ESP_LOGI(TAG, "Added reading: %d mg/dL at slot %d (timestamp: %lld)",
             glucose_value, g_history.head, timestamp);

    // Advance head (circular)
    g_history.head = (g_history.head + 1) % GLUCOSE_HISTORY_MAX_POINTS;

    // Update count (max 24)
    if (g_history.count < GLUCOSE_HISTORY_MAX_POINTS) {
        g_history.count++;
    }

    // Save to NVS every 4 readings (once per hour) to reduce flash wear
    static int save_counter = 0;
    save_counter++;
    if (save_counter >= 4) {
        glucose_history_save_to_nvs();
        save_counter = 0;
    }
}

const glucose_history_t* glucose_history_get(void) {
    return &g_history;
}

const glucose_history_point_t* glucose_history_get_at(int index) {
    if (index < 0 || index >= g_history.count) {
        return NULL;
    }

    // Calculate actual position in circular buffer
    // Oldest reading is at (head - count), newest is at (head - 1)
    int actual_index = (g_history.head - g_history.count + index + GLUCOSE_HISTORY_MAX_POINTS) % GLUCOSE_HISTORY_MAX_POINTS;

    if (!g_history.readings[actual_index].valid) {
        return NULL;
    }

    return &g_history.readings[actual_index];
}

int glucose_history_get_count(void) {
    return g_history.count;
}

const glucose_history_point_t* glucose_history_find_closest(int64_t timestamp) {
    if (g_history.count == 0) {
        return NULL;
    }

    const glucose_history_point_t *closest = NULL;
    int64_t min_diff = INT64_MAX;

    // Search all valid readings for the closest timestamp
    for (int i = 0; i < g_history.count; i++) {
        const glucose_history_point_t *point = glucose_history_get_at(i);
        if (point != NULL && point->valid) {
            int64_t diff = llabs(point->timestamp - timestamp);
            if (diff < min_diff) {
                min_diff = diff;
                closest = point;
            }
        }
    }

    return closest;
}

int glucose_history_get_range(int minutes_back, glucose_history_point_t *points_out, int max_points) {
    if (g_history.count == 0 || points_out == NULL || max_points <= 0) {
        return 0;
    }

    // Get current time in milliseconds
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    int64_t cutoff_ms = now_ms - ((int64_t)minutes_back * 60 * 1000);

    int filled = 0;
    for (int i = 0; i < g_history.count && filled < max_points; i++) {
        const glucose_history_point_t *point = glucose_history_get_at(i);
        if (point != NULL && point->valid && point->timestamp >= cutoff_ms) {
            points_out[filled] = *point;
            filled++;
        }
    }

    return filled;
}

void glucose_history_clear(void) {
    memset(&g_history, 0, sizeof(glucose_history_t));
    ESP_LOGI(TAG, "History cleared");

    // Clear from NVS as well
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_HISTORY_KEY);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

esp_err_t glucose_history_save_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing history");
        return ret;
    }

    ret = nvs_set_blob(nvs_handle, NVS_HISTORY_KEY, &g_history, sizeof(glucose_history_t));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Glucose history saved to NVS (%d readings)", g_history.count);
        } else {
            ESP_LOGE(TAG, "Failed to commit history to NVS");
        }
    } else {
        ESP_LOGE(TAG, "Failed to write history to NVS");
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t glucose_history_load_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading history");
        return ret;
    }

    size_t required_size = sizeof(glucose_history_t);
    ret = nvs_get_blob(nvs_handle, NVS_HISTORY_KEY, &g_history, &required_size);

    if (ret != ESP_OK || required_size != sizeof(glucose_history_t)) {
        ESP_LOGW(TAG, "Glucose history not found or corrupted in NVS");
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Validate loaded data
    if (g_history.count > GLUCOSE_HISTORY_MAX_POINTS || g_history.head >= GLUCOSE_HISTORY_MAX_POINTS) {
        ESP_LOGE(TAG, "Loaded history is corrupted (count: %d, head: %d)", g_history.count, g_history.head);
        memset(&g_history, 0, sizeof(glucose_history_t));
        nvs_close(nvs_handle);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Glucose history loaded from NVS (%d readings)", g_history.count);
    nvs_close(nvs_handle);
    return ESP_OK;
}
