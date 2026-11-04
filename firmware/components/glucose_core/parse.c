#include "glucose_core.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GLUCOSE_LATEST_FIELD_COUNT 13U
#define GLUCOSE_ERROR_FIELD_COUNT 3U
#define JSON_NUMBER_CAPACITY 64U
#define JSON_KEY_CAPACITY 32U
#define JSON_ENUM_CAPACITY 32U
#define JSON_DETAIL_CAPACITY 241U
#define NANOSECONDS_PER_SECOND 1000000000LL

typedef struct json_cursor {
    const char *text;
    size_t length;
    size_t position;
} json_cursor_t;

typedef struct parsed_time {
    int64_t seconds;
    int32_t nanoseconds;
} parsed_time_t;

static void skip_whitespace(json_cursor_t *cursor)
{
    while (cursor->position < cursor->length) {
        char character = cursor->text[cursor->position];
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
            break;
        }
        cursor->position++;
    }
}

static bool append_byte(char *output, size_t capacity, size_t *used, unsigned int byte)
{
    if (*used + 1U >= capacity || byte > 0xFFU || byte == 0U) {
        return false;
    }
    output[*used] = (char)byte;
    (*used)++;
    return true;
}

static bool parse_hex4(json_cursor_t *cursor, uint32_t *value_out)
{
    uint32_t value = 0U;
    unsigned int index;

    if (cursor->length - cursor->position < 4U) {
        return false;
    }
    for (index = 0U; index < 4U; index++) {
        unsigned char character = (unsigned char)cursor->text[cursor->position++];
        uint32_t digit;
        if (character >= '0' && character <= '9') {
            digit = (uint32_t)(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = (uint32_t)(character - 'a') + 10U;
        } else if (character >= 'A' && character <= 'F') {
            digit = (uint32_t)(character - 'A') + 10U;
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    *value_out = value;
    return true;
}

static bool append_utf8(char *output, size_t capacity, size_t *used, uint32_t code_point)
{
    if (code_point == 0U || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
        return false;
    }
    if (code_point <= 0x7FU) {
        return append_byte(output, capacity, used, code_point);
    }
    if (code_point <= 0x7FFU) {
        return append_byte(output, capacity, used, 0xC0U | (code_point >> 6U)) &&
               append_byte(output, capacity, used, 0x80U | (code_point & 0x3FU));
    }
    if (code_point <= 0xFFFFU) {
        return append_byte(output, capacity, used, 0xE0U | (code_point >> 12U)) &&
               append_byte(output, capacity, used, 0x80U | ((code_point >> 6U) & 0x3FU)) &&
               append_byte(output, capacity, used, 0x80U | (code_point & 0x3FU));
    }
    return append_byte(output, capacity, used, 0xF0U | (code_point >> 18U)) &&
           append_byte(output, capacity, used, 0x80U | ((code_point >> 12U) & 0x3FU)) &&
           append_byte(output, capacity, used, 0x80U | ((code_point >> 6U) & 0x3FU)) &&
           append_byte(output, capacity, used, 0x80U | (code_point & 0x3FU));
}

static bool append_raw_utf8(json_cursor_t *cursor,
                            char *output,
                            size_t capacity,
                            size_t *used,
                            unsigned int first_byte)
{
    unsigned int continuation_count;
    unsigned int second_min = 0x80U;
    unsigned int second_max = 0xBFU;
    unsigned int index;

    if (first_byte >= 0xC2U && first_byte <= 0xDFU) {
        continuation_count = 1U;
    } else if (first_byte >= 0xE0U && first_byte <= 0xEFU) {
        continuation_count = 2U;
        if (first_byte == 0xE0U) {
            second_min = 0xA0U;
        } else if (first_byte == 0xEDU) {
            second_max = 0x9FU;
        }
    } else if (first_byte >= 0xF0U && first_byte <= 0xF4U) {
        continuation_count = 3U;
        if (first_byte == 0xF0U) {
            second_min = 0x90U;
        } else if (first_byte == 0xF4U) {
            second_max = 0x8FU;
        }
    } else {
        return false;
    }
    if (cursor->length - cursor->position < continuation_count) {
        return false;
    }
    if ((unsigned char)cursor->text[cursor->position] < second_min ||
        (unsigned char)cursor->text[cursor->position] > second_max ||
        !append_byte(output, capacity, used, first_byte)) {
        return false;
    }
    for (index = 0U; index < continuation_count; index++) {
        unsigned int byte = (unsigned char)cursor->text[cursor->position++];
        if (byte < 0x80U || byte > 0xBFU ||
            !append_byte(output, capacity, used, byte)) {
            return false;
        }
    }
    return true;
}

static bool parse_json_string(json_cursor_t *cursor, char *output, size_t capacity)
{
    size_t used = 0U;

    skip_whitespace(cursor);
    if (output == NULL || capacity == 0U || cursor->position >= cursor->length ||
        cursor->text[cursor->position] != '"') {
        return false;
    }
    cursor->position++;
    while (cursor->position < cursor->length) {
        unsigned char character = (unsigned char)cursor->text[cursor->position++];
        if (character == '"') {
            output[used] = '\0';
            return true;
        }
        if (character < 0x20U) {
            return false;
        }
        if (character != '\\') {
            bool appended = character < 0x80U
                                ? append_byte(output, capacity, &used, character)
                                : append_raw_utf8(cursor, output, capacity, &used, character);
            if (!appended) {
                return false;
            }
            continue;
        }
        if (cursor->position >= cursor->length) {
            return false;
        }
        character = (unsigned char)cursor->text[cursor->position++];
        switch (character) {
        case '"':
        case '\\':
        case '/':
            if (!append_byte(output, capacity, &used, character)) {
                return false;
            }
            break;
        case 'b':
            if (!append_byte(output, capacity, &used, '\b')) {
                return false;
            }
            break;
        case 'f':
            if (!append_byte(output, capacity, &used, '\f')) {
                return false;
            }
            break;
        case 'n':
            if (!append_byte(output, capacity, &used, '\n')) {
                return false;
            }
            break;
        case 'r':
            if (!append_byte(output, capacity, &used, '\r')) {
                return false;
            }
            break;
        case 't':
            if (!append_byte(output, capacity, &used, '\t')) {
                return false;
            }
            break;
        case 'u': {
            uint32_t code_point;
            if (!parse_hex4(cursor, &code_point)) {
                return false;
            }
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                uint32_t low_surrogate;
                if (cursor->length - cursor->position < 6U ||
                    cursor->text[cursor->position] != '\\' ||
                    cursor->text[cursor->position + 1U] != 'u') {
                    return false;
                }
                cursor->position += 2U;
                if (!parse_hex4(cursor, &low_surrogate) ||
                    low_surrogate < 0xDC00U || low_surrogate > 0xDFFFU) {
                    return false;
                }
                code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                             (low_surrogate - 0xDC00U);
            }
            if (!append_utf8(output, capacity, &used, code_point)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

static bool parse_literal(json_cursor_t *cursor, const char *literal)
{
    size_t literal_length = strlen(literal);
    skip_whitespace(cursor);
    if (cursor->length - cursor->position < literal_length ||
        memcmp(cursor->text + cursor->position, literal, literal_length) != 0) {
        return false;
    }
    cursor->position += literal_length;
    return true;
}

static bool parse_json_number(json_cursor_t *cursor, double *value_out)
{
    size_t start;
    size_t number_length;
    char number[JSON_NUMBER_CAPACITY];
    char *end_pointer;
    double value;

    skip_whitespace(cursor);
    start = cursor->position;
    if (cursor->position < cursor->length && cursor->text[cursor->position] == '-') {
        cursor->position++;
    }
    if (cursor->position >= cursor->length) {
        return false;
    }
    if (cursor->text[cursor->position] == '0') {
        cursor->position++;
        if (cursor->position < cursor->length &&
            isdigit((unsigned char)cursor->text[cursor->position])) {
            return false;
        }
    } else if (cursor->text[cursor->position] >= '1' && cursor->text[cursor->position] <= '9') {
        while (cursor->position < cursor->length &&
               isdigit((unsigned char)cursor->text[cursor->position])) {
            cursor->position++;
        }
    } else {
        return false;
    }
    if (cursor->position < cursor->length && cursor->text[cursor->position] == '.') {
        cursor->position++;
        if (cursor->position >= cursor->length ||
            !isdigit((unsigned char)cursor->text[cursor->position])) {
            return false;
        }
        while (cursor->position < cursor->length &&
               isdigit((unsigned char)cursor->text[cursor->position])) {
            cursor->position++;
        }
    }
    if (cursor->position < cursor->length &&
        (cursor->text[cursor->position] == 'e' || cursor->text[cursor->position] == 'E')) {
        cursor->position++;
        if (cursor->position < cursor->length &&
            (cursor->text[cursor->position] == '+' || cursor->text[cursor->position] == '-')) {
            cursor->position++;
        }
        if (cursor->position >= cursor->length ||
            !isdigit((unsigned char)cursor->text[cursor->position])) {
            return false;
        }
        while (cursor->position < cursor->length &&
               isdigit((unsigned char)cursor->text[cursor->position])) {
            cursor->position++;
        }
    }

    number_length = cursor->position - start;
    if (number_length == 0U || number_length >= sizeof(number)) {
        return false;
    }
    memcpy(number, cursor->text + start, number_length);
    number[number_length] = '\0';
    errno = 0;
    end_pointer = NULL;
    value = strtod(number, &end_pointer);
    if (errno == ERANGE || end_pointer == NULL || *end_pointer != '\0' || !isfinite(value)) {
        return false;
    }
    *value_out = value;
    return true;
}

static bool double_to_u32(double value, uint32_t maximum, uint32_t *output)
{
    if (!isfinite(value) || value < 0.0 || value > (double)maximum || floor(value) != value) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

static bool parse_u32(json_cursor_t *cursor, uint32_t maximum, uint32_t *output)
{
    double value;
    return parse_json_number(cursor, &value) && double_to_u32(value, maximum, output);
}

static bool parse_digit_sequence(const char *text, size_t offset, size_t count, int *value_out)
{
    size_t index;
    int value = 0;
    for (index = 0U; index < count; index++) {
        unsigned char character = (unsigned char)text[offset + index];
        if (!isdigit(character)) {
            return false;
        }
        value = value * 10 + (int)(character - '0');
    }
    *value_out = value;
    return true;
}

static bool is_leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return month_days[month - 1];
}

static int64_t days_from_civil(int year, unsigned int month, unsigned int day)
{
    int adjusted_year = year - (month <= 2U ? 1 : 0);
    int era = adjusted_year / 400;
    unsigned int year_of_era = (unsigned int)(adjusted_year - era * 400);
    unsigned int adjusted_month = month > 2U ? month - 3U : month + 9U;
    unsigned int day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U -
                              year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static bool parse_timestamp(const char *text, parsed_time_t *time_out)
{
    size_t length = strlen(text);
    size_t position = 19U;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int32_t nanoseconds = 0;
    int fraction_digits = 0;

    if (length < 20U || length > 30U || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[length - 1U] != 'Z' ||
        !parse_digit_sequence(text, 0U, 4U, &year) ||
        !parse_digit_sequence(text, 5U, 2U, &month) ||
        !parse_digit_sequence(text, 8U, 2U, &day) ||
        !parse_digit_sequence(text, 11U, 2U, &hour) ||
        !parse_digit_sequence(text, 14U, 2U, &minute) ||
        !parse_digit_sequence(text, 17U, 2U, &second)) {
        return false;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    if (text[position] == '.') {
        position++;
        while (position < length - 1U && fraction_digits < 9) {
            unsigned char character = (unsigned char)text[position++];
            if (!isdigit(character)) {
                return false;
            }
            nanoseconds = nanoseconds * 10 + (int32_t)(character - '0');
            fraction_digits++;
        }
        if (fraction_digits == 0 || position != length - 1U) {
            return false;
        }
        while (fraction_digits < 9) {
            nanoseconds *= 10;
            fraction_digits++;
        }
    } else if (position != length - 1U) {
        return false;
    }
    time_out->seconds = days_from_civil(year, (unsigned int)month, (unsigned int)day) * 86400LL +
                        (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second;
    time_out->nanoseconds = nanoseconds;
    return true;
}

static int latest_field_index(const char *key)
{
    static const char *const fields[GLUCOSE_LATEST_FIELD_COUNT] = {
        "glucose", "unit", "trend", "range", "target_low", "target_high",
        "timestamp", "age_s", "server_time", "source", "poll_after_s", "schema",
        "libre_local_time"
    };
    unsigned int index;
    for (index = 0U; index < GLUCOSE_LATEST_FIELD_COUNT; index++) {
        if (strcmp(key, fields[index]) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool parse_unit(json_cursor_t *cursor, glucose_unit_t *unit_out)
{
    char value[JSON_ENUM_CAPACITY];
    if (!parse_json_string(cursor, value, sizeof(value))) {
        return false;
    }
    if (strcmp(value, "mg/dL") == 0) {
        *unit_out = GLUCOSE_UNIT_MG_DL;
        return true;
    }
    if (strcmp(value, "mmol/L") == 0) {
        *unit_out = GLUCOSE_UNIT_MMOL_L;
        return true;
    }
    return false;
}

static bool parse_trend(json_cursor_t *cursor, glucose_trend_t *trend_out)
{
    char value[JSON_ENUM_CAPACITY];
    int trend;
    if (!parse_json_string(cursor, value, sizeof(value))) {
        return false;
    }
    for (trend = 0; trend < (int)GLUCOSE_TREND_COUNT; trend++) {
        if (strcmp(value, glucose_trend_name((glucose_trend_t)trend)) == 0) {
            *trend_out = (glucose_trend_t)trend;
            return true;
        }
    }
    return false;
}

static bool parse_range(json_cursor_t *cursor, glucose_range_t *range_out)
{
    char value[JSON_ENUM_CAPACITY];
    int range;
    if (!parse_json_string(cursor, value, sizeof(value))) {
        return false;
    }
    for (range = 0; range < (int)GLUCOSE_RANGE_COUNT; range++) {
        if (strcmp(value, glucose_range_name((glucose_range_t)range)) == 0) {
            *range_out = (glucose_range_t)range;
            return true;
        }
    }
    return false;
}

static bool parse_source(json_cursor_t *cursor, glucose_source_t *source_out)
{
    char value[JSON_ENUM_CAPACITY];
    if (!parse_json_string(cursor, value, sizeof(value))) {
        return false;
    }
    if (strcmp(value, "librelinkup") == 0) {
        *source_out = GLUCOSE_SOURCE_LIBRELINKUP;
        return true;
    }
    if (strcmp(value, "mock") == 0) {
        *source_out = GLUCOSE_SOURCE_MOCK;
        return true;
    }
    return false;
}

static bool parse_nullable_number(json_cursor_t *cursor, bool *is_null, double *value_out)
{
    size_t saved_position;
    skip_whitespace(cursor);
    saved_position = cursor->position;
    if (parse_literal(cursor, "null")) {
        *is_null = true;
        *value_out = 0.0;
        return true;
    }
    cursor->position = saved_position;
    *is_null = false;
    return parse_json_number(cursor, value_out);
}

static bool parse_local_time(json_cursor_t *cursor, uint32_t *seconds_out)
{
    char value[GLUCOSE_LOCAL_TIME_CAPACITY];
    unsigned int hour;
    unsigned int minute;
    unsigned int second;

    if (seconds_out == NULL ||
        !parse_json_string(cursor, value, sizeof(value)) ||
        strlen(value) != 8U || value[2] != ':' || value[5] != ':' ||
        value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9' ||
        value[6] < '0' || value[6] > '9' ||
        value[7] < '0' || value[7] > '9') {
        return false;
    }
    hour = (unsigned int)(value[0] - '0') * 10U + (unsigned int)(value[1] - '0');
    minute = (unsigned int)(value[3] - '0') * 10U + (unsigned int)(value[4] - '0');
    second = (unsigned int)(value[6] - '0') * 10U + (unsigned int)(value[7] - '0');
    if (hour > 23U || minute > 59U || second > 59U) {
        return false;
    }
    *seconds_out = hour * 3600U + minute * 60U + second;
    return true;
}

glucose_parse_status_t glucose_parse_latest_json(const char *json,
                                                  size_t length,
                                                  glucose_reading_t *reading_out)
{
    json_cursor_t cursor;
    glucose_reading_t parsed;
    uint32_t seen = 0U;
    double glucose_wire = 0.0;
    double target_low_wire = 0.0;
    double target_high_wire = 0.0;
    bool target_low_null = false;
    bool target_high_null = false;
    parsed_time_t reading_time;
    parsed_time_t server_time;

    if (reading_out == NULL || json == NULL) {
        if (reading_out != NULL) {
            memset(reading_out, 0, sizeof(*reading_out));
        }
        return GLUCOSE_PARSE_NULL_ARGUMENT;
    }
    memset(reading_out, 0, sizeof(*reading_out));
    if (length > GLUCOSE_LATEST_JSON_MAX_BYTES) {
        return GLUCOSE_PARSE_INVALID_VALUE;
    }
    memset(&parsed, 0, sizeof(parsed));
    cursor.text = json;
    cursor.length = length;
    cursor.position = 0U;
    skip_whitespace(&cursor);
    if (cursor.position >= cursor.length || cursor.text[cursor.position++] != '{') {
        return GLUCOSE_PARSE_INVALID_JSON;
    }

    skip_whitespace(&cursor);
    if (cursor.position < cursor.length && cursor.text[cursor.position] == '}') {
        return GLUCOSE_PARSE_MISSING_FIELD;
    }
    for (;;) {
        char key[JSON_KEY_CAPACITY];
        int field;
        glucose_parse_status_t value_error = GLUCOSE_PARSE_INVALID_VALUE;

        if (!parse_json_string(&cursor, key, sizeof(key))) {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        skip_whitespace(&cursor);
        if (cursor.position >= cursor.length || cursor.text[cursor.position++] != ':') {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        field = latest_field_index(key);
        if (field < 0) {
            return GLUCOSE_PARSE_UNKNOWN_FIELD;
        }
        if ((seen & (1U << (unsigned int)field)) != 0U) {
            return GLUCOSE_PARSE_DUPLICATE_FIELD;
        }
        seen |= 1U << (unsigned int)field;

        switch (field) {
        case 0:
            if (!parse_json_number(&cursor, &glucose_wire)) {
                return cursor.position >= cursor.length ? GLUCOSE_PARSE_INVALID_JSON : value_error;
            }
            break;
        case 1:
            if (!parse_unit(&cursor, &parsed.unit)) {
                return value_error;
            }
            break;
        case 2:
            if (!parse_trend(&cursor, &parsed.trend)) {
                return value_error;
            }
            break;
        case 3:
            if (!parse_range(&cursor, &parsed.range)) {
                return value_error;
            }
            break;
        case 4:
            if (!parse_nullable_number(&cursor, &target_low_null, &target_low_wire)) {
                return value_error;
            }
            break;
        case 5:
            if (!parse_nullable_number(&cursor, &target_high_null, &target_high_wire)) {
                return value_error;
            }
            break;
        case 6:
            if (!parse_json_string(&cursor, parsed.timestamp, sizeof(parsed.timestamp))) {
                return value_error;
            }
            break;
        case 7:
            if (!parse_u32(&cursor, UINT32_MAX, &parsed.age_s)) {
                return value_error;
            }
            break;
        case 8:
            if (!parse_json_string(&cursor, parsed.server_time, sizeof(parsed.server_time))) {
                return value_error;
            }
            break;
        case 9:
            if (!parse_source(&cursor, &parsed.source)) {
                return value_error;
            }
            break;
        case 10:
            if (!parse_u32(&cursor, 300U, &parsed.poll_after_s) || parsed.poll_after_s < 20U) {
                return value_error;
            }
            break;
        case 11: {
            uint32_t schema;
            if (!parse_u32(&cursor, UINT32_MAX, &schema)) {
                return value_error;
            }
            if (schema != 2U) {
                return GLUCOSE_PARSE_UNSUPPORTED_SCHEMA;
            }
            break;
        }
        case 12:
            if (!parse_local_time(&cursor, &parsed.libre_local_seconds)) {
                return value_error;
            }
            break;
        default:
            return GLUCOSE_PARSE_UNKNOWN_FIELD;
        }

        skip_whitespace(&cursor);
        if (cursor.position >= cursor.length) {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        if (cursor.text[cursor.position] == ',') {
            cursor.position++;
            continue;
        }
        if (cursor.text[cursor.position] == '}') {
            cursor.position++;
            break;
        }
        return GLUCOSE_PARSE_INVALID_JSON;
    }
    skip_whitespace(&cursor);
    if (cursor.position != cursor.length) {
        return GLUCOSE_PARSE_INVALID_JSON;
    }
    if (seen != ((1U << GLUCOSE_LATEST_FIELD_COUNT) - 1U)) {
        return GLUCOSE_PARSE_MISSING_FIELD;
    }
    if (target_low_null != target_high_null) {
        return GLUCOSE_PARSE_INVALID_VALUE;
    }

    if (parsed.unit == GLUCOSE_UNIT_MG_DL) {
        if (glucose_wire < 20.0 || glucose_wire > 500.0 || floor(glucose_wire) != glucose_wire) {
            return GLUCOSE_PARSE_INVALID_VALUE;
        }
        parsed.value_mg_dl = glucose_wire;
        if (!target_low_null) {
            if (target_low_wire < 0.0 || target_high_wire > 500.0 ||
                target_low_wire > target_high_wire) {
                return GLUCOSE_PARSE_INVALID_VALUE;
            }
            parsed.target_low_mg_dl = target_low_wire;
            parsed.target_high_mg_dl = target_high_wire;
        }
    } else {
        if (glucose_wire < 1.0 || glucose_wire > 28.0) {
            return GLUCOSE_PARSE_INVALID_VALUE;
        }
        parsed.value_mg_dl = glucose_wire * 18.0;
        if (!target_low_null) {
            if (target_low_wire < 0.0 || target_high_wire > 28.0 ||
                target_low_wire > target_high_wire) {
                return GLUCOSE_PARSE_INVALID_VALUE;
            }
            parsed.target_low_mg_dl = target_low_wire * 18.0;
            parsed.target_high_mg_dl = target_high_wire * 18.0;
        }
    }
    parsed.has_target_range = !target_low_null;

    if (!parse_timestamp(parsed.timestamp, &reading_time) ||
        !parse_timestamp(parsed.server_time, &server_time)) {
        return GLUCOSE_PARSE_INVALID_VALUE;
    }
    {
        int64_t seconds_delta = server_time.seconds - reading_time.seconds;
        int64_t nanoseconds_delta = (int64_t)server_time.nanoseconds - reading_time.nanoseconds;
        int64_t total_nanoseconds;
        uint64_t derived_age;
        if (seconds_delta < 0 || seconds_delta > (int64_t)UINT32_MAX + 1LL) {
            return GLUCOSE_PARSE_TIME_MISMATCH;
        }
        total_nanoseconds = seconds_delta * NANOSECONDS_PER_SECOND + nanoseconds_delta;
        if (total_nanoseconds < 0) {
            return GLUCOSE_PARSE_TIME_MISMATCH;
        }
        derived_age = (uint64_t)(total_nanoseconds / NANOSECONDS_PER_SECOND);
        if (derived_age != parsed.age_s) {
            return GLUCOSE_PARSE_TIME_MISMATCH;
        }
    }

    *reading_out = parsed;
    return GLUCOSE_PARSE_OK;
}

static int error_field_index(const char *key)
{
    if (strcmp(key, "error") == 0) {
        return 0;
    }
    if (strcmp(key, "detail") == 0) {
        return 1;
    }
    if (strcmp(key, "retry_after_s") == 0) {
        return 2;
    }
    return -1;
}

static bool parse_error_code(json_cursor_t *cursor, glucose_error_code_t *code_out)
{
    char value[JSON_ENUM_CAPACITY];
    int code;
    if (!parse_json_string(cursor, value, sizeof(value))) {
        return false;
    }
    for (code = 0; code < (int)GLUCOSE_ERROR_CODE_COUNT; code++) {
        if (strcmp(value, glucose_error_code_name((glucose_error_code_t)code)) == 0) {
            *code_out = (glucose_error_code_t)code;
            return true;
        }
    }
    return false;
}

glucose_parse_status_t glucose_parse_error_json(const char *json,
                                                 size_t length,
                                                 glucose_error_payload_t *error_out)
{
    json_cursor_t cursor;
    glucose_error_payload_t parsed;
    uint32_t seen = 0U;

    if (error_out == NULL || json == NULL) {
        if (error_out != NULL) {
            memset(error_out, 0, sizeof(*error_out));
        }
        return GLUCOSE_PARSE_NULL_ARGUMENT;
    }
    memset(error_out, 0, sizeof(*error_out));
    if (length > GLUCOSE_ERROR_JSON_MAX_BYTES) {
        return GLUCOSE_PARSE_INVALID_VALUE;
    }
    memset(&parsed, 0, sizeof(parsed));
    cursor.text = json;
    cursor.length = length;
    cursor.position = 0U;
    skip_whitespace(&cursor);
    if (cursor.position >= cursor.length || cursor.text[cursor.position++] != '{') {
        return GLUCOSE_PARSE_INVALID_JSON;
    }

    for (;;) {
        char key[JSON_KEY_CAPACITY];
        int field;
        skip_whitespace(&cursor);
        if (cursor.position < cursor.length && cursor.text[cursor.position] == '}') {
            if (seen != 0U) {
                return GLUCOSE_PARSE_INVALID_JSON;
            }
            cursor.position++;
            break;
        }
        if (!parse_json_string(&cursor, key, sizeof(key))) {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        skip_whitespace(&cursor);
        if (cursor.position >= cursor.length || cursor.text[cursor.position++] != ':') {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        field = error_field_index(key);
        if (field < 0) {
            return GLUCOSE_PARSE_UNKNOWN_FIELD;
        }
        if ((seen & (1U << (unsigned int)field)) != 0U) {
            return GLUCOSE_PARSE_DUPLICATE_FIELD;
        }
        seen |= 1U << (unsigned int)field;

        if (field == 0) {
            if (!parse_error_code(&cursor, &parsed.code)) {
                return GLUCOSE_PARSE_INVALID_VALUE;
            }
        } else if (field == 1) {
            char detail[JSON_DETAIL_CAPACITY];
            if (!parse_json_string(&cursor, detail, sizeof(detail)) || detail[0] == '\0') {
                return GLUCOSE_PARSE_INVALID_VALUE;
            }
        } else if (!parse_u32(&cursor, 3600U, &parsed.retry_after_s)) {
            return GLUCOSE_PARSE_INVALID_VALUE;
        }

        skip_whitespace(&cursor);
        if (cursor.position >= cursor.length) {
            return GLUCOSE_PARSE_INVALID_JSON;
        }
        if (cursor.text[cursor.position] == ',') {
            cursor.position++;
            continue;
        }
        if (cursor.text[cursor.position] == '}') {
            cursor.position++;
            break;
        }
        return GLUCOSE_PARSE_INVALID_JSON;
    }
    skip_whitespace(&cursor);
    if (cursor.position != cursor.length) {
        return GLUCOSE_PARSE_INVALID_JSON;
    }
    if (seen != ((1U << GLUCOSE_ERROR_FIELD_COUNT) - 1U)) {
        return GLUCOSE_PARSE_MISSING_FIELD;
    }
    *error_out = parsed;
    return GLUCOSE_PARSE_OK;
}
