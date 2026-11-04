#include "runtime_core.h"

#include <string.h>

static size_t bounded_len(const char *text, size_t capacity)
{
    size_t length = 0U;

    if (text == NULL) {
        return capacity + 1U;
    }
    while (length < capacity && text[length] != '\0') {
        length++;
    }
    return length;
}

static bool ascii_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool base64url_char(char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '_' || value == '-';
}

static bool canonical_token_final_char(char value)
{
    static const char allowed[] = "AEIMQUYcgkosw048";
    size_t index;

    for (index = 0U; index < sizeof(allowed) - 1U; index++) {
        if (allowed[index] == value) {
            return true;
        }
    }
    return false;
}

static bool utf8_valid(const char *text, size_t length)
{
    size_t index = 0U;

    while (index < length) {
        unsigned char first = (unsigned char)text[index++];
        unsigned long codepoint;
        size_t continuation_count;
        size_t continuation;

        if (first <= 0x7fU) {
            if (first < 0x20U || first == 0x7fU) {
                return false;
            }
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1U;
            codepoint = (unsigned long)(first & 0x1fU);
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2U;
            codepoint = (unsigned long)(first & 0x0fU);
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3U;
            codepoint = (unsigned long)(first & 0x07U);
        } else {
            return false;
        }
        if (index + continuation_count > length) {
            return false;
        }
        for (continuation = 0U; continuation < continuation_count; continuation++) {
            unsigned char value = (unsigned char)text[index++];

            if (value < 0x80U || value > 0xbfU) {
                return false;
            }
            codepoint = (codepoint << 6) | (unsigned long)(value & 0x3fU);
        }
        if (codepoint > 0x10ffffUL ||
            (codepoint >= 0xd800UL && codepoint <= 0xdfffUL) ||
            (codepoint >= 0x80UL && codepoint <= 0x9fUL) ||
            (continuation_count == 1U && codepoint < 0x80UL) ||
            (continuation_count == 2U && codepoint < 0x800UL) ||
            (continuation_count == 3U && codepoint < 0x10000UL)) {
            return false;
        }
    }
    return true;
}

static bool string_range_valid(const char *text, size_t capacity,
                               size_t minimum, size_t maximum,
                               bool ascii_only)
{
    size_t length = bounded_len(text, capacity);
    size_t index;

    if (length < minimum || length > maximum || length == capacity) {
        return false;
    }
    if (ascii_only) {
        for (index = 0U; index < length; index++) {
            if ((unsigned char)text[index] > 0x7fU) {
                return false;
            }
        }
        return true;
    }
    return utf8_valid(text, length);
}

static bool decimal(const char *text, size_t length, size_t *position,
                    unsigned maximum, unsigned *value_out)
{
    size_t index = *position;
    unsigned value = 0U;

    if (index >= length || text[index] < '0' || text[index] > '9') {
        return false;
    }
    if (text[index] == '0' && index + 1U < length &&
        text[index + 1U] >= '0' && text[index + 1U] <= '9') {
        return false;
    }
    while (index < length && text[index] >= '0' && text[index] <= '9') {
        unsigned digit = (unsigned)(text[index] - '0');

        if (value > maximum / 10U ||
            (value == maximum / 10U && digit > maximum % 10U)) {
            return false;
        }
        value = value * 10U + digit;
        index++;
    }
    *position = index;
    *value_out = value;
    return true;
}

static bool two_digits(const char *text, size_t length, size_t *position,
                       unsigned maximum, unsigned *value_out)
{
    size_t index = *position;
    unsigned value;

    if (index + 2U > length || text[index] < '0' || text[index] > '9' ||
        text[index + 1U] < '0' || text[index + 1U] > '9') {
        return false;
    }
    value = (unsigned)(text[index] - '0') * 10U +
            (unsigned)(text[index + 1U] - '0');
    if (value > maximum) {
        return false;
    }
    *position = index + 2U;
    *value_out = value;
    return true;
}

