#ifndef NET_TASK_H
#define NET_TASK_H

#include "esp_err.h"
#include "glucose_core.h"
#include "runtime_core.h"
#include "ui_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the process-lifetime HTTPS poller. After successful start, only the
 * net task mutates core; config, core, and ui must remain alive forever.
 */
esp_err_t net_task_start(const runtime_config_t *config,
                         glucose_core_t *core,
                         ui_task_t *ui);

#ifdef __cplusplus
}
#endif

#endif
