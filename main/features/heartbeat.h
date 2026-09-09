/*
 * heartbeat.h
 *
 * Lightweight device online heartbeat — pings cygm.me/api/heartbeat.php over
 * HTTPS with a pseudonymous device identifier.
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The device's pseudonymous identifier: 32 lowercase hex characters, from 16
 * random bytes generated the first time this is called and kept in NVS.
 *
 * Returns a pointer to a static buffer; never NULL. A factory reset erases the
 * whole NVS partition, so the identifier is regenerated afterwards — that is
 * the intended privacy behaviour, not a bug. If NVS is unwritable the caller
 * still gets a valid identifier, stable for this boot only.
 *
 * Never log the return value.
 */
const char *cygm_device_id(void);

/**
 * Send a heartbeat ping to the server (HTTPS GET).
 *
 * Optional by design: it acquires network_mutex itself when the caller does
 * not already hold it, and skips the cycle rather than waiting or forcing an
 * allocation if the mutex is busy or contiguous heap is short. It never
 * touches glucose state.
 */
esp_err_t heartbeat_send(void);

#ifdef __cplusplus
}
#endif

#endif // HEARTBEAT_H
