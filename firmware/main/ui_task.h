#ifndef UI_TASK_H
#define UI_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"
#include "runtime_core.h"

typedef struct ui_task ui_task_t;

esp_err_t ui_task_start(ui_task_t **task_out, lv_display_t *display);
bool ui_task_publish(ui_task_t *task, const runtime_ui_snapshot_t *snapshot);
bool ui_task_publish_provisioning(ui_task_t *task,
                                  const char *ssid, size_t ssid_capacity,
                                  const char *password, size_t password_capacity);

#endif
