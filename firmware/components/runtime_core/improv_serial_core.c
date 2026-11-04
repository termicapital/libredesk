#include "improv_serial_core.h"

#include <string.h>

#define IMPROV_HEADER_BYTES 6U
#define IMPROV_FIXED_PACKET_BYTES 10U

static const uint8_t IMPROV_HEADER[IMPROV_HEADER_BYTES] = {
    'I', 'M', 'P', 'R', 'O', 'V'
};

static void parser_reset(improv_serial_parser_t *parser)
{
    memset(parser->bytes, 0, sizeof(parser->bytes));
    parser->length = 0U;
    parser->expected_length = 0U;
}

void improv_serial_parser_init(improv_serial_parser_t *parser)
{
    if (parser != NULL) {
        parser_reset(parser);
    }
}

static bool checksum_is_valid(const uint8_t *packet, size_t length)
{
    unsigned checksum = 0U;

    if (packet == NULL || length < IMPROV_FIXED_PACKET_BYTES) {
        return false;
    }
    for (size_t index = 0U; index < length - 1U; index++) {
        checksum += packet[index];
    }
    return (uint8_t)checksum == packet[length - 1U];
}

static bool copy_field(char *destination, size_t capacity,
                       const uint8_t *source, size_t length)
{
    if (destination == NULL || source == NULL || length == 0U ||
        length >= capacity || memchr(source, '\0', length) != NULL) {
        return false;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

static bool decode_wifi(const uint8_t *data, size_t length,
                        wifi_provisioning_submission_t *submission)
{
    size_t ssid_length;
    size_t password_offset;
    size_t password_length;

    if (data == NULL || submission == NULL || length < 2U) {
        return false;
    }
    memset(submission, 0, sizeof(*submission));
    ssid_length = data[0];
    password_offset = 1U + ssid_length;
    if (ssid_length == 0U || password_offset >= length) {
        return false;
    }
    password_length = data[password_offset];
    if (password_offset + 1U + password_length != length ||
        password_length >= sizeof(submission->password) ||
        memchr(data + password_offset + 1U, '\0', password_length) != NULL ||
        !copy_field(submission->ssid, sizeof(submission->ssid),
                    data + 1U, ssid_length)) {
        memset(submission, 0, sizeof(*submission));
        return false;
    }
    if (password_length > 0U) {
        memcpy(submission->password, data + password_offset + 1U,
               password_length);
    }
    submission->password[password_length] = '\0';
    return true;
}

static improv_serial_parse_result_t decode_rpc(
    const uint8_t *packet, size_t length, improv_serial_rpc_t *rpc_out)
{
    size_t outer_length;
    uint8_t command;
    size_t inner_length;

    if (packet == NULL || rpc_out == NULL ||
        length < IMPROV_FIXED_PACKET_BYTES || packet[6] != 1U ||
        packet[7] != IMPROV_SERIAL_MESSAGE_RPC || !checksum_is_valid(packet, length)) {
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    outer_length = packet[8];
    if (length != IMPROV_FIXED_PACKET_BYTES + outer_length || outer_length < 2U) {
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    command = packet[9];
    inner_length = packet[10];
    if (inner_length + 2U != outer_length) {
        return IMPROV_SERIAL_PARSE_INVALID;
    }

    memset(rpc_out, 0, sizeof(*rpc_out));
    rpc_out->command = command;
    if (command == IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS) {
        if (!decode_wifi(packet + 11U, inner_length, &rpc_out->wifi)) {
            memset(rpc_out, 0, sizeof(*rpc_out));
            return IMPROV_SERIAL_PARSE_INVALID;
        }
        rpc_out->has_wifi_submission = true;
    } else if ((command == IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE ||
                command == IMPROV_SERIAL_RPC_REQUEST_INFO ||
                command == IMPROV_SERIAL_RPC_REQUEST_WIFI_NETWORKS) &&
               inner_length != 0U) {
        memset(rpc_out, 0, sizeof(*rpc_out));
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    return IMPROV_SERIAL_PARSE_RPC;
}

improv_serial_parse_result_t improv_serial_parser_push(
    improv_serial_parser_t *parser, uint8_t byte, improv_serial_rpc_t *rpc_out)
{
    improv_serial_parse_result_t result;

    if (parser == NULL || rpc_out == NULL) {
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    if (parser->length < IMPROV_HEADER_BYTES) {
        if (byte == IMPROV_HEADER[parser->length]) {
            parser->bytes[parser->length++] = byte;
        } else if (byte == IMPROV_HEADER[0]) {
            parser_reset(parser);
            parser->bytes[0] = byte;
            parser->length = 1U;
        } else {
            parser_reset(parser);
        }
        return IMPROV_SERIAL_PARSE_NONE;
    }
    if (parser->length >= sizeof(parser->bytes)) {
        parser_reset(parser);
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    parser->bytes[parser->length++] = byte;
    if (parser->length == 9U) {
        parser->expected_length =
            IMPROV_FIXED_PACKET_BYTES + parser->bytes[8] + 1U;
        if (parser->expected_length > sizeof(parser->bytes)) {
            parser_reset(parser);
            return IMPROV_SERIAL_PARSE_INVALID;
        }
    }
    if (parser->expected_length == 0U || parser->length < parser->expected_length) {
        return IMPROV_SERIAL_PARSE_NONE;
    }
    if (parser->bytes[parser->length - 1U] != '\n') {
        bool starts_next_header = byte == IMPROV_HEADER[0];
        parser_reset(parser);
        if (starts_next_header) {
            parser->bytes[0] = byte;
            parser->length = 1U;
        }
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    result = decode_rpc(parser->bytes, parser->length - 1U, rpc_out);
    parser_reset(parser);
    return result;
}

static size_t encode_message(uint8_t type, const uint8_t *data, size_t data_length,
                             uint8_t *packet_out, size_t capacity)
{
    size_t packet_length = IMPROV_FIXED_PACKET_BYTES + data_length + 1U;
    size_t index = 0U;
    unsigned checksum = 0U;

    if (packet_out == NULL || data_length > UINT8_MAX ||
        capacity < packet_length || (data_length > 0U && data == NULL)) {
        return 0U;
    }
    memcpy(packet_out + index, IMPROV_HEADER, sizeof(IMPROV_HEADER));
    index += sizeof(IMPROV_HEADER);
    packet_out[index++] = 1U;
    packet_out[index++] = type;
    packet_out[index++] = (uint8_t)data_length;
    if (data_length > 0U) {
        memcpy(packet_out + index, data, data_length);
        index += data_length;
    }
    for (size_t cursor = 0U; cursor < index; cursor++) {
        checksum += packet_out[cursor];
    }
    packet_out[index++] = (uint8_t)checksum;
    packet_out[index++] = '\n';
    return index;
}

size_t improv_serial_encode_state(improv_serial_state_t state,
                                  uint8_t *packet_out, size_t capacity)
{
    uint8_t data = (uint8_t)state;
    return encode_message(IMPROV_SERIAL_MESSAGE_CURRENT_STATE, &data, 1U,
                          packet_out, capacity);
}

size_t improv_serial_encode_error(improv_serial_error_t error,
                                  uint8_t *packet_out, size_t capacity)
{
    uint8_t data = (uint8_t)error;
    return encode_message(IMPROV_SERIAL_MESSAGE_ERROR_STATE, &data, 1U,
                          packet_out, capacity);
}

size_t improv_serial_encode_rpc_result(uint8_t command,
                                       const char *const *values,
                                       size_t value_count,
                                       uint8_t *packet_out, size_t capacity)
{
    uint8_t data[UINT8_MAX];
    size_t index = 2U;
    size_t total_length = 0U;

    if ((value_count > 0U && values == NULL) || value_count > UINT8_MAX) {
        return 0U;
    }
    memset(data, 0, sizeof(data));
    data[0] = command;
    for (size_t value_index = 0U; value_index < value_count; value_index++) {
        size_t value_length;

        if (values[value_index] == NULL) {
            return 0U;
        }
        value_length = strlen(values[value_index]);
        if (value_length > UINT8_MAX ||
            index + 1U + value_length > sizeof(data)) {
            return 0U;
        }
        data[index++] = (uint8_t)value_length;
        memcpy(data + index, values[value_index], value_length);
        index += value_length;
        total_length += 1U + value_length;
    }
    if (total_length > UINT8_MAX || index > UINT8_MAX) {
        return 0U;
    }
    data[1] = (uint8_t)total_length;
    return encode_message(IMPROV_SERIAL_MESSAGE_RPC_RESULT, data, index,
                          packet_out, capacity);
}
