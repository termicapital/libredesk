#include "improv_serial_core.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static bool contains_bytes(const uint8_t *haystack, size_t haystack_length,
                           const char *needle)
{
    size_t needle_length = strlen(needle);

    if (needle_length > haystack_length) {
        return false;
    }
    for (size_t index = 0U; index <= haystack_length - needle_length; index++) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static size_t request_packet(uint8_t command, const uint8_t *data, size_t data_length,
                             uint8_t *packet, size_t capacity)
{
    static const uint8_t header[] = {'I', 'M', 'P', 'R', 'O', 'V'};
    size_t index = 0U;
    size_t packet_length = 6U + 3U + 2U + data_length + 1U;
    unsigned checksum = 0U;

    if (packet == NULL || capacity < packet_length + 1U || data_length > 253U) {
        return 0U;
    }
    memcpy(packet + index, header, sizeof(header));
    index += sizeof(header);
    packet[index++] = 1U;
    packet[index++] = IMPROV_SERIAL_MESSAGE_RPC;
    packet[index++] = (uint8_t)(data_length + 2U);
    packet[index++] = command;
    packet[index++] = (uint8_t)data_length;
    if (data_length > 0U) {
        memcpy(packet + index, data, data_length);
        index += data_length;
    }
    for (size_t cursor = 0U; cursor < index; cursor++) {
        checksum += packet[cursor];
    }
    packet[index++] = (uint8_t)checksum;
    packet[index++] = '\n';
    return index;
}

static int test_current_state_request(void)
{
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    uint8_t packet[32];
    size_t length = request_packet(IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE,
                                   NULL, 0U, packet, sizeof(packet));

    improv_serial_parser_init(&parser);
    memset(&rpc, 0xa5, sizeof(rpc));
    for (size_t index = 0U; index < length - 1U; index++) {
        CHECK(improv_serial_parser_push(&parser, packet[index], &rpc) ==
              IMPROV_SERIAL_PARSE_NONE);
    }
    CHECK(improv_serial_parser_push(&parser, packet[length - 1U], &rpc) ==
          IMPROV_SERIAL_PARSE_RPC);
    CHECK(rpc.command == IMPROV_SERIAL_RPC_REQUEST_CURRENT_STATE);
    CHECK(!rpc.has_wifi_submission);
    return 0;
}

static int test_wifi_settings_request(void)
{
    static const uint8_t data[] = {
        7U, 'H', 'o', 'm', 'e', ' ', '2', 'G',
        9U, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', '1'
    };
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    uint8_t packet[64];
    size_t length = request_packet(IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS,
                                   data, sizeof(data), packet, sizeof(packet));
    improv_serial_parse_result_t result = IMPROV_SERIAL_PARSE_NONE;

    improv_serial_parser_init(&parser);
    memset(&rpc, 0, sizeof(rpc));
    for (size_t index = 0U; index < length; index++) {
        improv_serial_parse_result_t next =
            improv_serial_parser_push(&parser, packet[index], &rpc);
        if (next != IMPROV_SERIAL_PARSE_NONE) {
            result = next;
        }
    }
    CHECK(result == IMPROV_SERIAL_PARSE_RPC);
    CHECK(rpc.command == IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS);
    CHECK(rpc.has_wifi_submission);
    CHECK(strcmp(rpc.wifi.ssid, "Home 2G") == 0);
    CHECK(strcmp(rpc.wifi.password, "password1") == 0);
    return 0;
}

static int test_bad_checksum_is_rejected_and_parser_recovers(void)
{
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    uint8_t packet[32];
    size_t length = request_packet(IMPROV_SERIAL_RPC_REQUEST_INFO,
                                   NULL, 0U, packet, sizeof(packet));
    improv_serial_parse_result_t result = IMPROV_SERIAL_PARSE_NONE;

    packet[length - 2U] ^= 0x01U;
    improv_serial_parser_init(&parser);
    for (size_t index = 0U; index < length; index++) {
        improv_serial_parse_result_t next =
            improv_serial_parser_push(&parser, packet[index], &rpc);
        if (next != IMPROV_SERIAL_PARSE_NONE) {
            result = next;
        }
    }
    CHECK(result == IMPROV_SERIAL_PARSE_INVALID);

    length = request_packet(IMPROV_SERIAL_RPC_REQUEST_INFO,
                            NULL, 0U, packet, sizeof(packet));
    result = IMPROV_SERIAL_PARSE_NONE;
    for (size_t index = 0U; index < length; index++) {
        improv_serial_parse_result_t next =
            improv_serial_parser_push(&parser, packet[index], &rpc);
        if (next != IMPROV_SERIAL_PARSE_NONE) {
            result = next;
        }
    }
    CHECK(result == IMPROV_SERIAL_PARSE_RPC);
    CHECK(rpc.command == IMPROV_SERIAL_RPC_REQUEST_INFO);
    return 0;
}

static int test_maximum_wifi_fields_are_bounded_and_preserved(void)
{
    uint8_t data[98];
    uint8_t packet[128];
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    improv_serial_parse_result_t result = IMPROV_SERIAL_PARSE_NONE;
    size_t length;

    memset(data, 0, sizeof(data));
    data[0] = 32U;
    memset(data + 1U, 'S', 32U);
    data[33] = 64U;
    memset(data + 34U, 'A', 64U);
    length = request_packet(IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS,
                            data, sizeof(data), packet, sizeof(packet));
    improv_serial_parser_init(&parser);
    memset(&rpc, 0, sizeof(rpc));
    for (size_t index = 0U; index < length; index++) {
        improv_serial_parse_result_t next =
            improv_serial_parser_push(&parser, packet[index], &rpc);
        if (next != IMPROV_SERIAL_PARSE_NONE) result = next;
    }
    CHECK(result == IMPROV_SERIAL_PARSE_RPC);
    CHECK(strlen(rpc.wifi.ssid) == 32U);
    CHECK(strlen(rpc.wifi.password) == 64U);
    return 0;
}

static int test_malformed_wifi_lengths_are_rejected(void)
{
    static const uint8_t data[] = {5U, 'a', 'b', 1U, 'c'};
    uint8_t packet[32];
    improv_serial_parser_t parser;
    improv_serial_rpc_t rpc;
    improv_serial_parse_result_t result = IMPROV_SERIAL_PARSE_NONE;
    size_t length = request_packet(IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS,
                                   data, sizeof(data), packet, sizeof(packet));

    improv_serial_parser_init(&parser);
    for (size_t index = 0U; index < length; index++) {
        improv_serial_parse_result_t next =
            improv_serial_parser_push(&parser, packet[index], &rpc);
        if (next != IMPROV_SERIAL_PARSE_NONE) result = next;
    }
    CHECK(result == IMPROV_SERIAL_PARSE_INVALID);
    return 0;
}

static int test_state_and_rpc_result_encoding(void)
{
    uint8_t packet[128];
    size_t length = improv_serial_encode_state(IMPROV_SERIAL_STATE_READY,
                                               packet, sizeof(packet));
    unsigned checksum = 0U;

    CHECK(length == 12U);
    CHECK(memcmp(packet, "IMPROV", 6U) == 0);
    CHECK(packet[6] == 1U);
    CHECK(packet[7] == IMPROV_SERIAL_MESSAGE_CURRENT_STATE);
    CHECK(packet[8] == 1U);
    CHECK(packet[9] == IMPROV_SERIAL_STATE_READY);
    for (size_t index = 0U; index < length - 2U; index++) {
        checksum += packet[index];
    }
    CHECK(packet[length - 2U] == (uint8_t)checksum);
    CHECK(packet[length - 1U] == '\n');

    length = improv_serial_encode_rpc_result(
        IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS, NULL, 0U,
        packet, sizeof(packet));
    CHECK(length == 13U);
    CHECK(packet[7] == IMPROV_SERIAL_MESSAGE_RPC_RESULT);
    CHECK(packet[8] == 2U);
    CHECK(packet[9] == IMPROV_SERIAL_RPC_SEND_WIFI_SETTINGS);
    CHECK(packet[10] == 0U);
    return 0;
}

static int test_info_result_contains_exact_identity(void)
{
    static const char *const values[] = {
        "Libre Desk live-data MVP", "0.5.0-webserial-wifi", "ESP32-C6", "Libre Desk"
    };
    uint8_t packet[160];
    size_t length = improv_serial_encode_rpc_result(
        IMPROV_SERIAL_RPC_REQUEST_INFO, values,
        sizeof(values) / sizeof(values[0]), packet, sizeof(packet));

    CHECK(length > 13U);
    CHECK(packet[7] == IMPROV_SERIAL_MESSAGE_RPC_RESULT);
    CHECK(packet[9] == IMPROV_SERIAL_RPC_REQUEST_INFO);
    CHECK(packet[10] > 0U);
    CHECK(contains_bytes(packet, length, values[0]));
    CHECK(contains_bytes(packet, length, values[1]));
    CHECK(contains_bytes(packet, length, values[2]));
    CHECK(contains_bytes(packet, length, values[3]));
    return 0;
}

int main(void)
{
    CHECK(test_current_state_request() == 0);
    CHECK(test_wifi_settings_request() == 0);
    CHECK(test_bad_checksum_is_rejected_and_parser_recovers() == 0);
    CHECK(test_maximum_wifi_fields_are_bounded_and_preserved() == 0);
    CHECK(test_malformed_wifi_lengths_are_rejected() == 0);
    CHECK(test_state_and_rpc_result_encoding() == 0);
    CHECK(test_info_result_contains_exact_identity() == 0);
    puts("improv serial core tests passed");
    return 0;
}