static bool tz_name(const char *text, size_t length, size_t *position,
                    bool bracketed)
{
    size_t index = *position;
    size_t start;

    if (bracketed) {
        if (index >= length || text[index] != '<') {
            return false;
        }
        index++;
        start = index;
        while (index < length && text[index] != '>') {
            char value = text[index];

            if (!((value >= 'A' && value <= 'Z') ||
                  (value >= 'a' && value <= 'z') ||
                  (value >= '0' && value <= '9') ||
                  value == '+' || value == '-')) {
                return false;
            }
            index++;
        }
        if (index >= length || text[index] != '>' ||
            index - start < 3U || index - start > 16U) {
            return false;
        }
        *position = index + 1U;
        return true;
    }
    start = index;
    while (index < length &&
           ((text[index] >= 'A' && text[index] <= 'Z') ||
            (text[index] >= 'a' && text[index] <= 'z'))) {
        index++;
    }
    if (index - start < 3U || index - start > 16U) {
        return false;
    }
    *position = index;
    return true;
}

static bool tz_offset(const char *text, size_t length, size_t *position)
{
    size_t index = *position;
    unsigned hours;
    unsigned minutes = 0U;
    unsigned seconds = 0U;

    if (index < length && (text[index] == '+' || text[index] == '-')) {
        index++;
    }
    if (!decimal(text, length, &index, 24U, &hours)) {
        return false;
    }
    if (index < length && text[index] == ':') {
        index++;
        if (!two_digits(text, length, &index, 59U, &minutes)) {
            return false;
        }
        if (index < length && text[index] == ':') {
            index++;
            if (!two_digits(text, length, &index, 59U, &seconds)) {
                return false;
            }
        }
    }
    if (hours == 24U && (minutes != 0U || seconds != 0U)) {
        return false;
    }
    *position = index;
    return true;
}

static bool tz_rule(const char *text, size_t length, size_t *position)
{
    size_t index = *position;
    unsigned first;
    unsigned second;
    unsigned third;

    if (index >= length) {
        return false;
    }
    if (text[index] == 'J') {
        index++;
        if (!decimal(text, length, &index, 365U, &first) || first == 0U) {
            return false;
        }
    } else if (text[index] == 'M') {
        index++;
        if (!decimal(text, length, &index, 12U, &first) || first == 0U ||
            index >= length || text[index++] != '.') {
            return false;
        }
        if (!decimal(text, length, &index, 5U, &second) || second == 0U ||
            index >= length || text[index++] != '.') {
            return false;
        }
        if (!decimal(text, length, &index, 6U, &third)) {
            return false;
        }
    } else if (!decimal(text, length, &index, 365U, &first)) {
        return false;
    }
    if (index < length && text[index] == '/') {
        index++;
        if (index < length && (text[index] == '+' || text[index] == '-')) {
            return false;
        }
        if (!tz_offset(text, length, &index)) {
            return false;
        }
    }
    *position = index;
    return true;
}

static bool tz_valid(const char *text)
{
    size_t length = bounded_len(text, RUNTIME_POSIX_TZ_CAPACITY);
    size_t position = 0U;
    size_t index;

    if (length < 4U || length >= RUNTIME_POSIX_TZ_CAPACITY) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char value = (unsigned char)text[index];

        if (value < 0x21U || value > 0x7eU) {
            return false;
        }
    }
    if (!tz_name(text, length, &position, text[position] == '<') ||
        !tz_offset(text, length, &position)) {
        return false;
    }
    if (position == length) {
        return true;
    }
    if (!tz_name(text, length, &position, text[position] == '<')) {
        return false;
    }
    if (position < length && text[position] != ',') {
        if (!tz_offset(text, length, &position)) {
            return false;
        }
    }
    if (position >= length || text[position++] != ',' ||
        !tz_rule(text, length, &position) || position >= length ||
        text[position++] != ',' || !tz_rule(text, length, &position) ||
        position != length) {
        return false;
    }
    return true;
}

static bool numeric_label(const char *text, size_t start, size_t end)
{
    size_t index = start;
    bool has_digit = false;

    if (start >= end) {
        return false;
    }
    if (end - start >= 2U && text[start] == '0' &&
        (text[start + 1U] == 'x' || text[start + 1U] == 'X')) {
        index += 2U;
        while (index < end) {
            if (!ascii_hex(text[index])) {
                return false;
            }
            has_digit = true;
            index++;
        }
        return has_digit;
    }
    while (index < end) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        has_digit = true;
        index++;
    }
    return has_digit;
}

