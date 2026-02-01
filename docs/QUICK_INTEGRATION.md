# Quick Integration Snippet

**Copy/paste code to integrate Dexcom API into main.c**

---

## Step 1: Add Include at Top of main.c

```c
#include "dexcom_api.h"
```

---

## Step 2: Add Global Variables (around line 110)

```c
// Dexcom/CGM data
static int current_glucose = 0;
static dexcom_trend_t glucose_trend = TREND_FLAT;
static time_t glucose_timestamp = 0;
static bool glucose_valid = false;

// UI widgets for right card (if not already declared)
// These will be set when home screen is created
static lv_obj_t *glucose_value_label = NULL;
static lv_obj_t *glucose_trend_label = NULL;
static lv_obj_t *glucose_time_label = NULL;
```

---

## Step 3: Add Helper Function for Trend Arrows

```c
// Around line 400, with other helper functions
static const char* get_trend_arrow(dexcom_trend_t trend) {
    switch (trend) {
        case TREND_DOUBLE_UP:       return "↑↑";
        case TREND_SINGLE_UP:       return "↑";
        case TREND_FORTY_FIVE_UP:   return "↗";
        case TREND_FLAT:            return "→";
        case TREND_FORTY_FIVE_DOWN: return "↘";
        case TREND_SINGLE_DOWN:     return "↓";
        case TREND_DOUBLE_DOWN:     return "↓↓";
        default:                    return "?";
    }
}
```

---

## Step 4: Initialize Dexcom API in app_main()

Find `app_main()` function (around line 2600) and add after WiFi initialization:

```c
void app_main(void) {
    // ... existing code ...

    // Initialize NVS
    nvs_config_init();

    // ... WiFi initialization ...

    // Initialize Dexcom API (ADD THIS)
    ESP_LOGI(TAG, "Initializing Dexcom API");
    dexcom_api_init();

    // Check if already authenticated
    if (dexcom_is_authenticated()) {
        ESP_LOGI(TAG, "Dexcom: Already authenticated");
        // Could start glucose update task here
    } else {
        ESP_LOGI(TAG, "Dexcom: Not authenticated yet");
        // User needs to go through OAuth flow
    }

    // ... rest of app_main ...
}
```

---

## Step 5: Optional - Add Test Code to Verify API

Add this temporary test function to verify the API works:

```c
// Temporary test function - remove after testing
static void test_dexcom_api(void) {
    ESP_LOGI(TAG, "=== Testing Dexcom API ===");

    // Test 1: Generate auth URL
    char auth_url[512];
    esp_err_t err = dexcom_get_auth_url(auth_url, sizeof(auth_url));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Auth URL generated successfully");
        ESP_LOGI(TAG, "  URL: %s", auth_url);
        ESP_LOGI(TAG, "  Length: %d", strlen(auth_url));
    } else {
        ESP_LOGE(TAG, "✗ Failed to generate auth URL: %s", esp_err_to_name(err));
    }

    // Test 2: Check authentication status
    bool is_authed = dexcom_is_authenticated();
    ESP_LOGI(TAG, "%s Authentication status: %s",
             is_authed ? "✓" : "✗",
             is_authed ? "Authenticated" : "Not authenticated");

    // Test 3: Try to fetch glucose (will fail if not authenticated)
    if (is_authed) {
        dexcom_glucose_t glucose;
        err = dexcom_fetch_glucose(&glucose);
        if (err == ESP_OK && glucose.valid) {
            ESP_LOGI(TAG, "✓ Glucose fetch successful");
            ESP_LOGI(TAG, "  Value: %d mg/dL", glucose.value);
            ESP_LOGI(TAG, "  Trend: %s", get_trend_arrow(glucose.trend));

            time_t now;
            time(&now);
            int minutes_ago = (now - glucose.timestamp) / 60;
            ESP_LOGI(TAG, "  Time: %d minutes ago", minutes_ago);
        } else {
            ESP_LOGE(TAG, "✗ Glucose fetch failed: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "=== Dexcom API Test Complete ===");
}
```

Call it from `app_main()`:
```c
// After dexcom_api_init()
test_dexcom_api();  // Remove this after testing
```

---

## Step 6: Add OAuth Flow UI (When Ready)

When you create the Dexcom settings screen, add these handlers:

