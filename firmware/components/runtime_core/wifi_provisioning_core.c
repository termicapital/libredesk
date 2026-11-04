#include "wifi_provisioning_core.h"

#include <string.h>

static const char PASSWORD_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const char HEX_UPPER[] = "0123456789ABCDEF";

static void secure_zero(void *address, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    while (size > 0U) {
        *bytes = 0U;
        bytes++;
        size--;
    }
}

static void clear_text(char *text, size_t capacity)
{
    if (text != NULL && capacity > 0U) {
        text[0] = '\0';
    }
}

bool wifi_provisioning_ap_credentials(
    const uint8_t *mac, size_t mac_capacity,
    const uint8_t *random, size_t random_capacity,
    char *ssid_out, size_t ssid_capacity,
    char *password_out, size_t password_capacity)
{
    uint64_t sample = UINT64_C(0);
    size_t index;

    clear_text(ssid_out, ssid_capacity);
    clear_text(password_out, password_capacity);
    if (mac == NULL || random == NULL || ssid_out == NULL || password_out == NULL ||
        mac_capacity < 6U || random_capacity < 8U ||
        ssid_capacity < WIFI_PROVISIONING_AP_SSID_CAPACITY ||
        password_capacity < WIFI_PROVISIONING_AP_PASSWORD_CAPACITY) {
        return false;
    }

    (void)memcpy(ssid_out, "LibreDesk-", 10U);
    for (index = 0U; index < 3U; index++) {
        uint8_t byte = mac[index + 3U];
        ssid_out[10U + index * 2U] = HEX_UPPER[byte >> 4U];
        ssid_out[11U + index * 2U] = HEX_UPPER[byte & UINT8_C(0x0f)];
    }
    ssid_out[16] = '\0';

    for (index = 0U; index < 8U; index++) {
        sample = (sample << 8U) | random[index];
    }
    sample >>= 4U;
    for (index = 0U; index < 12U; index++) {
        unsigned shift = (unsigned)(55U - index * 5U);
        password_out[index] = PASSWORD_ALPHABET[(sample >> shift) & UINT64_C(0x1f)];
    }
    password_out[12] = '\0';
    sample = UINT64_C(0);
    return true;
}

static int hex_value(uint8_t character)
{
    if (character >= (uint8_t)'0' && character <= (uint8_t)'9') {
        return (int)(character - (uint8_t)'0');
    }
    if (character >= (uint8_t)'a' && character <= (uint8_t)'f') {
        return (int)(character - (uint8_t)'a') + 10;
    }
    if (character >= (uint8_t)'A' && character <= (uint8_t)'F') {
        return (int)(character - (uint8_t)'A') + 10;
    }
    return -1;
}

static bool decode_form_value(const uint8_t *source, size_t length,
                              char *destination, size_t capacity)
{
    size_t source_index = 0U;
    size_t destination_index = 0U;

    if (source == NULL || destination == NULL || capacity == 0U) {
        return false;
    }
    while (source_index < length) {
        uint8_t value = source[source_index++];
        if (value == (uint8_t)'+') {
            value = (uint8_t)' ';
        } else if (value == (uint8_t)'%') {
            int high;
            int low;
            if (source_index + 2U > length) {
                return false;
            }
            high = hex_value(source[source_index]);
            low = hex_value(source[source_index + 1U]);
            if (high < 0 || low < 0) {
                return false;
            }
            source_index += 2U;
            value = (uint8_t)(((unsigned)high << 4U) | (unsigned)low);
        }
        if (value == 0U || destination_index + 1U >= capacity) {
            return false;
        }
        destination[destination_index++] = (char)value;
    }
    destination[destination_index] = '\0';
    return true;
}

