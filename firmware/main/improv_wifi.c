#include "improv_wifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config_store.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv_serial_core.h"
#include "provisioning_session.h"
#include "wifi_manager.h"

#define IMPROV_TASK_STACK_DEPTH 4096U
#define IMPROV_TASK_PRIORITY 4U
#define IMPROV_POLL_MS 20U
#define IMPROV_RESTART_DELAY_MS 300U
#define IMPROV_CANDIDATE_TIMEOUT_MS 25000U

static const char *const IMPROV_FIRMWARE_NAME = "Libre Desk live-data MVP";
static const char *const IMPROV_CHIP_FAMILY = "ESP32-C6";
static const char *const IMPROV_DEVICE_NAME = "Libre Desk";

typedef struct improv_wifi_context {
    runtime_config_t current;
    bool provisioned;
    bool started;
    StaticTask_t task_storage;
    StackType_t task_stack[IMPROV_TASK_STACK_DEPTH];
} improv_wifi_context_t;

typedef struct deferred_restart {
    bool requested;
} deferred_restart_t;

static improv_wifi_context_t s_improv;

static int discard_log_output(const char *format, va_list arguments)
{
    (void)format;
    (void)arguments;
    return 0;
}

static bool send_packet(const uint8_t *packet, size_t length)
{
    size_t written;
    int flush_result;

    if (packet == NULL || length == 0U) {
        return false;
    }
    /* ESP-IDF logging is disabled before this task starts. Keep direct stdio
     * writes atomic as an additional transport boundary. */
    flockfile(stdout);
    written = fwrite(packet, 1U, length, stdout);
    flush_result = fflush(stdout);
    funlockfile(stdout);
    return written == length && flush_result == 0;
}

static bool send_state(improv_serial_state_t state)
{
    uint8_t packet[16];
    size_t length = improv_serial_encode_state(state, packet, sizeof(packet));
    return send_packet(packet, length);
}

static bool send_error(improv_serial_error_t error)
{
    uint8_t packet[16];
    size_t length = improv_serial_encode_error(error, packet, sizeof(packet));
    return send_packet(packet, length);
}

static bool send_empty_result(uint8_t command)
{
    uint8_t packet[16];
    size_t length = improv_serial_encode_rpc_result(
        command, NULL, 0U, packet, sizeof(packet));
    return send_packet(packet, length);
}

static bool send_info(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    const char *values[4];
    uint8_t packet[160];
    size_t length;

    if (description == NULL) {
        return false;
    }
    values[0] = IMPROV_FIRMWARE_NAME;
    values[1] = description->version;
    values[2] = IMPROV_CHIP_FAMILY;
    values[3] = IMPROV_DEVICE_NAME;
    length = improv_serial_encode_rpc_result(
        IMPROV_SERIAL_RPC_REQUEST_INFO, values,
        sizeof(values) / sizeof(values[0]), packet, sizeof(packet));
    return send_packet(packet, length);
}

static improv_serial_state_t current_state(void)
{
    return s_improv.provisioned ? IMPROV_SERIAL_STATE_PROVISIONED
                                : IMPROV_SERIAL_STATE_READY;
}

static bool connect_candidate(const runtime_config_t *candidate, void *context)
{
    (void)context;
    return wifi_manager_try_candidate(candidate, &s_improv.current,
                                      IMPROV_CANDIDATE_TIMEOUT_MS) == ESP_OK;
}

static config_store_result_t save_candidate(const runtime_config_t *candidate,
                                             void *context)
{
    (void)context;
    return config_store_save(candidate);
}

static config_store_result_t reload_candidate(runtime_config_t *candidate_out,
                                               void *context)
{
    (void)context;
    return config_store_load(candidate_out);
}

static config_store_result_t clear_setup_request(void *context)
{
    (void)context;
    return config_store_setup_request_clear();
}

static void defer_restart(void *context)
{
    deferred_restart_t *restart = context;
    restart->requested = true;
}

