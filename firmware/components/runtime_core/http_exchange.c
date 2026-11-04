#include "runtime_core.h"

#include <limits.h>
#include <string.h>

static bool bounded_length(const char *text, size_t capacity, size_t *length_out)
{
    size_t index;
    if (text == NULL || length_out == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; index++) {
        if (text[index] == '\0') {
            *length_out = index;
            return true;
        }
    }
    return false;
}

static bool header_name_is(const char *name, size_t length, const char *known, size_t known_length)
{
    size_t index;
    if (length != known_length) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        char left = name[index];
        char right = known[index];
        if (left >= 'A' && left <= 'Z') {
            left = (char)(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

static bool parse_decimal(const char *value, size_t length, uint32_t maximum, uint32_t *out)
{
    size_t index;
    uint32_t number = 0U;
    if (length == 0U || out == NULL || (length > 1U && value[0] == '0')) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        uint32_t digit;
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        digit = (uint32_t)(value[index] - '0');
        if (number > maximum / 10U ||
            (number == maximum / 10U && digit > maximum % 10U)) {
            return false;
        }
        number = number * 10U + digit;
    }
    *out = number;
    return true;
}

static bool timestamp_digit(const char *value, size_t index)
{
    return value[index] >= '0' && value[index] <= '9';
}

static unsigned timestamp_field(const char *value, size_t offset)
{
    return (unsigned)(value[offset] - '0') * 10U +
           (unsigned)(value[offset + 1U] - '0');
}

static bool valid_timestamp(const char *value, size_t length)
{
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned max_day;
    size_t index;

    if (length < 20U || length > 30U ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[length - 1U] != 'Z') {
        return false;
    }
    for (index = 0U; index < 20U; index++) {
        if (index == 4U || index == 7U || index == 10U || index == 13U || index == 16U || index == 19U) {
            continue;
        }
        if (!timestamp_digit(value, index)) {
            return false;
        }
    }
    if (length != 20U) {
        if (length < 22U || value[19] != '.' || length - 21U > 9U) {
            return false;
        }
        for (index = 20U; index < length - 1U; index++) {
            if (!timestamp_digit(value, index)) {
                return false;
            }
        }
    }
    year = (unsigned)(value[0] - '0') * 1000U +
           (unsigned)(value[1] - '0') * 100U +
           (unsigned)(value[2] - '0') * 10U +
           (unsigned)(value[3] - '0');
    month = timestamp_field(value, 5U);
    day = timestamp_field(value, 8U);
    hour = timestamp_field(value, 11U);
    minute = timestamp_field(value, 14U);
    second = timestamp_field(value, 17U);
    if (year < 1970U || month < 1U || month > 12U || hour > 23U || minute > 59U || second > 59U) {
        return false;
    }
    max_day = (month == 2U) ? 28U : 31U;
    if (month == 2U && (year % 4U == 0U) && (year % 100U != 0U || year % 400U == 0U)) {
        max_day = 29U;
    } else if (month == 4U || month == 6U || month == 9U || month == 11U) {
        max_day = 30U;
    }
    return day >= 1U && day <= max_day;
}

static void fail_exchange(runtime_http_exchange_t *exchange)
{
    if (exchange == NULL) {
        return;
    }
    memset(exchange, 0, sizeof(*exchange));
    exchange->failed = true;
}

void runtime_http_exchange_init(runtime_http_exchange_t *exchange)
{
    if (exchange != NULL) {
        memset(exchange, 0, sizeof(*exchange));
    }
}

bool runtime_http_exchange_set_status(runtime_http_exchange_t *exchange, unsigned status)
{
    if (exchange == NULL || exchange->failed || exchange->status_set || status < 100U || status > 599U) {
        if (exchange != NULL && !exchange->failed) {
            fail_exchange(exchange);
        }
        return false;
    }
    if ((status >= 400U && exchange->body_length > GLUCOSE_ERROR_JSON_MAX_BYTES) ||
        (status == 304U && exchange->body_length != 0U)) {
        fail_exchange(exchange);
        return false;
    }
    exchange->status = status;
    exchange->status_set = true;
    return true;
}

bool runtime_http_exchange_add_header(runtime_http_exchange_t *exchange,
                                      const char *name, size_t name_capacity,
                                      const char *value, size_t value_capacity)
{
    size_t name_length;
    size_t value_length;
    bool recognized;

    if (exchange == NULL || exchange->failed ||
        !bounded_length(name, name_capacity, &name_length) ||
        !bounded_length(value, value_capacity, &value_length)) {
        if (exchange != NULL && !exchange->failed) {
            fail_exchange(exchange);
        }
        return false;
    }
    recognized = header_name_is(name, name_length, "etag", 4U) ||
                 header_name_is(name, name_length, "retry-after", 11U) ||
                 header_name_is(name, name_length, "x-reading-age-s", 15U) ||
                 header_name_is(name, name_length, "x-reading-server-time", 21U);
    if (!recognized) {
        return true;
    }
    if (header_name_is(name, name_length, "etag", 4U)) {
        size_t index;
        if (exchange->has_etag || value_length == 0U || value_length > 128U) {
            fail_exchange(exchange);
            return false;
        }
        for (index = 0U; index < value_length; index++) {
            if ((unsigned char)value[index] < 0x21U || (unsigned char)value[index] > 0x7eU) {
                fail_exchange(exchange);
                return false;
            }
        }
        memcpy(exchange->etag, value, value_length);
        exchange->etag[value_length] = '\0';
        exchange->has_etag = true;
        return true;
    }
    if (header_name_is(name, name_length, "retry-after", 11U)) {
        if (exchange->has_retry_after ||
            !parse_decimal(value, value_length, 3600U, &exchange->retry_after_s)) {
            fail_exchange(exchange);
            return false;
        }
        exchange->has_retry_after = true;
        return true;
    }
    if (header_name_is(name, name_length, "x-reading-age-s", 15U)) {
        if (exchange->has_reading_age ||
            !parse_decimal(value, value_length, UINT32_MAX, &exchange->reading_age_s)) {
            fail_exchange(exchange);
            return false;
        }
        exchange->has_reading_age = true;
        return true;
    }
    if (exchange->has_reading_server_time || value_length >= RUNTIME_HTTP_SERVER_TIME_CAPACITY ||
        !valid_timestamp(value, value_length)) {
        fail_exchange(exchange);
        return false;
    }
    memcpy(exchange->reading_server_time, value, value_length + 1U);
    exchange->has_reading_server_time = true;
    return true;
}

bool runtime_http_exchange_append_body(runtime_http_exchange_t *exchange,
                                       const uint8_t *data, size_t length)
{
    size_t maximum;
    if (exchange == NULL || exchange->failed ||
        (data == NULL && length != 0U) || length > RUNTIME_HTTP_BODY_CAPACITY - exchange->body_length) {
        if (exchange != NULL && !exchange->failed) {
            fail_exchange(exchange);
        }
        return false;
    }
    maximum = exchange->status_set && exchange->status >= 400U
                  ? GLUCOSE_ERROR_JSON_MAX_BYTES
                  : RUNTIME_HTTP_BODY_CAPACITY;
    if (exchange->status_set && exchange->status == 304U) {
        if (length != 0U) {
            fail_exchange(exchange);
            return false;
        }
        return true;
    }
    if (exchange->body_length + length > maximum) {
        fail_exchange(exchange);
        return false;
    }
    if (length != 0U) {
        memcpy(exchange->body + exchange->body_length, data, length);
        exchange->body_length += length;
    }
    return true;
}

runtime_http_result_t runtime_http_exchange_finish(runtime_http_exchange_t *exchange)
{
    if (exchange == NULL || exchange->failed || !exchange->status_set) {
        return RUNTIME_HTTP_RESULT_MALFORMED;
    }
    if (exchange->status == 200U) {
        return RUNTIME_HTTP_RESULT_BODY;
    }
    if (exchange->status >= 400U && exchange->status <= 599U) {
        return RUNTIME_HTTP_RESULT_ERROR_BODY;
    }
    if (exchange->status == 304U && exchange->body_length == 0U &&
        exchange->has_reading_age && exchange->has_reading_server_time) {
        return RUNTIME_HTTP_RESULT_NOT_MODIFIED;
    }
    if (exchange->status == 304U) {
        fail_exchange(exchange);
        return RUNTIME_HTTP_RESULT_MALFORMED;
    }
    return RUNTIME_HTTP_RESULT_UNKNOWN_STATUS;
}