static bool numeric_host(const char *text, size_t start, size_t end)
{
    size_t index = start;
    size_t label_start = start;
    unsigned labels = 0U;

    while (index <= end) {
        if (index == end || text[index] == '.') {
            if (!numeric_label(text, label_start, index)) {
                return false;
            }
            labels++;
            label_start = index + 1U;
        }
        index++;
    }
    return labels >= 1U;
}

#if defined(RUNTIME_CORE_HOST_TEST) && defined(RUNTIME_ALLOW_IP_LITERAL)
static bool canonical_ipv4(const char *text, size_t start, size_t end)
{
    size_t index = start;
    unsigned octets = 0U;

    while (index < end) {
        unsigned value = 0U;
        unsigned digits = 0U;

        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        if (text[index] == '0' && index + 1U < end && text[index + 1U] != '.') {
            return false;
        }
        while (index < end && text[index] >= '0' && text[index] <= '9') {
            if (digits == 3U) {
                return false;
            }
            value = value * 10U + (unsigned)(text[index] - '0');
            digits++;
            index++;
        }
        if (digits == 0U || value > 255U) {
            return false;
        }
        octets++;
        if (index == end) {
            break;
        }
        if (text[index++] != '.') {
            return false;
        }
    }
    return octets == 4U;
}
#endif

static bool dns_host(const char *text, size_t start, size_t end)
{
    size_t index = start;
    size_t label_start = start;
    size_t labels = 0U;

    if (start >= end) {
        return false;
    }
    while (index <= end) {
        if (index == end || text[index] == '.') {
            size_t label_length = index - label_start;
            size_t label_index;

            if (label_length < 1U || label_length > 63U ||
                text[label_start] == '-' || text[index - 1U] == '-') {
                return false;
            }
            for (label_index = label_start; label_index < index; label_index++) {
                char value = text[label_index];

                if (!((value >= 'a' && value <= 'z') ||
                      (value >= '0' && value <= '9') || value == '-')) {
                    return false;
                }
            }
            labels++;
            label_start = index + 1U;
        }
        index++;
    }
    return labels >= 2U;
}

bool runtime_url_is_https_origin(const char *text, size_t capacity)
{
    size_t length;
    size_t host_start = 8U;
    size_t host_end;
    size_t index;
    unsigned port;

    if (text == NULL || capacity < 1U ||
        capacity > RUNTIME_BRIDGE_ORIGIN_CAPACITY) {
        return false;
    }
    length = bounded_len(text, capacity);
    if (length == capacity || length < 11U || length > 255U ||
        memcmp(text, "https://", 8U) != 0) {
        return false;
    }
    index = host_start;
    while (index < length && text[index] != ':' && text[index] != '/' &&
           text[index] != '?' && text[index] != '#') {
        if (text[index] == '\\' || text[index] == '%' ||
            (unsigned char)text[index] <= 0x20U) {
            return false;
        }
        index++;
    }
    host_end = index;
    if (host_start >= host_end) {
        return false;
    }
    for (index = host_start; index < host_end; index++) {
        if (text[index] == '\\' || text[index] == '%' ||
            (unsigned char)text[index] <= 0x20U) {
            return false;
        }
    }
    if (numeric_host(text, host_start, host_end)) {
#if defined(RUNTIME_CORE_HOST_TEST) && defined(RUNTIME_ALLOW_IP_LITERAL)
        if (!canonical_ipv4(text, host_start, host_end)) {
            return false;
        }
#else
        return false;
#endif
    } else if (!dns_host(text, host_start, host_end)) {
        return false;
    }
    if (index < length && text[index] == ':') {
        index++;
        if (!decimal(text, length, &index, 65535U, &port) ||
            port == 0U || index != length) {
            return false;
        }
    }
    return index == length;
}

