#include "app_boot_flow.h"

#include <string.h>

static app_boot_result_t map_store_failure(config_store_result_t result)
{
    return result == CONFIG_STORE_RESULT_INVALID
               ? APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID
               : APP_BOOT_RESULT_IO_ERROR;
}

app_boot_result_t app_boot_flow_select(const app_boot_flow_ops_t *ops,
                                       runtime_config_t *config_out,
                                       app_boot_mode_t *mode_out)
{
    runtime_config_t selected;
    runtime_config_t persisted;
    config_store_result_t store_result;
    bool setup_requested = false;

    if (config_out != NULL) {
        memset(config_out, 0, sizeof(*config_out));
    }
    if (mode_out != NULL) {
        *mode_out = APP_BOOT_MODE_NORMAL;
    }
    if (ops == NULL || config_out == NULL || mode_out == NULL ||
        ops->load_embedded == NULL || ops->load_persisted == NULL ||
        ops->get_setup_request == NULL) {
        return APP_BOOT_RESULT_BAD_ARGUMENT;
    }

    memset(&selected, 0, sizeof(selected));
    if (!ops->load_embedded(&selected, ops->context) ||
        !runtime_config_validate(&selected, NULL)) {
        memset(&selected, 0, sizeof(selected));
        return APP_BOOT_RESULT_EMBEDDED_CONFIG_INVALID;
    }

    memset(&persisted, 0, sizeof(persisted));
    store_result = ops->load_persisted(&persisted, ops->context);
    if (store_result == CONFIG_STORE_RESULT_OK) {
        if (!runtime_config_validate(&persisted, NULL)) {
            memset(&selected, 0, sizeof(selected));
            memset(&persisted, 0, sizeof(persisted));
            return APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID;
        }
        /* Wi-Fi and display preferences are user-owned, but the bridge endpoint
         * and revocable device credential belong to the installed firmware.
         * Refresh them on every boot so normal updates cannot retain a retired
         * endpoint or superseded token from NVS. */
        memcpy(persisted.bridge_origin, selected.bridge_origin,
               sizeof(persisted.bridge_origin));
        memcpy(persisted.device_token, selected.device_token,
               sizeof(persisted.device_token));
        if (!runtime_config_validate(&persisted, NULL)) {
            memset(&selected, 0, sizeof(selected));
            memset(&persisted, 0, sizeof(persisted));
            return APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID;
        }
        selected = persisted;
    } else if (store_result != CONFIG_STORE_RESULT_ABSENT) {
        app_boot_result_t result = map_store_failure(store_result);
        memset(&selected, 0, sizeof(selected));
        memset(&persisted, 0, sizeof(persisted));
        return result;
    }
    memset(&persisted, 0, sizeof(persisted));

    store_result = ops->get_setup_request(&setup_requested, ops->context);
    if (store_result != CONFIG_STORE_RESULT_OK) {
        app_boot_result_t result = map_store_failure(store_result);
        memset(&selected, 0, sizeof(selected));
        return result;
    }

    *config_out = selected;
    *mode_out = setup_requested ? APP_BOOT_MODE_SETUP : APP_BOOT_MODE_NORMAL;
    memset(&selected, 0, sizeof(selected));
    return APP_BOOT_RESULT_OK;
}
