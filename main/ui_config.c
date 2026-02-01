/*
 * UI Configuration Module
 * Custom UI layouts and icon designs with NVS storage
 */

#include "ui_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "UIConfig";

// NVS namespaces
#define NVS_NAMESPACE_UI "ui_config"
#define NVS_NAMESPACE_ICONS "ui_icons"

// NVS keys
#define NVS_KEY_CUSTOM_LAYOUT "has_custom"
#define NVS_KEY_LAYOUT_JSON "layout_json"

// ==================== Default UI Elements ====================

static const ui_element_config_t default_elements[] = {
    {
        .id = "glucose_value",
        .x = 50, .y = 60,
        .width = 220, .height = 80,
        .font = "montserrat_48",
        .color = 0xFFFFFF,
        .visible = true
    },
    {
        .id = "trend_arrow",
        .x = 240, .y = 70,
        .width = 40, .height = 40,
        .font = "",
        .color = 0x3B82F6,
        .visible = true
    },
    {
        .id = "time_label",
        .x = 10, .y = 10,
        .width = 140, .height = 40,
        .font = "montserrat_24",
        .color = 0xFFFFFF,
        .visible = true
    },
    {
        .id = "date_label",
        .x = 10, .y = 210,
        .width = 200, .height = 24,
        .font = "montserrat_16",
        .color = 0xAAAAAA,
        .visible = true
    },
    {
        .id = "weather_icon",
        .x = 260, .y = 10,
        .width = 50, .height = 50,
        .font = "",
        .color = 0xFDB462,
        .visible = true
    },
    {
        .id = "weather_temp",
        .x = 260, .y = 210,
        .width = 50, .height = 24,
        .font = "montserrat_16",
        .color = 0xFFFFFF,
        .visible = true
    },
    {
        .id = "delta_label",
        .x = 50, .y = 145,
        .width = 100, .height = 30,
        .font = "montserrat_20",
        .color = 0xAAAAAA,
        .visible = true
    },
    {
        .id = "last_update",
        .x = 50, .y = 175,
        .width = 200, .height = 24,
        .font = "montserrat_14",
        .color = 0x888888,
        .visible = true
    }
};

#define DEFAULT_ELEMENT_COUNT (sizeof(default_elements) / sizeof(default_elements[0]))

// ==================== UI Layout Functions ====================

esp_err_t ui_config_init(void) {
    ESP_LOGI(TAG, "Initializing UI configuration system");
    return ESP_OK;
}

bool ui_config_has_custom_layout(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_UI, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t has_custom = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_CUSTOM_LAYOUT, &has_custom);
    nvs_close(nvs_handle);

    return (err == ESP_OK && has_custom == 1);
}