static bool config_strings_valid(const runtime_config_t *config,
                                 runtime_config_error_t *error_out)
{
    size_t length;
    size_t index;

    if (!string_range_valid(config->wifi_ssid, RUNTIME_WIFI_SSID_CAPACITY,
                            1U, 32U, false)) {
        *error_out = RUNTIME_CONFIG_ERROR_SSID;
        return false;
    }
    length = bounded_len(config->wifi_password, RUNTIME_WIFI_PASSWORD_CAPACITY);
    if (length == RUNTIME_WIFI_PASSWORD_CAPACITY ||
        !(length == 0U || (length >= 8U && length <= 63U &&
                           utf8_valid(config->wifi_password, length)) ||
          length == 64U)) {
        *error_out = RUNTIME_CONFIG_ERROR_PASSWORD;
        return false;
    }
    if (length == 64U) {
        for (index = 0U; index < length; index++) {
            if (!ascii_hex(config->wifi_password[index])) {
                *error_out = RUNTIME_CONFIG_ERROR_PASSWORD;
                return false;
            }
        }
    }
    if (!runtime_url_is_https_origin(config->bridge_origin,
                                     RUNTIME_BRIDGE_ORIGIN_CAPACITY)) {
        *error_out = RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN;
        return false;
    }
    length = bounded_len(config->device_token, RUNTIME_DEVICE_TOKEN_CAPACITY);
    if (length != 43U) {
        *error_out = RUNTIME_CONFIG_ERROR_DEVICE_TOKEN;
        return false;
    }
    for (index = 0U; index < 42U; index++) {
        if (!base64url_char(config->device_token[index])) {
            *error_out = RUNTIME_CONFIG_ERROR_DEVICE_TOKEN;
            return false;
        }
    }
    if (!canonical_token_final_char(config->device_token[42])) {
        *error_out = RUNTIME_CONFIG_ERROR_DEVICE_TOKEN;
        return false;
    }
    return true;
}

bool runtime_config_validate(const runtime_config_t *config,
                             runtime_config_error_t *error_out)
{
    runtime_config_error_t error = RUNTIME_CONFIG_ERROR_NONE;

    if (config == NULL) {
        error = RUNTIME_CONFIG_ERROR_NULL_CONFIG;
    } else if (!config_strings_valid(config, &error)) {
        /* The string validator sets the first error. */
    } else if (config->unit >= GLUCOSE_UNIT_COUNT) {
        error = RUNTIME_CONFIG_ERROR_UNIT;
    } else if (config->brightness_day_percent < 1U ||
               config->brightness_day_percent > 100U) {
        error = RUNTIME_CONFIG_ERROR_BRIGHTNESS_DAY;
    } else if (config->brightness_night_percent < 1U ||
               config->brightness_night_percent >
                   config->brightness_day_percent) {
        error = RUNTIME_CONFIG_ERROR_BRIGHTNESS_NIGHT;
    } else if (config->night_start_minute > 1439U ||
               config->night_end_minute > 1439U ||
               config->night_start_minute == config->night_end_minute) {
        error = RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE;
    } else if (!tz_valid(config->tz)) {
        error = RUNTIME_CONFIG_ERROR_TZ;
    } else if (!(config->freshness.fresh_max_s >= 1U &&
                 config->freshness.fresh_max_s < config->freshness.aging_max_s &&
                 config->freshness.aging_max_s <= 900U &&
                 config->freshness.stale_max_s > 900U &&
                 config->freshness.stale_max_s <= 86400U)) {
        error = RUNTIME_CONFIG_ERROR_FRESHNESS;
    } else if (config->poll_s < 20U || config->poll_s > 300U) {
        error = RUNTIME_CONFIG_ERROR_POLL;
    } else if (config->ota_enabled > 1U) {
        error = RUNTIME_CONFIG_ERROR_OTA_FLAG;
    }
    if (error_out != NULL) {
        *error_out = error;
    }
    return error == RUNTIME_CONFIG_ERROR_NONE;
}

uint32_t runtime_poll_interval_clamp(uint32_t poll_s)
{
    if (poll_s < 20U) {
        return 20U;
    }
    if (poll_s > 300U) {
        return 300U;
    }
    return poll_s;
}