```c
// Button handler: "Connect Dexcom"
static void connect_dexcom_btn_clicked(lv_event_t *e) {
    // Generate auth URL
    char auth_url[512];
    if (dexcom_get_auth_url(auth_url, sizeof(auth_url)) == ESP_OK) {
        // Display URL on screen for user to open
        // Show keyboard for code entry
        // ... UI code ...
    }
}

// Keyboard handler: User entered auth code
static void auth_code_entered(const char *code) {
    ESP_LOGI(TAG, "Exchanging auth code for tokens...");

    esp_err_t err = dexcom_exchange_code_for_token(code);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully authenticated with Dexcom!");
        // Update UI to show "Connected"
        // Return to CGM settings screen
        // Could immediately fetch glucose to test
    } else {
        ESP_LOGE(TAG, "Authentication failed: %s", esp_err_to_name(err));
        // Show error message to user
    }
}
```

---

## Step 7: Add Glucose Update Task (When Ready)

```c
// Task to periodically fetch glucose
static void glucose_update_task(void *pvParameters) {
    ESP_LOGI(TAG, "Glucose update task started");

    while (1) {
        if (dexcom_is_authenticated()) {
            dexcom_glucose_t glucose;
            esp_err_t err = dexcom_fetch_glucose(&glucose);

            if (err == ESP_OK && glucose.valid) {
                // Update global variables
                current_glucose = glucose.value;
                glucose_trend = glucose.trend;
                glucose_timestamp = glucose.timestamp;
                glucose_valid = true;

                // Update UI
                if (glucose_value_label != NULL && lvgl_port_lock(0)) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d", glucose.value);
                    lv_label_set_text(glucose_value_label, buf);

                    lv_label_set_text(glucose_trend_label, get_trend_arrow(glucose.trend));

                    time_t now;
                    time(&now);
                    int min_ago = (now - glucose.timestamp) / 60;
                    snprintf(buf, sizeof(buf), "%d min ago", min_ago);
                    lv_label_set_text(glucose_time_label, buf);

                    // Color coding
                    lv_color_t color;
                    if (glucose.value < 70) {
                        color = lv_color_hex(0xFF0000);  // Red - LOW
                    } else if (glucose.value > 180) {
                        color = lv_color_hex(0xFFFF00);  // Yellow - HIGH
                    } else {
                        color = lv_color_hex(0x00FF00);  // Green - IN RANGE
                    }
                    lv_obj_set_style_text_color(glucose_value_label, color, 0);

                    lvgl_port_unlock();
                }

                ESP_LOGI(TAG, "Glucose updated: %d mg/dL %s (%d min ago)",
                         glucose.value, get_trend_arrow(glucose.trend), min_ago);
            } else {
                ESP_LOGW(TAG, "Failed to fetch glucose: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGD(TAG, "Not authenticated, skipping glucose fetch");
        }

        // Wait 5 minutes
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
}

// Start the task (call after WiFi connects and authentication succeeds)
static void start_glucose_updates(void) {
    static TaskHandle_t task_handle = NULL;
    if (task_handle == NULL) {
        xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &task_handle);
        ESP_LOGI(TAG, "Glucose update task created");
    }
}
```

---

## Complete Integration Checklist

- [ ] Add `#include "dexcom_api.h"`
- [ ] Add global variables for glucose data
- [ ] Add `get_trend_arrow()` helper function
- [ ] Call `dexcom_api_init()` in `app_main()`
- [ ] Add test function to verify API works
- [ ] Build and flash, verify serial output shows API initializing
- [ ] Test auth URL generation
- [ ] Create UI screens for OAuth flow
- [ ] Add auth code entry and token exchange
- [ ] Update right card with glucose labels
- [ ] Add glucose update task
- [ ] Test end-to-end flow

---

## Expected Serial Output After Integration

```
I (1234) MAIN: Initializing Dexcom API
I (1234) DEXCOM_API: Initializing Dexcom API client
I (1234) DEXCOM_API: No saved tokens found, need to authenticate
I (1234) MAIN: Dexcom: Not authenticated yet
I (1235) MAIN: === Testing Dexcom API ===
I (1236) DEXCOM_API: Generated auth URL (length: 234)
I (1236) MAIN: ✓ Auth URL generated successfully
I (1236) MAIN:   URL: https://sandbox-api.dexcom.com/v2/oauth2/login?client_id=...
I (1236) MAIN: ✗ Authentication status: Not authenticated
I (1236) MAIN: === Dexcom API Test Complete ===
```

After authentication:
```
I (5678) MAIN: Exchanging auth code for tokens...
I (5678) DEXCOM_API: Exchanging authorization code for access token
I (6123) DEXCOM_API: Token exchange response: HTTP 200, length: 456
I (6124) DEXCOM_API: Successfully authenticated! Token expires in 7200 seconds
I (6125) NVS_CONFIG: Dexcom OAuth tokens saved to NVS
I (6125) NVS_CONFIG: CGM type saved to NVS: dexcom
I (6125) MAIN: Successfully authenticated with Dexcom!
```

---

**That's it!** Copy the relevant sections and integrate step-by-step.