esp_err_t ui_config_load_element(const char* element_id, ui_element_config_t* config) {
    if (element_id == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Try to load from NVS first
    if (ui_config_has_custom_layout()) {
        char* json_str = ui_config_get_layout_json();
        if (json_str != NULL) {
            cJSON* root = cJSON_Parse(json_str);
            free(json_str);

            if (root != NULL) {
                cJSON* elements = cJSON_GetObjectItem(root, "elements");
                if (elements != NULL) {
                    cJSON* element = NULL;
                    cJSON_ArrayForEach(element, elements) {
                        cJSON* id = cJSON_GetObjectItem(element, "id");
                        if (id != NULL && strcmp(id->valuestring, element_id) == 0) {
                            // Found matching element
                            strncpy(config->id, element_id, sizeof(config->id) - 1);

                            cJSON* item;
                            if ((item = cJSON_GetObjectItem(element, "x")) != NULL) config->x = item->valueint;
                            if ((item = cJSON_GetObjectItem(element, "y")) != NULL) config->y = item->valueint;
                            if ((item = cJSON_GetObjectItem(element, "width")) != NULL) config->width = item->valueint;
                            if ((item = cJSON_GetObjectItem(element, "height")) != NULL) config->height = item->valueint;
                            if ((item = cJSON_GetObjectItem(element, "font")) != NULL) {
                                strncpy(config->font, item->valuestring, sizeof(config->font) - 1);
                            }
                            if ((item = cJSON_GetObjectItem(element, "color")) != NULL) config->color = (uint32_t)item->valueint;
                            if ((item = cJSON_GetObjectItem(element, "visible")) != NULL) config->visible = cJSON_IsTrue(item);

                            cJSON_Delete(root);
                            return ESP_OK;
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
    }

    // Fall back to default
    for (int i = 0; i < DEFAULT_ELEMENT_COUNT; i++) {
        if (strcmp(default_elements[i].id, element_id) == 0) {
            memcpy(config, &default_elements[i], sizeof(ui_element_config_t));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t ui_config_save_element(const ui_element_config_t* config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Load existing layout
    char* existing_json = ui_config_get_layout_json();
    cJSON* root;

    if (existing_json != NULL) {
        root = cJSON_Parse(existing_json);
        free(existing_json);
    } else {
        root = cJSON_CreateObject();
        cJSON_AddArrayToObject(root, "elements");
    }

    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON* elements = cJSON_GetObjectItem(root, "elements");
    if (elements == NULL) {
        elements = cJSON_AddArrayToObject(root, "elements");
    }

    // Find and update or add new element
    cJSON* element = NULL;
    cJSON* found = NULL;
    cJSON_ArrayForEach(element, elements) {
        cJSON* id = cJSON_GetObjectItem(element, "id");
        if (id != NULL && strcmp(id->valuestring, config->id) == 0) {
            found = element;
            break;
        }
    }

    if (found == NULL) {
        found = cJSON_CreateObject();
        cJSON_AddItemToArray(elements, found);
    }

    // Update element
    cJSON_DeleteItemFromObject(found, "id");
    cJSON_DeleteItemFromObject(found, "x");
    cJSON_DeleteItemFromObject(found, "y");
    cJSON_DeleteItemFromObject(found, "width");
    cJSON_DeleteItemFromObject(found, "height");
    cJSON_DeleteItemFromObject(found, "font");
    cJSON_DeleteItemFromObject(found, "color");
    cJSON_DeleteItemFromObject(found, "visible");

    cJSON_AddStringToObject(found, "id", config->id);
    cJSON_AddNumberToObject(found, "x", config->x);
    cJSON_AddNumberToObject(found, "y", config->y);
    cJSON_AddNumberToObject(found, "width", config->width);
    cJSON_AddNumberToObject(found, "height", config->height);
    cJSON_AddStringToObject(found, "font", config->font);
    cJSON_AddNumberToObject(found, "color", config->color);
    cJSON_AddBoolToObject(found, "visible", config->visible);

    // Save to NVS
    char* json_str = cJSON_PrintUnformatted(root);
    esp_err_t err = ui_config_save_layout_json(json_str);

    free(json_str);
    cJSON_Delete(root);

    return err;
}

char* ui_config_get_layout_json(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_UI, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Return default layout as JSON
        cJSON* root = cJSON_CreateObject();
        cJSON* elements = cJSON_AddArrayToObject(root, "elements");

        for (int i = 0; i < DEFAULT_ELEMENT_COUNT; i++) {
            cJSON* elem = cJSON_CreateObject();
            cJSON_AddStringToObject(elem, "id", default_elements[i].id);
            cJSON_AddNumberToObject(elem, "x", default_elements[i].x);
            cJSON_AddNumberToObject(elem, "y", default_elements[i].y);
            cJSON_AddNumberToObject(elem, "width", default_elements[i].width);
            cJSON_AddNumberToObject(elem, "height", default_elements[i].height);
            cJSON_AddStringToObject(elem, "font", default_elements[i].font);
            cJSON_AddNumberToObject(elem, "color", default_elements[i].color);
            cJSON_AddBoolToObject(elem, "visible", default_elements[i].visible);
            cJSON_AddItemToArray(elements, elem);
        }

        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return json_str;
    }

    // Get stored JSON length
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_LAYOUT_JSON, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return NULL;
    }

    char* json_str = malloc(required_size);
    if (json_str == NULL) {
        nvs_close(nvs_handle);
        return NULL;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_LAYOUT_JSON, json_str, &required_size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        free(json_str);
        return NULL;
    }

    return json_str;
}

esp_err_t ui_config_save_layout_json(const char* json_str) {
    if (json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_UI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_LAYOUT_JSON, json_str);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_CUSTOM_LAYOUT, 1);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t ui_config_reset_layout(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_UI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(nvs_handle, NVS_KEY_CUSTOM_LAYOUT);
    nvs_erase_key(nvs_handle, NVS_KEY_LAYOUT_JSON);
    err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    return err;
}

esp_err_t ui_config_apply_layout(void) {
    // This function will be implemented to update LVGL widgets
    // after integration with main.c
    ESP_LOGI(TAG, "Applying custom layout (stub)");
    return ESP_OK;
}

// ==================== Icon Design Functions ====================

esp_err_t ui_config_load_icon(const char* icon_type, const char* icon_id, icon_design_t* design) {
    if (icon_type == NULL || icon_id == NULL || design == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char* json_str = ui_config_get_icon_json(icon_type, icon_id);
    if (json_str == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Parse icon design
    cJSON* item;
    if ((item = cJSON_GetObjectItem(root, "id")) != NULL) {
        strncpy(design->id, item->valuestring, sizeof(design->id) - 1);
    }
    if ((item = cJSON_GetObjectItem(root, "canvas_width")) != NULL) {
        design->canvas_width = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "canvas_height")) != NULL) {
        design->canvas_height = item->valueint;
    }

    cJSON* paths = cJSON_GetObjectItem(root, "paths");
    if (paths != NULL) {
        design->path_count = 0;
        cJSON* path;
        cJSON_ArrayForEach(path, paths) {
            if (design->path_count >= 20) break;

            icon_path_t* p = &design->paths[design->path_count];

            cJSON* type = cJSON_GetObjectItem(path, "type");
            if (type != NULL) {
                if (strcmp(type->valuestring, "line") == 0) p->type = ICON_PATH_LINE;
                else if (strcmp(type->valuestring, "arc") == 0) p->type = ICON_PATH_ARC;
                else if (strcmp(type->valuestring, "rect") == 0) p->type = ICON_PATH_RECT;
                else if (strcmp(type->valuestring, "circle") == 0) p->type = ICON_PATH_CIRCLE;
            }

            cJSON* points = cJSON_GetObjectItem(path, "points");
            if (points != NULL) {
                p->point_count = 0;
                cJSON* point;
                cJSON_ArrayForEach(point, points) {
                    if (p->point_count >= 10) break;
                    if (cJSON_GetArraySize(point) >= 2) {
                        p->points[p->point_count].x = cJSON_GetArrayItem(point, 0)->valueint;
                        p->points[p->point_count].y = cJSON_GetArrayItem(point, 1)->valueint;
                        p->point_count++;
                    }
                }
            }

            if ((item = cJSON_GetObjectItem(path, "width")) != NULL) p->width = item->valueint;
            if ((item = cJSON_GetObjectItem(path, "color")) != NULL) {
                // Parse hex color string
                const char* color_str = item->valuestring;
                if (color_str[0] == '#') {
                    p->color = strtol(color_str + 1, NULL, 16);
                } else {
                    p->color = item->valueint;
                }
            }
            if ((item = cJSON_GetObjectItem(path, "round_caps")) != NULL) p->round_caps = cJSON_IsTrue(item);
            if ((item = cJSON_GetObjectItem(path, "radius")) != NULL) p->radius = item->valueint;
            if ((item = cJSON_GetObjectItem(path, "start_angle")) != NULL) p->start_angle = item->valueint;
            if ((item = cJSON_GetObjectItem(path, "end_angle")) != NULL) p->end_angle = item->valueint;

            design->path_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ui_config_save_icon(const char* icon_type, const icon_design_t* design) {
    if (icon_type == NULL || design == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Convert to JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", design->id);
    cJSON_AddNumberToObject(root, "canvas_width", design->canvas_width);
    cJSON_AddNumberToObject(root, "canvas_height", design->canvas_height);

    cJSON* paths = cJSON_AddArrayToObject(root, "paths");
    for (int i = 0; i < design->path_count; i++) {
        const icon_path_t* p = &design->paths[i];
        cJSON* path = cJSON_CreateObject();

        const char* type_str = "line";
        if (p->type == ICON_PATH_ARC) type_str = "arc";
        else if (p->type == ICON_PATH_RECT) type_str = "rect";
        else if (p->type == ICON_PATH_CIRCLE) type_str = "circle";
        cJSON_AddStringToObject(path, "type", type_str);

        cJSON* points = cJSON_AddArrayToObject(path, "points");
        for (int j = 0; j < p->point_count; j++) {
            cJSON* point = cJSON_CreateArray();
            cJSON_AddItemToArray(point, cJSON_CreateNumber(p->points[j].x));
            cJSON_AddItemToArray(point, cJSON_CreateNumber(p->points[j].y));
            cJSON_AddItemToArray(points, point);
        }

        cJSON_AddNumberToObject(path, "width", p->width);

        char color_str[8];
        snprintf(color_str, sizeof(color_str), "#%06X", (unsigned int)p->color);
        cJSON_AddStringToObject(path, "color", color_str);

        cJSON_AddBoolToObject(path, "round_caps", p->round_caps);
        if (p->type == ICON_PATH_ARC || p->type == ICON_PATH_CIRCLE) {
            cJSON_AddNumberToObject(path, "radius", p->radius);
            cJSON_AddNumberToObject(path, "start_angle", p->start_angle);
            cJSON_AddNumberToObject(path, "end_angle", p->end_angle);
        }

        cJSON_AddItemToArray(paths, path);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    esp_err_t err = ui_config_save_icon_json(icon_type, json_str);

    free(json_str);
    cJSON_Delete(root);

    return err;
}

char* ui_config_get_icon_json(const char* icon_type, const char* icon_id) {
    if (icon_type == NULL || icon_id == NULL) {
        return NULL;
    }

    // Validate lengths to prevent buffer overflow
    if (strlen(icon_type) > 16 || strlen(icon_id) > 32) {
        ESP_LOGE(TAG, "Icon type or ID too long");
        return NULL;
    }

    char key[64];
    snprintf(key, sizeof(key), "%s_%s", icon_type, icon_id);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ICONS, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return NULL;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return NULL;
    }

    char* json_str = malloc(required_size);
    if (json_str == NULL) {
        nvs_close(nvs_handle);
        return NULL;
    }

    err = nvs_get_str(nvs_handle, key, json_str, &required_size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        free(json_str);
        return NULL;
    }

    return json_str;
}

esp_err_t ui_config_save_icon_json(const char* icon_type, const char* json_str) {
    if (icon_type == NULL || json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Extract icon ID from JSON
    cJSON* root = cJSON_Parse(json_str);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* id = cJSON_GetObjectItem(root, "id");
    if (id == NULL || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Validate icon_id length to prevent buffer overflow
    if (strlen(id->valuestring) > 32) {
        ESP_LOGE(TAG, "Icon ID too long: %s", id->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char key[64];
    snprintf(key, sizeof(key), "%s_%s", icon_type, id->valuestring);
    cJSON_Delete(root);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ICONS, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, key, json_str);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t ui_config_render_icon(lv_obj_t* canvas, const icon_design_t* design) {
    if (canvas == NULL || design == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Render each path
    for (int i = 0; i < design->path_count; i++) {
        const icon_path_t* path = &design->paths[i];
        lv_color_t color = lv_color_hex(path->color);

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = color;
        line_dsc.width = path->width;
        line_dsc.round_start = path->round_caps;
        line_dsc.round_end = path->round_caps;

        switch (path->type) {
            case ICON_PATH_LINE:
                if (path->point_count >= 2) {
                    // LVGL expects array of points and count
                    lv_canvas_draw_line(canvas, path->points, path->point_count, &line_dsc);
                }
                break;

            case ICON_PATH_RECT:
                if (path->point_count >= 2) {
                    lv_draw_rect_dsc_t rect_dsc;
                    lv_draw_rect_dsc_init(&rect_dsc);
                    rect_dsc.bg_color = color;
                    rect_dsc.border_width = 0;

                    int w = path->points[1].x - path->points[0].x;
                    int h = path->points[1].y - path->points[0].y;
                    lv_canvas_draw_rect(canvas, path->points[0].x, path->points[0].y,
                                       w, h, &rect_dsc);
                }
                break;

            case ICON_PATH_CIRCLE:
            case ICON_PATH_ARC:
                if (path->point_count >= 1) {
                    lv_draw_arc_dsc_t arc_dsc;
                    lv_draw_arc_dsc_init(&arc_dsc);
                    arc_dsc.color = color;
                    arc_dsc.width = path->width;
                    arc_dsc.rounded = path->round_caps;

                    lv_canvas_draw_arc(canvas, path->points[0].x, path->points[0].y,
                                      path->radius, path->start_angle, path->end_angle,
                                      &arc_dsc);
                }
                break;
        }
    }

    return ESP_OK;
}
