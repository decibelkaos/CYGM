/*
 * heartbeat.h
 *
 * Lightweight device online heartbeat — pings cygm.me/api/heartbeat.php
 * with the device MAC as an anonymous identifier.
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Send a heartbeat ping to the server (HTTP GET, ~500ms, ~3KB heap).
 * Caller must hold network_mutex.
 */
esp_err_t heartbeat_send(void);

#ifdef __cplusplus
}
#endif

#endif // HEARTBEAT_H