static void apply_wifi(const wifi_provisioning_submission_t *submission)
{
    deferred_restart_t restart = {0};
    provisioning_session_ops_t operations = {
        .connect_candidate = connect_candidate,
        .save_candidate = save_candidate,
        .reload_active = reload_candidate,
        .clear_setup_request = clear_setup_request,
        .restart = defer_restart,
        .context = &restart,
    };
    provisioning_session_result_t result;

    (void)send_state(IMPROV_SERIAL_STATE_PROVISIONING);
    result = provisioning_session_apply(&s_improv.current, submission, &operations);
    if (result != PROVISIONING_SESSION_RESULT_OK || !restart.requested) {
        (void)send_state(current_state());
        (void)send_error(
            result == PROVISIONING_SESSION_RESULT_INVALID_CANDIDATE
                ? IMPROV_SERIAL_ERROR_INVALID_RPC
                : (result == PROVISIONING_SESSION_RESULT_NO_IP
                       ? IMPROV_SERIAL_ERROR_UNABLE_TO_CONNECT
                       : IMPROV_SERIAL_ERROR_UNKNOWN));
        return;
    }

    s_improv.provisioned = true;
    (void)send_state(IMPROV_SERIAL_STATE_PROVISIONED);
    (void)send_empty_result(IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS);
    vTaskDelay(pdMS_TO_TICKS(IMPROV_RESTART_DELAY_MS));
    esp_restart();
}

static void handle_rpc(const improv_serial_rpc_t *rpc)
{
    if (rpc == NULL) {
        (void)send_error(IMPROV_SERIAL_ERROR_INVALID_RPC);
        return;
    }
    switch (rpc->command) {
        case IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE:
            (void)send_state(current_state());
            /* EWT resolves READY from CURRENT_STATE itself. PROVISIONED also
             * receives an empty RPC result for the optional device URL. */
            if (s_improv.provisioned) {
                (void)send_empty_result(IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE);
            }
            break;
        case IMPROV_SERIAL_RPC_REQUEST_INFO:
            if (!send_info()) {
                (void)send_error(IMPROV_SERIAL_ERROR_UNKNOWN);
            }
            break;
        case IMPROV_SERIAL_RPC_REQUEST_WIFI_NETWORKS:
            /* MVP: rejecting scan makes EWT show manual SSID/password fields. */
            (void)send_error(IMPROV_SERIAL_ERROR_UNKNOWN_RPC);
            break;
        case IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS:
            if (!rpc->has_wifi_submission) {
                (void)send_error(IMPROV_SERIAL_ERROR_INVALID_RPC);
            } else {
                apply_wifi(&rpc->wifi);
            }
            break;
        default:
            (void)send_error(IMPROV_SERIAL_ERROR_UNKNOWN_RPC);
            break;
    }
}

static void improv_task(void *argument)
{
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    uint8_t input[64];

    (void)argument;
    improv_serial_parser_init(&parser);
    memset(&rpc, 0, sizeof(rpc));
    for (;;) {
        ssize_t received = read(STDIN_FILENO, input, sizeof(input));
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(IMPROV_POLL_MS));
            continue;
        }
        for (ssize_t index = 0; index < received; index++) {
            improv_serial_parse_result_t result =
                improv_serial_parser_push(&parser, input[index], &rpc);
            if (result == IMPROV_SERIAL_PARSE_RPC) {
                handle_rpc(&rpc);
                memset(&rpc, 0, sizeof(rpc));
            } else if (result == IMPROV_SERIAL_PARSE_INVALID) {
                (void)send_error(IMPROV_SERIAL_ERROR_INVALID_RPC);
                memset(&rpc, 0, sizeof(rpc));
            }
        }
    }
}

esp_err_t improv_wifi_start(const runtime_config_t *current,
                            bool persisted_config_present)
{
    TaskHandle_t task;

    if (current == NULL || !runtime_config_validate(current, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_improv.started) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_improv, 0, sizeof(s_improv));
    s_improv.current = *current;
    s_improv.provisioned = persisted_config_present;
    (void)esp_log_set_vprintf(discard_log_output);
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    task = xTaskCreateStatic(improv_task, "improv_wifi",
                             IMPROV_TASK_STACK_DEPTH, NULL,
                             IMPROV_TASK_PRIORITY, s_improv.task_stack,
                             &s_improv.task_storage);
    if (task == NULL) {
        memset(&s_improv, 0, sizeof(s_improv));
        return ESP_ERR_NO_MEM;
    }
    s_improv.started = true;
    return ESP_OK;
}
