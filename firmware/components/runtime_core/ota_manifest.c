#include "runtime_core.h"

#include <string.h>

typedef struct semver_view {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    bool prerelease;
    size_t prerelease_start;
    size_t length;
} semver_view_t;

typedef struct origin_view {
    size_t host_start;
    size_t host_end;
    unsigned port;
} origin_view_t;

static bool ascii_digit(char value)
{
    return value >= '0' && value <= '9';
}

static bool ascii_lower(char value)
{
    return value >= 'a' && value <= 'z';
}

static bool ascii_hex_lower(char value)
{
    return ascii_digit(value) || (value >= 'a' && value <= 'f');
}

static bool bounded_text(const char *text, size_t capacity, size_t maximum,
                         size_t *length_out)
{
    size_t index;

    if (text == NULL || length_out == NULL || capacity == 0U ||
        capacity > maximum) {
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

static bool literal_at(const char *text, size_t start, size_t length,
                       const char *literal, size_t literal_length)
{
    size_t index;

    if (start > length || literal_length > length - start) {
        return false;
    }
    for (index = 0U; index < literal_length; index++) {
        if (text[start + index] != literal[index]) {
            return false;
        }
    }
    return true;
}

static bool key_equals(const char *key, const char *literal, size_t literal_length)
{
    size_t index;

    for (index = 0U; index < literal_length; index++) {
        if (key[index] != literal[index]) {
            return false;
        }
    }
    return key[literal_length] == '\0';
}

static bool json_space(unsigned char value)
{
    return value == (unsigned char)' ' || value == (unsigned char)'\t' ||
           value == (unsigned char)'\r' || value == (unsigned char)'\n';
}

static void skip_json_space(const uint8_t *json, size_t length, size_t *cursor)
{
    while (*cursor < length && json_space(json[*cursor])) {
        (*cursor)++;
    }
}

static bool parse_json_string(const uint8_t *json, size_t length, size_t *cursor,
                              char *output, size_t output_capacity,
                              bool *overflow_out)
{
    size_t output_length = 0U;
    bool overflow = false;

    if (*cursor >= length || json[*cursor] != (uint8_t)'"') {
        return false;
    }
    (*cursor)++;
    while (*cursor < length && json[*cursor] != (uint8_t)'"') {
        uint8_t value = json[*cursor];

        if (value == (uint8_t)'\\' || value < 0x20U || value >= 0x80U) {
            return false;
        }
        if (output != NULL) {
            if (output_length + 1U < output_capacity) {
                output[output_length] = (char)value;
            } else {
                overflow = true;
            }
        }
        output_length++;
        (*cursor)++;
    }
    if (*cursor >= length || json[*cursor] != (uint8_t)'"') {
        return false;
    }
    (*cursor)++;
    if (output != NULL && output_capacity > 0U) {
        output[output_length < output_capacity ? output_length : output_capacity - 1U] = '\0';
    }
    if (overflow_out != NULL) {
        *overflow_out = overflow;
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
            if (!ascii_hex_lower(text[index]) &&
                !(text[index] >= 'A' && text[index] <= 'F')) {
                return false;
            }
            has_digit = true;
            index++;
        }
        return has_digit;
    }
    while (index < end) {
        if (!ascii_digit(text[index])) {
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

static bool dns_host(const char *text, size_t start, size_t end)
{
    size_t index = start;
    size_t label_start = start;
    unsigned labels = 0U;

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

                if (!(ascii_lower(value) || ascii_digit(value) || value == '-')) {
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

static bool decimal_port(const char *text, size_t start, size_t end,
                         unsigned *port_out)
{
    size_t index = start;
    unsigned value = 0U;

    if (start >= end || (end - start > 1U && text[start] == '0')) {
        return false;
    }
    while (index < end) {
        unsigned digit;

        if (!ascii_digit(text[index])) {
            return false;
        }
        digit = (unsigned)(text[index] - '0');
        if (value > (65535U - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        index++;
    }
    if (value == 0U) {
        return false;
    }
    *port_out = value;
    return true;
}

static bool path_segment_is_dot(const char *text, size_t start, size_t end)
{
    return end - start == 1U && text[start] == '.';
}

static bool path_segment_is_dot_dot(const char *text, size_t start, size_t end)
{
    return end - start == 2U && text[start] == '.' && text[start + 1U] == '.';
}

static bool validate_path(const char *text, size_t start, size_t length)
{
    size_t index = start;
    size_t segment_start;

    if (start >= length || text[start] != '/' || start + 1U >= length) {
        return false;
    }
    segment_start = start + 1U;
    for (index = start + 1U; index <= length; index++) {
        if (index == length || text[index] == '/') {
            size_t segment_length = index - segment_start;
            size_t value_index;

            if (segment_length == 0U ||
                path_segment_is_dot(text, segment_start, index) ||
                path_segment_is_dot_dot(text, segment_start, index)) {
                return false;
            }
            for (value_index = segment_start; value_index < index; value_index++) {
                char value = text[value_index];

                if (!(ascii_lower(value) || (value >= 'A' && value <= 'Z') ||
                      ascii_digit(value) || value == '/' || value == '-' ||
                      value == '.' || value == '_' || value == '~')) {
                    return false;
                }
            }
            segment_start = index + 1U;
        }
    }
    return text[length - 1U] != '/';
}

static bool parse_https_reference(const char *text, size_t capacity,
                                  bool require_path, origin_view_t *origin_out)
{
    size_t length;
    size_t authority_start = 8U;
    size_t authority_end;
    size_t host_end;
    size_t index;
    unsigned port = 443U;

    if (text == NULL || capacity == 0U ||
        capacity > (require_path ? RUNTIME_OTA_URL_CAPACITY : RUNTIME_BRIDGE_ORIGIN_CAPACITY) ||
        !bounded_text(text, capacity,
                      require_path ? RUNTIME_OTA_URL_CAPACITY : RUNTIME_BRIDGE_ORIGIN_CAPACITY,
                      &length) || length < (require_path ? 13U : 11U) ||
        !literal_at(text, 0U, length, "https://", 8U)) {
        return false;
    }
    index = authority_start;
    while (index < length && text[index] != '/' && text[index] != '?' &&
           text[index] != '#') {
        if (text[index] == '\\' || text[index] == '%' ||
            (unsigned char)text[index] <= 0x20U ||
            (unsigned char)text[index] >= 0x80U) {
            return false;
        }
        index++;
    }
    authority_end = index;
    if (authority_start >= authority_end) {
        return false;
    }
    host_end = authority_end;
    for (index = authority_start; index < authority_end; index++) {
        if (text[index] == ':') {
            host_end = index;
            break;
        }
    }
    if (host_end <= authority_start || numeric_host(text, authority_start, host_end) ||
        !dns_host(text, authority_start, host_end)) {
        return false;
    }
    if (host_end < authority_end &&
        !decimal_port(text, host_end + 1U, authority_end, &port)) {
        return false;
    }
    index = authority_end;
    if (require_path) {
        if (index >= length || text[index] != '/' ||
            !validate_path(text, index, length)) {
            return false;
        }
    } else if (index != length) {
        return false;
    }
    if (origin_out != NULL) {
        origin_out->host_start = authority_start;
        origin_out->host_end = host_end;
        origin_out->port = port;
    }
    return true;
}

static bool parse_u32_component(const char *text, size_t length, size_t *cursor,
                                uint32_t *value_out)
{
    size_t index = *cursor;
    size_t start = index;
    uint32_t value = 0U;

    if (index >= length || !ascii_digit(text[index])) {
        return false;
    }
    while (index < length && ascii_digit(text[index])) {
        uint32_t digit = (uint32_t)(text[index] - '0');

        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        index++;
    }
    if (index - start > 1U && text[start] == '0') {
        return false;
    }
    *cursor = index;
    *value_out = value;
    return true;
}

static bool parse_version(const char *text, size_t capacity, semver_view_t *view_out)
{
    size_t length;
    size_t cursor = 0U;
    size_t identifier_start;

    if (view_out == NULL || !bounded_text(text, capacity, RUNTIME_OTA_VERSION_CAPACITY,
                                          &length) || length == 0U || length > 31U) {
        return false;
    }
    if (!parse_u32_component(text, length, &cursor, &view_out->major) ||
        cursor >= length || text[cursor++] != '.' ||
        !parse_u32_component(text, length, &cursor, &view_out->minor) ||
        cursor >= length || text[cursor++] != '.' ||
        !parse_u32_component(text, length, &cursor, &view_out->patch)) {
        return false;
    }
    view_out->length = length;
    view_out->prerelease = false;
    view_out->prerelease_start = length;
    if (cursor < length) {
        if (text[cursor++] != '-' || cursor >= length) {
            return false;
        }
        view_out->prerelease = true;
        view_out->prerelease_start = cursor;
        identifier_start = cursor;
        while (cursor <= length) {
            if (cursor == length || text[cursor] == '.') {
                size_t identifier_length = cursor - identifier_start;
                size_t identifier_index;
                bool numeric = true;

                if (identifier_length == 0U) {
                    return false;
                }
                for (identifier_index = identifier_start;
                     identifier_index < cursor; identifier_index++) {
                    char value = text[identifier_index];

                    if (!(ascii_digit(value) || ascii_lower(value) || value == '-')) {
                        return false;
                    }
                    if (!ascii_digit(value)) {
                        numeric = false;
                    }
                }
                if (numeric && identifier_length > 1U &&
                    text[identifier_start] == '0') {
                    return false;
                }
                identifier_start = cursor + 1U;
            }
            cursor++;
        }
    }
    return cursor == (view_out->prerelease ? length + 1U : length);
}

static bool next_prerelease_identifier(const char *text, const semver_view_t *version,
                                       size_t *cursor, size_t *start_out,
                                       size_t *end_out, bool *numeric_out)
{
    size_t start;
    size_t end;
    bool numeric = true;

    if (*cursor >= version->length) {
        return false;
    }
    start = *cursor;
    end = start;
    while (end < version->length && text[end] != '.') {
        if (!ascii_digit(text[end])) {
            numeric = false;
        }
        end++;
    }
    *start_out = start;
    *end_out = end;
    *numeric_out = numeric;
    *cursor = end < version->length ? end + 1U : end;
    return true;
}

static int compare_bytes(const char *left, size_t left_start, size_t left_end,
                         const char *right, size_t right_start, size_t right_end)
{
    size_t index;
    size_t left_length = left_end - left_start;
    size_t right_length = right_end - right_start;
    size_t common = left_length < right_length ? left_length : right_length;

    for (index = 0U; index < common; index++) {
        if (left[left_start + index] < right[right_start + index]) {
            return -1;
        }
        if (left[left_start + index] > right[right_start + index]) {
            return 1;
        }
    }
    return left_length < right_length ? -1 : (left_length > right_length ? 1 : 0);
}

static int compare_versions(const char *left_text, const semver_view_t *left,
                            const char *right_text, const semver_view_t *right)
{
    int comparison;

    if (left->major != right->major) {
        return left->major < right->major ? -1 : 1;
    }
    if (left->minor != right->minor) {
        return left->minor < right->minor ? -1 : 1;
    }
    if (left->patch != right->patch) {
        return left->patch < right->patch ? -1 : 1;
    }
    if (left->prerelease != right->prerelease) {
        return left->prerelease ? -1 : 1;
    }
    if (!left->prerelease) {
        return 0;
    }
    {
        size_t left_cursor = left->prerelease_start;
        size_t right_cursor = right->prerelease_start;

        while (left_cursor < left->length || right_cursor < right->length) {
            size_t left_start;
            size_t left_end;
            size_t right_start;
            size_t right_end;
            bool left_numeric;
            bool right_numeric;

            if (!next_prerelease_identifier(left_text, left, &left_cursor,
                                            &left_start, &left_end, &left_numeric)) {
                return -1;
            }
            if (!next_prerelease_identifier(right_text, right, &right_cursor,
                                            &right_start, &right_end, &right_numeric)) {
                return 1;
            }
            if (left_numeric && right_numeric) {
                size_t left_length = left_end - left_start;
                size_t right_length = right_end - right_start;

                if (left_length != right_length) {
                    return left_length < right_length ? -1 : 1;
                }
                comparison = compare_bytes(left_text, left_start, left_end,
                                            right_text, right_start, right_end);
                if (comparison != 0) {
                    return comparison;
                }
            } else if (left_numeric != right_numeric) {
                return left_numeric ? -1 : 1;
            } else {
                comparison = compare_bytes(left_text, left_start, left_end,
                                            right_text, right_start, right_end);
                if (comparison != 0) {
                    return comparison;
                }
            }
            if (left_cursor >= left->length && right_cursor < right->length) {
                return -1;
            }
            if (right_cursor >= right->length && left_cursor < left->length) {
                return 1;
            }
        }
    }
    return 0;
}

static bool valid_sha256(const char *text, size_t capacity)
{
    size_t length;
    size_t index;

    if (!bounded_text(text, capacity, RUNTIME_OTA_SHA256_CAPACITY, &length) ||
        length != 64U) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        if (!ascii_hex_lower(text[index])) {
            return false;
        }
    }
    return true;
}

runtime_ota_parse_result_t runtime_ota_manifest_parse(
    const uint8_t *json, size_t length, runtime_ota_manifest_t *manifest_out)
{
    runtime_ota_manifest_t temporary;
    size_t cursor = 0U;
    char key[RUNTIME_OTA_MANIFEST_MAX_BYTES + 1U];
    bool key_overflow;
    bool unknown = false;
    bool duplicate = false;
    bool seen_version = false;
    bool seen_url = false;
    bool seen_sha256 = false;
    bool seen_minimum = false;
    bool overflow_version = false;
    bool overflow_url = false;
    bool overflow_sha256 = false;
    bool overflow_minimum = false;
    semver_view_t target_view;
    semver_view_t minimum_view;

    if (manifest_out != NULL) {
        memset(manifest_out, 0, sizeof(*manifest_out));
    }
    if (json == NULL || manifest_out == NULL) {
        return RUNTIME_OTA_PARSE_NULL_ARGUMENT;
    }
    if (length > RUNTIME_OTA_MANIFEST_MAX_BYTES) {
        return RUNTIME_OTA_PARSE_BODY_TOO_LARGE;
    }
    memset(&temporary, 0, sizeof(temporary));
    if (length == 0U) {
        return RUNTIME_OTA_PARSE_INVALID_JSON;
    }
    skip_json_space(json, length, &cursor);
    if (cursor >= length || json[cursor++] != (uint8_t)'{') {
        return RUNTIME_OTA_PARSE_INVALID_JSON;
    }
    skip_json_space(json, length, &cursor);
    if (cursor < length && json[cursor] != (uint8_t)'}') {
        for (;;) {
            char *value_output = NULL;
            size_t value_capacity = 0U;
            bool *value_overflow = NULL;

            memset(key, 0, sizeof(key));
            key_overflow = false;
            if (!parse_json_string(json, length, &cursor, key, sizeof(key),
                                   &key_overflow) || key_overflow) {
                return RUNTIME_OTA_PARSE_INVALID_JSON;
            }
            skip_json_space(json, length, &cursor);
            if (cursor >= length || json[cursor++] != (uint8_t)':') {
                return RUNTIME_OTA_PARSE_INVALID_JSON;
            }
            skip_json_space(json, length, &cursor);
            if (key_equals(key, "version", 7U)) {
                if (seen_version) {
                    duplicate = true;
                } else {
                    seen_version = true;
                    value_output = temporary.version;
                    value_capacity = sizeof(temporary.version);
                    value_overflow = &overflow_version;
                }
            } else if (key_equals(key, "url", 3U)) {
                if (seen_url) {
                    duplicate = true;
                } else {
                    seen_url = true;
                    value_output = temporary.url;
                    value_capacity = sizeof(temporary.url);
                    value_overflow = &overflow_url;
                }
            } else if (key_equals(key, "sha256", 6U)) {
                if (seen_sha256) {
                    duplicate = true;
                } else {
                    seen_sha256 = true;
                    value_output = temporary.sha256;
                    value_capacity = sizeof(temporary.sha256);
                    value_overflow = &overflow_sha256;
                }
            } else if (key_equals(key, "min_prev_version", 16U)) {
                if (seen_minimum) {
                    duplicate = true;
                } else {
                    seen_minimum = true;
                    value_output = temporary.min_prev_version;
                    value_capacity = sizeof(temporary.min_prev_version);
                    value_overflow = &overflow_minimum;
                }
            } else {
                unknown = true;
            }
            if (!parse_json_string(json, length, &cursor, value_output, value_capacity,
                                   value_overflow)) {
                return RUNTIME_OTA_PARSE_INVALID_JSON;
            }
            skip_json_space(json, length, &cursor);
            if (cursor >= length) {
                return RUNTIME_OTA_PARSE_INVALID_JSON;
            }
            if (json[cursor] == (uint8_t)',') {
                cursor++;
                skip_json_space(json, length, &cursor);
                continue;
            }
            if (json[cursor] == (uint8_t)'}') {
                cursor++;
                break;
            }
            return RUNTIME_OTA_PARSE_INVALID_JSON;
        }
    } else if (cursor >= length || json[cursor] != (uint8_t)'}') {
        return RUNTIME_OTA_PARSE_INVALID_JSON;
    } else {
        cursor++;
    }
    skip_json_space(json, length, &cursor);
    if (cursor != length) {
        return RUNTIME_OTA_PARSE_INVALID_JSON;
    }
    if (unknown) {
        return RUNTIME_OTA_PARSE_UNKNOWN_FIELD;
    }
    if (duplicate) {
        return RUNTIME_OTA_PARSE_DUPLICATE_FIELD;
    }
    if (!seen_version || !seen_url || !seen_sha256 || !seen_minimum) {
        return RUNTIME_OTA_PARSE_MISSING_FIELD;
    }
    if (overflow_version || !parse_version(temporary.version, sizeof(temporary.version),
                                           &target_view)) {
        return RUNTIME_OTA_PARSE_INVALID_VERSION;
    }
    if (overflow_url || !parse_https_reference(temporary.url, sizeof(temporary.url),
                                               true, NULL)) {
        return RUNTIME_OTA_PARSE_INVALID_URL;
    }
    if (overflow_sha256 || !valid_sha256(temporary.sha256, sizeof(temporary.sha256))) {
        return RUNTIME_OTA_PARSE_INVALID_SHA256;
    }
    if (overflow_minimum || !parse_version(temporary.min_prev_version,
                                            sizeof(temporary.min_prev_version),
                                            &minimum_view)) {
        return RUNTIME_OTA_PARSE_INVALID_MIN_PREV_VERSION;
    }
    if (compare_versions(temporary.version, &target_view,
                         temporary.min_prev_version, &minimum_view) <= 0) {
        return RUNTIME_OTA_PARSE_INVALID_VERSION_RELATION;
    }
    *manifest_out = temporary;
    return RUNTIME_OTA_PARSE_OK;
}

runtime_ota_policy_t runtime_ota_manifest_policy(
    const runtime_ota_manifest_t *manifest,
    const char *current_version, size_t current_version_capacity)
{
    semver_view_t target;
    semver_view_t minimum;
    semver_view_t current;

    if (manifest == NULL || !parse_version(manifest->version, sizeof(manifest->version),
                                           &target) ||
        !parse_https_reference(manifest->url, sizeof(manifest->url), true, NULL) ||
        !valid_sha256(manifest->sha256, sizeof(manifest->sha256)) ||
        !parse_version(manifest->min_prev_version, sizeof(manifest->min_prev_version),
                       &minimum) ||
        compare_versions(manifest->version, &target, manifest->min_prev_version,
                         &minimum) <= 0 ||
        !parse_version(current_version, current_version_capacity, &current)) {
        return RUNTIME_OTA_POLICY_INVALID;
    }
    if (compare_versions(current_version, &current, manifest->version, &target) == 0) {
        return RUNTIME_OTA_POLICY_UP_TO_DATE;
    }
    if (compare_versions(current_version, &current, manifest->version, &target) > 0) {
        return RUNTIME_OTA_POLICY_DOWNGRADE;
    }
    if (compare_versions(current_version, &current, manifest->min_prev_version,
                         &minimum) < 0) {
        return RUNTIME_OTA_POLICY_REQUIRES_INTERMEDIATE;
    }
    return RUNTIME_OTA_POLICY_UPDATE;
}

bool runtime_ota_url_same_origin(
    const char *url, size_t url_capacity,
    const char *bridge_origin, size_t bridge_origin_capacity)
{
    origin_view_t url_origin;
    origin_view_t bridge;
    size_t url_host_length;
    size_t bridge_host_length;
    size_t index;

    if (!parse_https_reference(url, url_capacity, true, &url_origin) ||
        !parse_https_reference(bridge_origin, bridge_origin_capacity, false, &bridge)) {
        return false;
    }
    url_host_length = url_origin.host_end - url_origin.host_start;
    bridge_host_length = bridge.host_end - bridge.host_start;
    if (url_host_length != bridge_host_length || url_origin.port != bridge.port) {
        return false;
    }
    for (index = 0U; index < url_host_length; index++) {
        if (url[url_origin.host_start + index] !=
            bridge_origin[bridge.host_start + index]) {
            return false;
        }
    }
    return true;
}
