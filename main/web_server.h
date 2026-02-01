/*
 * Web Server Module
 * Provides HTTP server with REST API for device configuration
 * and developer tools interface
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Initialize and start the web server
 *
 * Mounts SPIFFS, starts HTTP server on port 80,
 * and registers all REST API endpoints
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t start_webserver(void);

/**
 * @brief Stop the web server and unmount SPIFFS
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stop_webserver(void);

/**
 * @brief Get the HTTP server handle
 *
 * @return httpd_handle_t Server handle or NULL if not running
 */
httpd_handle_t get_webserver_handle(void);

#endif // WEB_SERVER_H
