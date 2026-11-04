#ifndef LIBRE_DESK_CONFIG_STORE_H
#define LIBRE_DESK_CONFIG_STORE_H

#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum config_store_result {
    CONFIG_STORE_RESULT_OK = 0,
    CONFIG_STORE_RESULT_ABSENT,
    CONFIG_STORE_RESULT_INVALID,
    CONFIG_STORE_RESULT_IO_ERROR,
    CONFIG_STORE_RESULT_BAD_ARGUMENT,
    CONFIG_STORE_RESULT_COUNT
} config_store_result_t;

config_store_result_t config_store_init(void);
config_store_result_t config_store_load(runtime_config_t *config_out);
config_store_result_t config_store_save(const runtime_config_t *config);
config_store_result_t config_store_erase_namespace(void);
config_store_result_t config_store_setup_request_set(void);
config_store_result_t config_store_setup_request_get(bool *requested_out);
config_store_result_t config_store_setup_request_clear(void);
config_store_result_t config_store_factory_reset_request(void);

#ifdef __cplusplus
}
#endif

#endif
