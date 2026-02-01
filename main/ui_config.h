/*
 * UI Configuration Module
 * Manages custom UI layouts and icon designs
 * Stores configurations in NVS and provides rendering functions
 */

#ifndef UI_CONFIG_H
#define UI_CONFIG_H

#include "esp_err.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

// ==================== Data Structures ====================

/**
 * @brief UI element configuration
 */
typedef struct {
    char id[32];           // Element identifier (e.g., "clock", "glucose")
    int16_t x, y;          // Position
    int16_t width, height; // Size
    char font[32];         // Font name
    uint32_t color;        // RGB hex color
    bool visible;          // Visibility flag
} ui_element_config_t;

/**
 * @brief Icon path types
 */
typedef enum {
    ICON_PATH_LINE,
    ICON_PATH_ARC,
    ICON_PATH_RECT,
    ICON_PATH_CIRCLE
} icon_path_type_t;

/**
 * @brief Icon drawing path
 */
typedef struct {
    icon_path_type_t type;
    lv_point_t points[10]; // Coordinate array
    uint8_t point_count;
    uint16_t width;        // Line width
    uint32_t color;        // RGB hex color
    bool round_caps;       // Rounded line ends
    int16_t radius;        // For circles/arcs
    int16_t start_angle;   // For arcs (degrees)
    int16_t end_angle;     // For arcs (degrees)
} icon_path_t;

/**
 * @brief Icon design
 */
typedef struct {
    char id[32];
    uint16_t canvas_width, canvas_height;
    icon_path_t paths[20];
    uint8_t path_count;
} icon_design_t;

// ==================== UI Layout Functions ====================

/**
 * @brief Initialize UI configuration system
 *
 * @return ESP_OK on success
 */
esp_err_t ui_config_init(void);

/**
 * @brief Check if custom layout exists
 *
 * @return true if custom layout is stored, false otherwise
 */
bool ui_config_has_custom_layout(void);

/**
 * @brief Load UI element configuration
 *
 * @param element_id Element identifier
 * @param config Output configuration
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t ui_config_load_element(const char* element_id, ui_element_config_t* config);

/**
 * @brief Save UI element configuration
 *
 * @param config Element configuration
 * @return ESP_OK on success
 */
esp_err_t ui_config_save_element(const ui_element_config_t* config);

/**
 * @brief Get all UI element configurations as JSON
 *
 * @return JSON string (caller must free), or NULL on error
 */
char* ui_config_get_layout_json(void);

/**
 * @brief Save UI layout from JSON
 *
 * @param json_str JSON string containing layout
 * @return ESP_OK on success
 */
esp_err_t ui_config_save_layout_json(const char* json_str);

/**
 * @brief Reset to default layout
 *
 * @return ESP_OK on success
 */
esp_err_t ui_config_reset_layout(void);

/**
 * @brief Apply custom layout to LVGL objects
 *
 * This function should be called after UI initialization
 * to apply saved custom positions/sizes
 *
 * @return ESP_OK on success
 */
esp_err_t ui_config_apply_layout(void);

// ==================== Icon Design Functions ====================

/**
 * @brief Load icon design
 *
 * @param icon_type Type category ("trend" or "weather")
 * @param icon_id Icon identifier (e.g., "single_up", "cloudy")
 * @param design Output design structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t ui_config_load_icon(const char* icon_type, const char* icon_id, icon_design_t* design);

/**
 * @brief Save icon design
 *
 * @param icon_type Type category ("trend" or "weather")
 * @param design Icon design structure
 * @return ESP_OK on success
 */
esp_err_t ui_config_save_icon(const char* icon_type, const icon_design_t* design);

/**
 * @brief Get icon design as JSON
 *
 * @param icon_type Type category ("trend" or "weather")
 * @param icon_id Icon identifier
 * @return JSON string (caller must free), or NULL on error
 */
char* ui_config_get_icon_json(const char* icon_type, const char* icon_id);

/**
 * @brief Save icon design from JSON
 *
 * @param icon_type Type category ("trend" or "weather")
 * @param json_str JSON string containing design
 * @return ESP_OK on success
 */
esp_err_t ui_config_save_icon_json(const char* icon_type, const char* json_str);

/**
 * @brief Render icon design on LVGL canvas
 *
 * @param canvas LVGL canvas object
 * @param design Icon design to render
 * @return ESP_OK on success
 */
esp_err_t ui_config_render_icon(lv_obj_t* canvas, const icon_design_t* design);

#endif // UI_CONFIG_H
