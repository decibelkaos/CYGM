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
#include "esp_system.h"

static const char *TAG = "GLUCOSE_HISTORY";

// NVS key for glucose history
#define NVS_NAMESPACE "cygm"
#define NVS_HISTORY_KEY "glucose_hist"

// Compact NVS serialization format (1,448 bytes max vs 4,616 raw)
// Delta-encodes timestamps as minutes-ago from newest reading
#define NVS_FORMAT_VERSION   1
#define NVS_HISTORY_MAX_SIZE (8 + GLUCOSE_HISTORY_MAX_POINTS * 5)  // 1,448 bytes

typedef struct __attribute__((packed)) {
    uint8_t  version;       // Format version (1)
    uint8_t  entry_count;   // Number of valid entries stored (0-255)
    uint16_t head;          // Circular buffer head index
    int32_t  base_ts;       // Unix SECONDS of newest reading
} nvs_history_header_t;

typedef struct __attribute__((packed)) {
    uint16_t minutes_ago;   // Minutes before base_ts (0-65535 ≈ 45 days)
    uint16_t glucose;       // mg/dL value
    uint8_t  flags;         // bit 0 = valid
} nvs_history_entry_t;

// Old namespace/key for legacy cleanup
#define NVS_OLD_NAMESPACE "storage"

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

    // One-time cleanup: erase legacy blob from old "storage" namespace
    nvs_handle_t old_handle;
    if (nvs_open(NVS_OLD_NAMESPACE, NVS_READWRITE, &old_handle) == ESP_OK) {
        if (nvs_erase_key(old_handle, NVS_HISTORY_KEY) == ESP_OK) {
            nvs_commit(old_handle);
            ESP_LOGI(TAG, "Erased legacy history from '%s' namespace", NVS_OLD_NAMESPACE);
        }
        nvs_close(old_handle);
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

    ESP_LOGI(TAG, "Added reading: %d mg/dL at slot %d (timestamp: %ld)",
             glucose_value, g_history.head, (long)timestamp);

    // Advance head (circular)
    g_history.head = (g_history.head + 1) % GLUCOSE_HISTORY_MAX_POINTS;

    // Update count (max 24)
    if (g_history.count < GLUCOSE_HISTORY_MAX_POINTS) {
        g_history.count++;
    }

    // Save to NVS every 4 readings (~20 min) to reduce flash wear
    static int save_counter = 0;
    save_counter++;
    if (save_counter >= 4) {
        esp_err_t save_ret = glucose_history_save_to_nvs();
        if (save_ret == ESP_OK) {
            save_counter = 0;
        } else {
            // Retry on next reading instead of waiting another 4 cycles
            save_counter = 3;
        }
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
    if (g_history.count == 0) {
        return ESP_OK;  // Nothing to save
    }

    // Find newest reading to use as base timestamp
    int32_t base_ts = 0;
    for (int i = 0; i < g_history.count; i++) {
        const glucose_history_point_t *pt = glucose_history_get_at(i);
        if (pt && pt->valid) {
            int32_t ts_sec = (int32_t)(pt->timestamp / 1000);
            if (ts_sec > base_ts) base_ts = ts_sec;
        }
    }

    if (base_ts == 0) {
        ESP_LOGW(TAG, "No valid timestamps to save");
        return ESP_OK;
    }

    // Serialize into compact format — heap allocated to avoid stack overflow
    // (NVS_HISTORY_MAX_SIZE=1448 bytes plus NVS library internals exceed 5KB stack)
    uint8_t *buf = malloc(NVS_HISTORY_MAX_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate NVS save buffer");
        return ESP_ERR_NO_MEM;
    }
    nvs_history_header_t *hdr = (nvs_history_header_t *)buf;
    hdr->version = NVS_FORMAT_VERSION;
    hdr->entry_count = 0;
    hdr->head = (uint16_t)g_history.head;
    hdr->base_ts = base_ts;

    nvs_history_entry_t *entries = (nvs_history_entry_t *)(buf + sizeof(nvs_history_header_t));
    int count = 0;

    for (int i = 0; i < g_history.count; i++) {
        const glucose_history_point_t *pt = glucose_history_get_at(i);
        if (pt && pt->valid) {
            int32_t ts_sec = (int32_t)(pt->timestamp / 1000);
            int32_t diff_sec = base_ts - ts_sec;
            int32_t diff_min = (diff_sec > 0) ? (diff_sec / 60) : 0;
            uint16_t minutes_ago = (diff_min > 65535) ? 65535 : (uint16_t)diff_min;

            entries[count].minutes_ago = minutes_ago;
            entries[count].glucose = (uint16_t)pt->glucose;
            entries[count].flags = 1;  // valid
            count++;
        }
    }
    hdr->entry_count = (uint8_t)((count > 255) ? 255 : count);

    size_t blob_size = sizeof(nvs_history_header_t) + count * sizeof(nvs_history_entry_t);

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for history save: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    ret = nvs_set_blob(nvs_handle, NVS_HISTORY_KEY, buf, blob_size);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "History saved: %d entries, %u bytes", count, (unsigned)blob_size);
        } else {
            ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "NVS write failed: %s (blob=%u bytes)", esp_err_to_name(ret), (unsigned)blob_size);
    }

    nvs_close(nvs_handle);
    free(buf);
    return ret;
}

