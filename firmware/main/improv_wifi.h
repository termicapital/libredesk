#ifndef LIBRE_DESK_IMPROV_WIFI_H
#define LIBRE_DESK_IMPROV_WIFI_H

#include <stdbool.h>

#include "esp_err.h"
#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t improv_wifi_start(const runtime_config_t *current,
                            bool persisted_config_present);

#ifdef __cplusplus
}
#endif

#endif
