#ifndef LIBRE_DESK_IMPROV_SERIAL_CORE_H
#define LIBRE_DESK_IMPROV_SERIAL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_provisioning_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMPROV_SERIAL_MAX_PACKET_BYTES 266U

typedef enum improv_serial_message_type {
    IMPROV_SERIAL_MESSAGE_CURRENT_STATE = 0x01,
    IMPROV_SERIAL_MESSAGE_ERROR_STATE = 0x02,
    IMPROV_SERIAL_MESSAGE_RPC = 0x03,
    IMPROV_SERIAL_MESSAGE_RPC_RESULT = 0x04
} improv_serial_message_type_t;

typedef enum improv_serial_state {
    IMPROV_SERIAL_STATE_STOPPED = 0x00,
    IMPROV_SERIAL_STATE_READY = 0x02,
    IMPROV_SERIAL_STATE_PROVISIONING = 0x03,
    IMPROV_SERIAL_STATE_PROVISIONED = 0x04
} improv_serial_state_t;

typedef enum improv_serial_error {
    IMPROV_SERIAL_ERROR_NONE = 0x00,
    IMPROV_SERIAL_ERROR_INVALID_RPC = 0x01,
    IMPROV_SERIAL_ERROR_UNKNOWN_RPC = 0x02,
    IMPROV_SERIAL_ERROR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_SERIAL_ERROR_UNKNOWN = 0xff
} improv_serial_error_t;

typedef enum improv_serial_rpc_command {
    IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS = 0x01,
    IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE = 0x02,
    IMPROV_SERIAL_RPC_REQUEST_INFO = 0x03,
    IMPROV_SERIAL_RPC_REQUEST_WIFI_NETWORKS = 0x04
} improv_serial_rpc_command_t;

typedef enum improv_serial_parse_result {
    IMPROV_SERIAL_PARSE_NONE = 0,
    IMPROV_SERIAL_PARSE_RPC,
    IMPROV_SERIAL_PARSE_INVALID
} improv_serial_parse_result_t;

typedef struct improv_serial_rpc {
    uint8_t command;
    bool has_wifi_submission;
    wifi_provisioning_submission_t wifi;
} improv_serial_rpc_t;

typedef struct improv_serial_parser {
    uint8_t bytes[IMPROV_SERIAL_MAX_PACKET_BYTES];
    size_t length;
    size_t expected_length;
} improv_serial_parser_t;

void improv_serial_parser_init(improv_serial_parser_t *parser);
improv_serial_parse_result_t improv_serial_parser_push(
    improv_serial_parser_t *parser, uint8_t byte, improv_serial_rpc_t *rpc_out);

size_t improv_serial_encode_state(improv_serial_state_t state,
                                  uint8_t *packet_out, size_t capacity);
size_t improv_serial_encode_error(improv_serial_error_t error,
                                  uint8_t *packet_out, size_t capacity);
size_t improv_serial_encode_rpc_result(uint8_t command,
                                       const char *const *values,
                                       size_t value_count,
                                       uint8_t *packet_out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
