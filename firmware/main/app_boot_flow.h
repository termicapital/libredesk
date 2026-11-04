#ifndef LIBRE_DESK_APP_BOOT_FLOW_H
#define LIBRE_DESK_APP_BOOT_FLOW_H

#include <stdbool.h>

#include "config_store.h"
#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum app_boot_mode {
    APP_BOOT_MODE_NORMAL = 0,
    APP_BOOT_MODE_SETUP,
    APP_BOOT_MODE_COUNT
} app_boot_mode_t;

typedef enum app_boot_result {
    APP_BOOT_RESULT_OK = 0,
    APP_BOOT_RESULT_BAD_ARGUMENT,
    APP_BOOT_RESULT_EMBEDDED_CONFIG_INVALID,
    APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID,
    APP_BOOT_RESULT_IO_ERROR,
    APP_BOOT_RESULT_COUNT
} app_boot_result_t;

typedef struct app_boot_flow_ops {
    bool (*load_embedded)(runtime_config_t *config_out, void *context);
    config_store_result_t (*load_persisted)(runtime_config_t *config_out,
                                             void *context);
    config_store_result_t (*get_setup_request)(bool *requested_out,
                                                void *context);
    void *context;
} app_boot_flow_ops_t;

app_boot_result_t app_boot_flow_select(const app_boot_flow_ops_t *ops,
                                       runtime_config_t *config_out,
                                       app_boot_mode_t *mode_out);

#ifdef __cplusplus
}
#endif

#endif
