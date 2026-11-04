#ifndef LIBRE_DESK_PROVISIONING_PORTAL_H
#define LIBRE_DESK_PROVISIONING_PORTAL_H

#include <stdint.h>

#include "esp_err.h"
#include "runtime_core.h"
#include "ui_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROVISIONING_PORTAL_DEFAULT_SESSION_TIMEOUT_MS UINT32_C(600000)
#define PROVISIONING_PORTAL_CANDIDATE_TIMEOUT_MS UINT32_C(30000)

esp_err_t provisioning_portal_start(const runtime_config_t *current,
                                    ui_task_t *ui,
                                    uint32_t session_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