wifi_provisioning_result_t wifi_provisioning_parse_form(
    const uint8_t *body, size_t length,
    wifi_provisioning_submission_t *submission_out)
{
    wifi_provisioning_submission_t candidate;
    bool have_ssid = false;
    bool have_password = false;
    size_t cursor = 0U;

    if (submission_out != NULL) {
        secure_zero(submission_out, sizeof(*submission_out));
    }
    secure_zero(&candidate, sizeof(candidate));
    if (body == NULL || submission_out == NULL) {
        return WIFI_PROVISIONING_NULL_ARGUMENT;
    }
    if (length > WIFI_PROVISIONING_FORM_MAX_BYTES) {
        return WIFI_PROVISIONING_BODY_TOO_LARGE;
    }
    if (length == 0U || memchr(body, '\0', length) != NULL) {
        return WIFI_PROVISIONING_MALFORMED;
    }

    while (cursor < length) {
        size_t segment_start = cursor;
        size_t segment_end;
        size_t equals;
        char *destination;
        size_t destination_capacity;

        while (cursor < length && body[cursor] != (uint8_t)'&') {
            cursor++;
        }
        segment_end = cursor;
        if (segment_end == segment_start ||
            (cursor < length && cursor + 1U == length)) {
            secure_zero(&candidate, sizeof(candidate));
            return WIFI_PROVISIONING_MALFORMED;
        }
        equals = segment_start;
        while (equals < segment_end && body[equals] != (uint8_t)'=') {
            equals++;
        }
        if (equals == segment_start || equals == segment_end) {
            secure_zero(&candidate, sizeof(candidate));
            return WIFI_PROVISIONING_MALFORMED;
        }
        if (equals - segment_start == 4U &&
            memcmp(body + segment_start, "ssid", 4U) == 0) {
            if (have_ssid) {
                secure_zero(&candidate, sizeof(candidate));
                return WIFI_PROVISIONING_MALFORMED;
            }
            have_ssid = true;
            destination = candidate.ssid;
            destination_capacity = sizeof(candidate.ssid);
        } else if (equals - segment_start == 8U &&
                   memcmp(body + segment_start, "password", 8U) == 0) {
            if (have_password) {
                secure_zero(&candidate, sizeof(candidate));
                return WIFI_PROVISIONING_MALFORMED;
            }
            have_password = true;
            destination = candidate.password;
            destination_capacity = sizeof(candidate.password);
        } else {
            secure_zero(&candidate, sizeof(candidate));
            return WIFI_PROVISIONING_MALFORMED;
        }
        if (!decode_form_value(body + equals + 1U,
                               segment_end - equals - 1U,
                               destination, destination_capacity)) {
            secure_zero(&candidate, sizeof(candidate));
            return WIFI_PROVISIONING_MALFORMED;
        }
        if (cursor < length) {
            cursor++;
        }
    }
    if (!have_ssid || !have_password) {
        secure_zero(&candidate, sizeof(candidate));
        return WIFI_PROVISIONING_MALFORMED;
    }
    *submission_out = candidate;
    secure_zero(&candidate, sizeof(candidate));
    return WIFI_PROVISIONING_OK;
}

static bool bounded_copy(char *destination, size_t destination_capacity,
                         const char *source, size_t source_capacity)
{
    const char *terminator;
    size_t length;
    if (destination == NULL || source == NULL || destination_capacity == 0U ||
        source_capacity == 0U) {
        return false;
    }
    terminator = (const char *)memchr(source, '\0', source_capacity);
    if (terminator == NULL) {
        return false;
    }
    length = (size_t)(terminator - source);
    if (length >= destination_capacity) {
        return false;
    }
    (void)memcpy(destination, source, length + 1U);
    return true;
}

bool wifi_provisioning_candidate_build(
    const runtime_config_t *active,
    const wifi_provisioning_submission_t *submission,
    runtime_config_t *candidate_out)
{
    runtime_config_t candidate;
    bool valid = false;

    if (candidate_out != NULL) {
        secure_zero(candidate_out, sizeof(*candidate_out));
    }
    secure_zero(&candidate, sizeof(candidate));
    if (active != NULL && submission != NULL && candidate_out != NULL &&
        runtime_config_validate(active, NULL)) {
        candidate = *active;
        if (bounded_copy(candidate.wifi_ssid, sizeof(candidate.wifi_ssid),
                         submission->ssid, sizeof(submission->ssid)) &&
            bounded_copy(candidate.wifi_password, sizeof(candidate.wifi_password),
                         submission->password, sizeof(submission->password)) &&
            runtime_config_validate(&candidate, NULL)) {
            *candidate_out = candidate;
            valid = true;
        }
    }
    secure_zero(&candidate, sizeof(candidate));
    return valid;
}
