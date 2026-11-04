#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>

#include "esp_err.h"
#include "glucose_core.h"
#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Personal MVP adapter: one process-wide station. cb and ctx are retained for
 * later reconnect events, so the caller must provide process-lifetime storage. */
typedef void (*wifi_manager_state_cb_t)(glucose_display_state_t state, void *ctx);

esp_err_t wifi_manager_prepare(void);
esp_err_t wifi_manager_try_candidate(const runtime_config_t *candidate,
                                     const runtime_config_t *fallback,
                                     uint32_t timeout_ms);
esp_err_t wifi_manager_connect_and_sync(const runtime_config_t *config,
                                        wifi_manager_state_cb_t cb,
                                        void *ctx,
                                        uint32_t wifi_timeout_ms,
                                        uint32_t sntp_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
