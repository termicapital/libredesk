#include "wifi_setup_sequence.h"

#include <stddef.h>

esp_err_t wifi_setup_sequence_run(wifi_setup_call_t call,
                                  void *context,
                                  esp_err_t tolerated_start_error,
                                  wifi_setup_failure_t on_failure,
                                  void *failure_context)
{
    wifi_setup_operation_t operation;

    if (call == NULL) {
        if (on_failure != NULL) {
            on_failure(failure_context);
        }
        return ESP_ERR_INVALID_ARG;
    }
    for (operation = WIFI_SETUP_OPERATION_INITIALIZE;
         operation < WIFI_SETUP_OPERATION_COUNT;
         operation = (wifi_setup_operation_t)(operation + 1)) {
        esp_err_t error = call(operation, context);
        if (operation == WIFI_SETUP_OPERATION_START &&
            error == tolerated_start_error) {
            error = ESP_OK;
        }
        if (error != ESP_OK) {
            if (on_failure != NULL) {
                on_failure(failure_context);
            }
            return error;
        }
    }
    return ESP_OK;
}
