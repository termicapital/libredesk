#ifndef LIBRE_DESK_PROVISIONING_SESSION_H
#define LIBRE_DESK_PROVISIONING_SESSION_H

#include "config_store.h"
#include "runtime_core.h"
#include "wifi_provisioning_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum provisioning_session_result {
    PROVISIONING_SESSION_RESULT_OK = 0,
    PROVISIONING_SESSION_RESULT_BAD_ARGUMENT,
    PROVISIONING_SESSION_RESULT_INVALID_CANDIDATE,
    PROVISIONING_SESSION_RESULT_NO_IP,
    PROVISIONING_SESSION_RESULT_SAVE_FAILED,
    PROVISIONING_SESSION_RESULT_SAVE_UNCERTAIN,
    PROVISIONING_SESSION_RESULT_RELOAD_FAILED,
    PROVISIONING_SESSION_RESULT_VERIFY_FAILED,
    PROVISIONING_SESSION_RESULT_MARKER_CLEAR_FAILED,
    PROVISIONING_SESSION_RESULT_COUNT
} provisioning_session_result_t;

typedef struct provisioning_session_ops {
    bool (*connect_candidate)(const runtime_config_t *candidate, void *context);
    config_store_result_t (*save_candidate)(const runtime_config_t *candidate,
                                             void *context);
    config_store_result_t (*reload_active)(runtime_config_t *active_out,
                                           void *context);
    config_store_result_t (*clear_setup_request)(void *context);
    void (*restart)(void *context);
    void *context;
} provisioning_session_ops_t;

bool provisioning_runtime_config_equal(const runtime_config_t *left,
                                       const runtime_config_t *right);
provisioning_session_result_t provisioning_session_apply(
    const runtime_config_t *current,
    const wifi_provisioning_submission_t *submission,
    const provisioning_session_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif
