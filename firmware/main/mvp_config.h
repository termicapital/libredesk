#ifndef MVP_CONFIG_H
#define MVP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build-time prototype seam for the personal MVP. The Wi-Fi password and
 * revocable device token are embedded in the firmware binary; this is
 * acceptable only for the personal MVP and must be replaced after proof.
 */
bool mvp_config_build(runtime_config_t *config_out,
                      const char *wifi_ssid, size_t wifi_ssid_capacity,
                      const char *wifi_password, size_t wifi_password_capacity,
                      const char *bridge_origin, size_t bridge_origin_capacity,
                      const char *device_token, size_t device_token_capacity);

/* Load the local compile-time prototype values from mvp_secrets.h. */
bool mvp_config_load(runtime_config_t *config_out);

#ifdef __cplusplus
}
#endif

#endif