esp_err_t glucose_history_load_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Query blob size first
    size_t blob_size = 0;
    ret = nvs_get_blob(nvs_handle, NVS_HISTORY_KEY, NULL, &blob_size);
    if (ret != ESP_OK || blob_size < sizeof(nvs_history_header_t)) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Cap at max expected size
    if (blob_size > NVS_HISTORY_MAX_SIZE) {
        ESP_LOGW(TAG, "NVS blob too large (%u), ignoring", (unsigned)blob_size);
        nvs_close(nvs_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc(NVS_HISTORY_MAX_SIZE);
    if (!buf) {
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }
    ret = nvs_get_blob(nvs_handle, NVS_HISTORY_KEY, buf, &blob_size);
    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    // Parse header
    nvs_history_header_t *hdr = (nvs_history_header_t *)buf;
    if (hdr->version != NVS_FORMAT_VERSION) {
        ESP_LOGW(TAG, "Unknown NVS format version %d, starting fresh", hdr->version);
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }

    int entry_count = hdr->entry_count;
    size_t expected_size = sizeof(nvs_history_header_t) + entry_count * sizeof(nvs_history_entry_t);
    if (blob_size < expected_size || entry_count > GLUCOSE_HISTORY_MAX_POINTS) {
        ESP_LOGE(TAG, "NVS history corrupted (entries=%d, blob=%u, expected=%u)",
                 entry_count, (unsigned)blob_size, (unsigned)expected_size);
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    if (hdr->head >= GLUCOSE_HISTORY_MAX_POINTS) {
        ESP_LOGE(TAG, "NVS history corrupted (head=%d)", hdr->head);
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    // Deserialize into circular buffer
    memset(&g_history, 0, sizeof(glucose_history_t));
    nvs_history_entry_t *entries = (nvs_history_entry_t *)(buf + sizeof(nvs_history_header_t));
    int64_t base_ts_ms = (int64_t)hdr->base_ts * 1000;

    for (int i = 0; i < entry_count; i++) {
        if (!(entries[i].flags & 0x01)) continue;  // Skip invalid entries

        int64_t ts_ms = base_ts_ms - ((int64_t)entries[i].minutes_ago * 60 * 1000);
        g_history.readings[i].timestamp = ts_ms;
        g_history.readings[i].glucose = (int)entries[i].glucose;
        g_history.readings[i].valid = true;
    }

    g_history.count = entry_count;
    g_history.head = (int)hdr->head;

    ESP_LOGI(TAG, "Loaded %d readings from NVS (compact format, %u bytes)", entry_count, (unsigned)blob_size);
    free(buf);
    return ESP_OK;
}

static void glucose_history_shutdown_handler(void) {
    if (g_initialized && g_history.count > 0) {
        glucose_history_save_to_nvs();
    }
}

void glucose_history_register_shutdown_handler(void) {
    esp_register_shutdown_handler(glucose_history_shutdown_handler);
}
