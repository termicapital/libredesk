#ifndef WIFI_SETUP_SEQUENCE_H
#define WIFI_SETUP_SEQUENCE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wifi_setup_operation {
    WIFI_SETUP_OPERATION_INITIALIZE = 0,
    WIFI_SETUP_OPERATION_SET_MODE,
    WIFI_SETUP_OPERATION_SET_CONFIG,
    WIFI_SETUP_OPERATION_START,
    WIFI_SETUP_OPERATION_COUNT
} wifi_setup_operation_t;

typedef esp_err_t (*wifi_setup_call_t)(wifi_setup_operation_t operation,
                                       void *context);
typedef void (*wifi_setup_failure_t)(void *context);

esp_err_t wifi_setup_sequence_run(wifi_setup_call_t call,
                                  void *context,
                                  esp_err_t tolerated_start_error,
                                  wifi_setup_failure_t on_failure,
                                  void *failure_context);

#ifdef __cplusplus
}
#endif

#endif
