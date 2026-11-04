#include "glucose_core.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GLUCOSE_FIXTURE_DIR
#define GLUCOSE_FIXTURE_DIR "contracts/fixtures"
#endif

static unsigned tests_run;
static unsigned tests_failed;
static unsigned assertions_failed;

#define EXPECT_TRUE(expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #expression); \
            assertions_failed++; \
        } \
    } while (0)

#define EXPECT_FALSE(expression) EXPECT_TRUE(!(expression))

#define EXPECT_EQ_INT(expected, actual) \
    do { \
        long long expected_value_ = (long long)(expected); \
        long long actual_value_ = (long long)(actual); \
        if (expected_value_ != actual_value_) { \
            fprintf(stderr, "%s:%d: expected %lld, got %lld: %s\n", \
                    __FILE__, __LINE__, expected_value_, actual_value_, #actual); \
            assertions_failed++; \
        } \
    } while (0)

#define EXPECT_EQ_UINT(expected, actual) \
    do { \
        unsigned long long expected_value_ = (unsigned long long)(expected); \
        unsigned long long actual_value_ = (unsigned long long)(actual); \
        if (expected_value_ != actual_value_) { \
            fprintf(stderr, "%s:%d: expected %llu, got %llu: %s\n", \
                    __FILE__, __LINE__, expected_value_, actual_value_, #actual); \
            assertions_failed++; \
        } \
    } while (0)

#define EXPECT_STREQ(expected, actual) \
    do { \
        const char *expected_value_ = (expected); \
        const char *actual_value_ = (actual); \
        if (actual_value_ == NULL || strcmp(expected_value_, actual_value_) != 0) { \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\": %s\n", \
                    __FILE__, __LINE__, expected_value_, \
                    actual_value_ == NULL ? "(null)" : actual_value_, #actual); \
            assertions_failed++; \
        } \
    } while (0)

#define EXPECT_NEAR(expected, actual, tolerance) \
    do { \
        double expected_value_ = (double)(expected); \
        double actual_value_ = (double)(actual); \
        if (!isfinite(actual_value_) || fabs(expected_value_ - actual_value_) > (tolerance)) { \
            fprintf(stderr, "%s:%d: expected %.9g +/- %.9g, got %.9g: %s\n", \
                    __FILE__, __LINE__, expected_value_, (double)(tolerance), \
                    actual_value_, #actual); \
            assertions_failed++; \
        } \
    } while (0)

static const char valid_latest_json[] =
    "{"
    "\"glucose\":112,"
    "\"unit\":\"mg/dL\","
    "\"trend\":\"stable\","
    "\"range\":\"in_range\","
    "\"target_low\":70,"
    "\"target_high\":180,"
    "\"timestamp\":\"2026-08-22T09:15:00Z\","
    "\"libre_local_time\":\"11:15:00\","
    "\"age_s\":47,"
    "\"server_time\":\"2026-08-22T09:15:47Z\","
    "\"source\":\"mock\","
    "\"poll_after_s\":45,"
    "\"schema\":2"
    "}";

static const glucose_freshness_config_t default_freshness = {
    GLUCOSE_DEFAULT_FRESH_MAX_S,
    GLUCOSE_DEFAULT_AGING_MAX_S,
    GLUCOSE_DEFAULT_STALE_MAX_S
};

static char *read_fixture(const char *name, size_t *length_out)
{
    char path[512];
    FILE *file;
    long length;
    char *buffer;
    size_t bytes_read;

    if (snprintf(path, sizeof(path), "%s/%s", GLUCOSE_FIXTURE_DIR, name) < 0) {
        return NULL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "could not open fixture: %s\n", path);
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)length + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    bytes_read = fread(buffer, 1U, (size_t)length, file);
    fclose(file);
    if (bytes_read != (size_t)length) {
        free(buffer);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    *length_out = bytes_read;
    return buffer;
}

static char *replace_once(const char *source, const char *needle, const char *replacement)
{
    const char *match = strstr(source, needle);
    size_t prefix_length;
    size_t needle_length;
    size_t replacement_length;
    size_t suffix_length;
    char *result;

    if (match == NULL) {
        fprintf(stderr, "test setup could not find replacement needle: %s\n", needle);
        return NULL;
    }
    prefix_length = (size_t)(match - source);
    needle_length = strlen(needle);
    replacement_length = strlen(replacement);
    suffix_length = strlen(match + needle_length);
    result = (char *)malloc(prefix_length + replacement_length + suffix_length + 1U);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, source, prefix_length);
    memcpy(result + prefix_length, replacement, replacement_length);
    memcpy(result + prefix_length + replacement_length,
           match + needle_length,
           suffix_length + 1U);
    return result;
}

static void run_test(const char *name, void (*test_function)(void))
{
    unsigned failures_before = assertions_failed;
    tests_run++;
    test_function();
    if (assertions_failed == failures_before) {
        printf("PASS %s\n", name);
    } else {
        tests_failed++;
        printf("FAIL %s\n", name);
    }
}

static void test_parses_all_success_contract_fixtures(void)
{
    static const char *const fixture_names[] = {
        "fresh.json", "aging.json", "stale.json"
    };
    static const uint32_t expected_ages[] = {47U, 367U, 1200U};
    static const uint32_t expected_local_seconds[] = {40500U, 40200U, 39600U};
    static const char *const expected_timestamps[] = {
        "2026-08-22T09:15:00Z",
        "2026-08-22T09:10:00Z",
        "2026-08-22T09:00:00Z"
    };
    size_t index;

    for (index = 0U; index < sizeof(fixture_names) / sizeof(fixture_names[0]); index++) {
        size_t length = 0U;
        char *json = read_fixture(fixture_names[index], &length);
        glucose_reading_t reading;
        glucose_parse_status_t status;

        EXPECT_TRUE(json != NULL);
        if (json == NULL) {
            continue;
        }
        memset(&reading, 0, sizeof(reading));
        status = glucose_parse_latest_json(json, length, &reading);
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK, status);
        EXPECT_NEAR(112.0, reading.value_mg_dl, 0.000001);
        EXPECT_EQ_INT(GLUCOSE_UNIT_MG_DL, reading.unit);
        EXPECT_EQ_INT(GLUCOSE_TREND_STABLE, reading.trend);
        EXPECT_EQ_INT(GLUCOSE_RANGE_IN_RANGE, reading.range);
        EXPECT_TRUE(reading.has_target_range);
        EXPECT_NEAR(70.0, reading.target_low_mg_dl, 0.000001);
        EXPECT_NEAR(180.0, reading.target_high_mg_dl, 0.000001);
        EXPECT_EQ_UINT(expected_ages[index], reading.age_s);
        EXPECT_EQ_UINT(expected_local_seconds[index], reading.libre_local_seconds);
        EXPECT_EQ_UINT(index == 0U ? 45U : 60U, reading.poll_after_s);
        EXPECT_EQ_INT(GLUCOSE_SOURCE_MOCK, reading.source);
        EXPECT_STREQ(expected_timestamps[index], reading.timestamp);
        free(json);
    }
}

static void test_parses_mmol_and_null_target_contract_values(void)
{
    static const char mmol_json[] =
        "{\"glucose\":6.2,\"unit\":\"mmol/L\",\"trend\":\"rising\","
        "\"range\":\"in_range\",\"target_low\":3.9,\"target_high\":10,"
        "\"timestamp\":\"2026-08-22T09:15:00.123Z\","
        "\"libre_local_time\":\"06:30:00\",\"age_s\":47,"
        "\"server_time\":\"2026-08-22T09:15:47.999Z\",\"source\":\"librelinkup\","
        "\"poll_after_s\":20,\"schema\":2}";
    static const char unknown_range_json[] =
        "{\"glucose\":112,\"unit\":\"mg/dL\",\"trend\":\"unknown\","
        "\"range\":\"unknown\",\"target_low\":null,\"target_high\":null,"
        "\"timestamp\":\"2026-08-22T09:15:00Z\","
        "\"libre_local_time\":\"11:15:00\",\"age_s\":47,"
        "\"server_time\":\"2026-08-22T09:15:47Z\",\"source\":\"mock\","
        "\"poll_after_s\":300,\"schema\":2}";
    glucose_reading_t reading;

    EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                  glucose_parse_latest_json(mmol_json, strlen(mmol_json), &reading));
    EXPECT_EQ_INT(GLUCOSE_UNIT_MMOL_L, reading.unit);
    EXPECT_NEAR(111.6, reading.value_mg_dl, 0.000001);
    EXPECT_NEAR(70.2, reading.target_low_mg_dl, 0.000001);
    EXPECT_NEAR(180.0, reading.target_high_mg_dl, 0.000001);
    EXPECT_EQ_INT(GLUCOSE_SOURCE_LIBRELINKUP, reading.source);
    EXPECT_EQ_UINT(23400U, reading.libre_local_seconds);

    EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                  glucose_parse_latest_json(unknown_range_json,
                                            strlen(unknown_range_json), &reading));
    EXPECT_FALSE(reading.has_target_range);
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, reading.range);
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, reading.trend);
}

static void test_rejects_malformed_latest_payloads(void)
{
    struct malformed_case {
        const char *needle;
        const char *replacement;
    };
    static const struct malformed_case cases[] = {
        {"\"glucose\":112,", ""},
        {"\"glucose\":112", "\"glucose\":null"},
        {"\"glucose\":112", "\"glucose\":\"112\""},
        {"\"glucose\":112", "\"glucose\":19"},
        {"\"glucose\":112", "\"glucose\":501"},
        {"\"glucose\":112", "\"glucose\":112.5"},
        {"\"glucose\":112", "\"glucose\":1e999"},
        {"\"unit\":\"mg/dL\"", "\"unit\":\"g/L\""},
        {"\"trend\":\"stable\"", "\"trend\":\"up-ish\""},
        {"\"range\":\"in_range\"", "\"range\":\"normal\""},
        {"\"target_low\":70", "\"target_low\":null"},
        {"\"target_low\":70,\"target_high\":180", "\"target_low\":190,\"target_high\":180"},
        {"2026-08-22T09:15:00Z", "2026-02-30T09:15:00Z"},
        {"\"libre_local_time\":\"11:15:00\",", ""},
        {"\"libre_local_time\":\"11:15:00\"", "\"libre_local_time\":\"24:00:00\""},
        {"\"libre_local_time\":\"11:15:00\"", "\"libre_local_time\":\"11:60:00\""},
        {"\"libre_local_time\":\"11:15:00\"", "\"libre_local_time\":\"11:15\""},
        {"\"libre_local_time\":\"11:15:00\"", "\"libre_local_time\":111500"},
        {"\"age_s\":47", "\"age_s\":-1"},
        {"\"age_s\":47", "\"age_s\":47.5"},
        {"2026-08-22T09:15:47Z", "2026-08-22 09:15:47Z"},
        {"\"source\":\"mock\"", "\"source\":\"cache\""},
        {"\"poll_after_s\":45", "\"poll_after_s\":19"},
        {"\"poll_after_s\":45", "\"poll_after_s\":301"},
        {"\"poll_after_s\":45", "\"poll_after_s\":45.5"},
        {"\"schema\":2", "\"schema\":1"}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        char *json = replace_once(valid_latest_json, cases[index].needle, cases[index].replacement);
        glucose_reading_t reading;
        glucose_parse_status_t status;

        EXPECT_TRUE(json != NULL);
        if (json == NULL) {
            continue;
        }
        memset(&reading, 0xA5, sizeof(reading));
        status = glucose_parse_latest_json(json, strlen(json), &reading);
        EXPECT_TRUE(status != GLUCOSE_PARSE_OK);
        EXPECT_NEAR(0.0, reading.value_mg_dl, 0.0);
        EXPECT_EQ_UINT(0U, reading.age_s);
        free(json);
    }
}

static void test_rejects_time_mismatch_unknown_duplicate_and_invalid_json(void)
{
    char *time_mismatch = replace_once(valid_latest_json, "\"age_s\":47", "\"age_s\":0");
    char *unknown = replace_once(valid_latest_json, "\"schema\":2", "\"schema\":2,\"extra\":true");
    char *duplicate = replace_once(valid_latest_json, "\"schema\":2", "\"schema\":2,\"glucose\":112");
    char *future = replace_once(valid_latest_json,
                                "2026-08-22T09:15:00Z",
                                "2026-08-22T09:18:00Z");
    char trailing[sizeof(valid_latest_json) + 9U];
    char trailing_comma[sizeof(valid_latest_json) + 2U];
    glucose_reading_t reading;

    EXPECT_TRUE(time_mismatch != NULL);
    EXPECT_TRUE(unknown != NULL);
    EXPECT_TRUE(duplicate != NULL);
    EXPECT_TRUE(future != NULL);
    if (time_mismatch != NULL) {
        EXPECT_EQ_INT(GLUCOSE_PARSE_TIME_MISMATCH,
                      glucose_parse_latest_json(time_mismatch, strlen(time_mismatch), &reading));
    }
    if (unknown != NULL) {
        EXPECT_EQ_INT(GLUCOSE_PARSE_UNKNOWN_FIELD,
                      glucose_parse_latest_json(unknown, strlen(unknown), &reading));
    }
    if (duplicate != NULL) {
        EXPECT_EQ_INT(GLUCOSE_PARSE_DUPLICATE_FIELD,
                      glucose_parse_latest_json(duplicate, strlen(duplicate), &reading));
    }
    if (future != NULL) {
        EXPECT_EQ_INT(GLUCOSE_PARSE_TIME_MISMATCH,
                      glucose_parse_latest_json(future, strlen(future), &reading));
    }

    (void)snprintf(trailing, sizeof(trailing), "%s trailing", valid_latest_json);
    EXPECT_EQ_INT(GLUCOSE_PARSE_INVALID_JSON,
                  glucose_parse_latest_json(trailing, strlen(trailing), &reading));
    memcpy(trailing_comma, valid_latest_json, strlen(valid_latest_json) - 1U);
    trailing_comma[strlen(valid_latest_json) - 1U] = ',';
    trailing_comma[strlen(valid_latest_json)] = '}';
    trailing_comma[strlen(valid_latest_json) + 1U] = '\0';
    EXPECT_EQ_INT(GLUCOSE_PARSE_INVALID_JSON,
                  glucose_parse_latest_json(trailing_comma, strlen(trailing_comma), &reading));
    EXPECT_EQ_INT(GLUCOSE_PARSE_INVALID_JSON,
                  glucose_parse_latest_json("[]", 2U, &reading));
    EXPECT_EQ_INT(GLUCOSE_PARSE_INVALID_JSON,
                  glucose_parse_latest_json("{\"glucose\":", 11U, &reading));
    EXPECT_EQ_INT(GLUCOSE_PARSE_NULL_ARGUMENT,
                  glucose_parse_latest_json(NULL, 0U, &reading));
    EXPECT_EQ_INT(GLUCOSE_PARSE_NULL_ARGUMENT,
                  glucose_parse_latest_json(valid_latest_json,
                                            strlen(valid_latest_json), NULL));

    free(time_mismatch);
    free(unknown);
    free(duplicate);
    free(future);
}

static void test_json_body_limits_are_enforced_transactionally(void)
{
    static const char valid_error_json[] =
        "{\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":1}";
    char *latest = (char *)malloc(GLUCOSE_LATEST_JSON_MAX_BYTES + 1U);
    char *error = (char *)malloc(GLUCOSE_ERROR_JSON_MAX_BYTES + 1U);
    glucose_reading_t reading;
    glucose_error_payload_t payload;

    EXPECT_TRUE(latest != NULL);
    EXPECT_TRUE(error != NULL);
    if (latest != NULL) {
        memset(latest, ' ', GLUCOSE_LATEST_JSON_MAX_BYTES + 1U);
        memcpy(latest, valid_latest_json, strlen(valid_latest_json));
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                      glucose_parse_latest_json(latest, GLUCOSE_LATEST_JSON_MAX_BYTES,
                                                &reading));
        memset(&reading, 0xA5, sizeof(reading));
        EXPECT_TRUE(glucose_parse_latest_json(latest,
                                              GLUCOSE_LATEST_JSON_MAX_BYTES + 1U,
                                              &reading) != GLUCOSE_PARSE_OK);
        EXPECT_NEAR(0.0, reading.value_mg_dl, 0.0);
    }
    if (error != NULL) {
        memset(error, ' ', GLUCOSE_ERROR_JSON_MAX_BYTES + 1U);
        memcpy(error, valid_error_json, strlen(valid_error_json));
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                      glucose_parse_error_json(error, GLUCOSE_ERROR_JSON_MAX_BYTES,
                                               &payload));
        memset(&payload, 0xA5, sizeof(payload));
        EXPECT_TRUE(glucose_parse_error_json(error,
                                             GLUCOSE_ERROR_JSON_MAX_BYTES + 1U,
                                             &payload) != GLUCOSE_PARSE_OK);
        EXPECT_EQ_UINT(0U, payload.retry_after_s);
    }
    free(latest);
    free(error);
}

static void test_error_contract_fixtures_parse_and_map_to_safe_states(void)
{
    struct error_case {
        const char *fixture;
        unsigned status;
        glucose_error_code_t code;
        glucose_display_state_t state;
        uint32_t retry_after_s;
    };
    static const struct error_case cases[] = {
        {"no-data.json", 503U, GLUCOSE_ERROR_UPSTREAM_UNAVAILABLE, GLUCOSE_STATE_NO_DATA, 30U},
        {"auth-error.json", 401U, GLUCOSE_ERROR_INVALID_TOKEN, GLUCOSE_STATE_AUTH_ERROR, 0U},
        {"server-error.json", 500U, GLUCOSE_ERROR_INTERNAL, GLUCOSE_STATE_SRV_ERROR, 30U}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        size_t length = 0U;
        char *json = read_fixture(cases[index].fixture, &length);
        glucose_error_payload_t payload;
        glucose_core_t core;
        glucose_display_model_t display;
        glucose_parse_status_t parse_status = GLUCOSE_PARSE_OK;

        EXPECT_TRUE(json != NULL);
        if (json == NULL) {
            continue;
        }
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                      glucose_parse_error_json(json, length, &payload));
        EXPECT_EQ_INT(cases[index].code, payload.code);
        EXPECT_EQ_UINT(cases[index].retry_after_s, payload.retry_after_s);

        EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
        EXPECT_EQ_INT(GLUCOSE_INGEST_ERROR_PAYLOAD,
                      glucose_core_ingest_http(&core, cases[index].status,
                                               json, length, 100U, &parse_status));
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK, parse_status);
        EXPECT_EQ_INT(cases[index].state, core.state);
        EXPECT_FALSE(core.has_reading);
        EXPECT_EQ_UINT(cases[index].retry_after_s, core.retry_after_s);
        EXPECT_TRUE(glucose_core_build_display(&core, &display));
        EXPECT_FALSE(display.show_number);
        EXPECT_FALSE(display.use_range_hue);
        EXPECT_STREQ("--", display.value_text);
        free(json);
    }
}

static void test_error_parser_accepts_all_codes_and_rejects_malformed_payloads(void)
{
    static const char *const code_names[] = {
        "invalid_token", "revoked_token", "rate_limited", "upstream_unavailable",
        "upstream_auth_failed", "internal", "malformed_upstream", "terms_of_use_required",
        "invalid_payload", "ota_unavailable"
    };
    size_t index;
    glucose_error_payload_t payload;

    for (index = 0U; index < sizeof(code_names) / sizeof(code_names[0]); index++) {
        char json[384];
        int written = snprintf(json, sizeof(json),
                               "{\"error\":\"%s\",\"detail\":\"valid detail\",\"retry_after_s\":30}",
                               code_names[index]);
        EXPECT_TRUE(written > 0 && (size_t)written < sizeof(json));
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK,
                      glucose_parse_error_json(json, (size_t)written, &payload));
        EXPECT_STREQ(code_names[index], glucose_error_code_name(payload.code));
    }

    {
        static const char *const invalid_json[] = {
            "{\"detail\":\"x\",\"retry_after_s\":1}",
            "{\"error\":\"other\",\"detail\":\"x\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":1,\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":-1}",
            "{\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":3601}",
            "{\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":1,\"extra\":0}",
            "{\"error\":\"internal\",\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"x\",\"retry_after_s\":1,}"
        };
        for (index = 0U; index < sizeof(invalid_json) / sizeof(invalid_json[0]); index++) {
            EXPECT_TRUE(glucose_parse_error_json(invalid_json[index], strlen(invalid_json[index]),
                                                 &payload) != GLUCOSE_PARSE_OK);
        }
    }

    {
        char detail[242];
        char json[512];
        memset(detail, 'x', 241U);
        detail[241] = '\0';
        (void)snprintf(json, sizeof(json),
                       "{\"error\":\"internal\",\"detail\":\"%s\",\"retry_after_s\":1}",
                       detail);
        EXPECT_TRUE(glucose_parse_error_json(json, strlen(json), &payload) != GLUCOSE_PARSE_OK);
    }

    {
        static const char *const invalid_utf8[] = {
            "{\"error\":\"internal\",\"detail\":\"\xC3\x28\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\xC0\xAF\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\x80\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\xF4\x90\x80\x80\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\\uD800\",\"retry_after_s\":1}",
            "{\"error\":\"internal\",\"detail\":\"\\uDC00\",\"retry_after_s\":1}"
        };
        for (index = 0U; index < sizeof(invalid_utf8) / sizeof(invalid_utf8[0]); index++) {
            EXPECT_TRUE(glucose_parse_error_json(invalid_utf8[index], strlen(invalid_utf8[index]),
                                                 &payload) != GLUCOSE_PARSE_OK);
        }
    }
}

static void test_trend_mapping_is_complete_and_never_guesses(void)
{
    EXPECT_EQ_INT(GLUCOSE_TREND_FALLING, glucose_trend_from_libre_arrow(1));
    EXPECT_EQ_INT(GLUCOSE_TREND_FALLING_SLIGHTLY, glucose_trend_from_libre_arrow(2));
    EXPECT_EQ_INT(GLUCOSE_TREND_STABLE, glucose_trend_from_libre_arrow(3));
    EXPECT_EQ_INT(GLUCOSE_TREND_RISING_SLIGHTLY, glucose_trend_from_libre_arrow(4));
    EXPECT_EQ_INT(GLUCOSE_TREND_RISING, glucose_trend_from_libre_arrow(5));
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, glucose_trend_from_libre_arrow(6));
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, glucose_trend_from_libre_arrow(7));
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, glucose_trend_from_libre_arrow(-1));
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, glucose_trend_from_libre_arrow(999));
    EXPECT_STREQ("rising_fast", glucose_trend_name(GLUCOSE_TREND_RISING_FAST));
    EXPECT_STREQ("falling_fast", glucose_trend_name(GLUCOSE_TREND_FALLING_FAST));
    EXPECT_STREQ("unknown", glucose_trend_name((glucose_trend_t)999));
}

static void test_range_classification_uses_explicit_thresholds(void)
{
    glucose_range_t range = GLUCOSE_RANGE_HIGH;

    EXPECT_TRUE(glucose_range_classify_mg_dl(70.0, 70.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_URGENT_LOW, range);
    EXPECT_TRUE(glucose_range_classify_mg_dl(70.1, 70.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_LOW, range);
    EXPECT_TRUE(glucose_range_classify_mg_dl(80.0, 70.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_IN_RANGE, range);
    EXPECT_TRUE(glucose_range_classify_mg_dl(180.0, 70.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_IN_RANGE, range);
    EXPECT_TRUE(glucose_range_classify_mg_dl(180.1, 70.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_HIGH, range);

    range = GLUCOSE_RANGE_HIGH;
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, NAN, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, range);
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, 70.0, 190.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, range);
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, 90.0, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, range);
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, 39.9, 80.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, range);
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, 100.1, 110.0, 180.0, &range));
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, range);
    EXPECT_FALSE(glucose_range_classify_mg_dl(112.0, 70.0, 80.0, 180.0, NULL));
}

static void test_freshness_boundaries_and_invalid_configuration_fail_closed(void)
{
    glucose_display_state_t state = GLUCOSE_STATE_BOOT;
    glucose_freshness_config_t invalid_order = {900U, 360U, 3600U};
    glucose_freshness_config_t invalid_stale_order = {360U, 900U, 800U};
    glucose_freshness_config_t zero = {0U, 0U, 0U};

    EXPECT_TRUE(glucose_freshness_config_is_valid(&default_freshness));
    EXPECT_TRUE(glucose_freshness_classify(0U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, state);
    EXPECT_TRUE(glucose_freshness_classify(360U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, state);
    EXPECT_TRUE(glucose_freshness_classify(361U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_AGING, state);
    EXPECT_TRUE(glucose_freshness_classify(900U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_AGING, state);
    EXPECT_TRUE(glucose_freshness_classify(901U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_STALE, state);
    EXPECT_TRUE(glucose_freshness_classify(3600U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_STALE, state);
    EXPECT_TRUE(glucose_freshness_classify(3601U, &default_freshness, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_NO_DATA, state);

    EXPECT_FALSE(glucose_freshness_config_is_valid(&invalid_order));
    EXPECT_FALSE(glucose_freshness_config_is_valid(&invalid_stale_order));
    EXPECT_FALSE(glucose_freshness_config_is_valid(&zero));
    EXPECT_FALSE(glucose_freshness_config_is_valid(NULL));
    state = GLUCOSE_STATE_FRESH;
    EXPECT_FALSE(glucose_freshness_classify(10U, &invalid_order, &state));
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, state);
    EXPECT_FALSE(glucose_freshness_classify(10U, &default_freshness, NULL));
}

static void test_core_tracks_age_monotonically_and_crosses_every_boundary(void)
{
    glucose_core_t core;
    glucose_parse_status_t status;

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &status));
    EXPECT_EQ_INT(GLUCOSE_PARSE_OK, status);
    EXPECT_EQ_UINT(47U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, core.state);

    glucose_core_tick(&core, 110U);
    EXPECT_EQ_UINT(57U, core.current_age_s);
    glucose_core_tick(&core, 90U);
    EXPECT_EQ_UINT(57U, core.current_age_s);
    glucose_core_tick(&core, 111U);
    EXPECT_EQ_UINT(58U, core.current_age_s);

    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 1000U, &status));
    glucose_core_tick(&core, 1313U);
    EXPECT_EQ_UINT(360U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, core.state);
    glucose_core_tick(&core, 1314U);
    EXPECT_EQ_UINT(361U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_AGING, core.state);
    glucose_core_tick(&core, 1853U);
    EXPECT_EQ_UINT(900U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_AGING, core.state);
    glucose_core_tick(&core, 1854U);
    EXPECT_EQ_UINT(901U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_STALE, core.state);
    glucose_core_tick(&core, 4553U);
    EXPECT_EQ_UINT(3600U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_STALE, core.state);
    glucose_core_tick(&core, 4554U);
    EXPECT_EQ_UINT(3601U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_NO_DATA, core.state);
    EXPECT_TRUE(core.has_reading);
    glucose_core_tick(&core, 4555U);
    EXPECT_EQ_UINT(3602U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_NO_DATA, core.state);
}

static void test_core_handles_304_and_operational_states_without_leaking_numbers(void)
{
    glucose_core_t core;
    glucose_display_model_t display;
    glucose_parse_status_t parse_status;

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(&core, 304U, NULL, 0U, 100U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
    EXPECT_FALSE(core.has_reading);

    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    EXPECT_TRUE(glucose_core_set_operational_state(&core, GLUCOSE_STATE_NO_WIFI));
    glucose_core_tick(&core, 120U);
    EXPECT_EQ_UINT(67U, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_NO_WIFI, core.state);
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_FALSE(display.show_number);
    EXPECT_FALSE(display.use_range_hue);
    EXPECT_STREQ("--", display.value_text);

    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(&core, 304U, NULL, 0U, 130U, &parse_status));
    EXPECT_FALSE(core.has_reading);

    EXPECT_TRUE(glucose_core_set_operational_state(&core, GLUCOSE_STATE_BOOT));
    EXPECT_FALSE(core.has_reading);
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_FALSE(display.show_number);
    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(&core, 304U, NULL, 0U, 140U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);

    EXPECT_FALSE(glucose_core_set_operational_state(&core, GLUCOSE_STATE_FRESH));
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
}

static void test_metadata_aware_304_preserves_monotonic_age(void)
{
    glucose_core_t core;
    glucose_parse_status_t status;

    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_not_modified(NULL, 20U, 100U));
    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_not_modified(&core, 20U, 100U));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &status));
    core.retry_after_s = 30U;
    EXPECT_EQ_INT(GLUCOSE_INGEST_NOT_MODIFIED,
                  glucose_core_ingest_not_modified(&core, 20U, 110U));
    EXPECT_EQ_UINT(47U, core.current_age_s);
    EXPECT_EQ_UINT(0U, core.retry_after_s);
    EXPECT_EQ_UINT(110U, core.last_monotonic_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, core.state);

    EXPECT_EQ_INT(GLUCOSE_INGEST_NOT_MODIFIED,
                  glucose_core_ingest_not_modified(&core, 100U, 90U));
    EXPECT_EQ_UINT(100U, core.current_age_s);
    EXPECT_EQ_UINT(90U, core.last_monotonic_s);
}

static void test_malformed_or_error_response_clears_previous_number_transactionally(void)
{
    glucose_core_t core;
    glucose_display_model_t display;
    glucose_parse_status_t parse_status;
    char *bad = replace_once(valid_latest_json, "\"glucose\":112", "\"glucose\":null");
    size_t error_length = 0U;
    char *error_json = read_fixture("server-error.json", &error_length);

    EXPECT_TRUE(bad != NULL);
    EXPECT_TRUE(error_json != NULL);
    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_TRUE(display.show_number);
    EXPECT_STREQ("112", display.value_text);

    if (bad != NULL) {
        EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                      glucose_core_ingest_http(&core, 200U, bad, strlen(bad),
                                               110U, &parse_status));
        EXPECT_TRUE(parse_status != GLUCOSE_PARSE_OK);
        EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
        EXPECT_FALSE(core.has_reading);
        EXPECT_TRUE(glucose_core_build_display(&core, &display));
        EXPECT_FALSE(display.show_number);
        EXPECT_FALSE(display.use_range_hue);
        EXPECT_STREQ("--", display.value_text);
    }

    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 120U, &parse_status));
    if (error_json != NULL) {
        EXPECT_EQ_INT(GLUCOSE_INGEST_ERROR_PAYLOAD,
                      glucose_core_ingest_http(&core, 500U, error_json, error_length,
                                               130U, &parse_status));
        EXPECT_FALSE(core.has_reading);
        EXPECT_TRUE(glucose_core_build_display(&core, &display));
        EXPECT_FALSE(display.show_number);
        EXPECT_STREQ("--", display.value_text);
    }

    free(bad);
    free(error_json);
}

static void test_large_number_and_range_hue_are_impossible_outside_fresh_or_aging(void)
{
    int state_value;

    for (state_value = 0; state_value < (int)GLUCOSE_STATE_COUNT; state_value++) {
        glucose_display_state_t state = (glucose_display_state_t)state_value;
        bool should_allow = state == GLUCOSE_STATE_FRESH || state == GLUCOSE_STATE_AGING;
        EXPECT_EQ_INT(should_allow, glucose_state_allows_number(state));
        EXPECT_EQ_INT(should_allow, glucose_state_allows_range_hue(state));
    }
    EXPECT_FALSE(glucose_state_allows_number((glucose_display_state_t)-1));
    EXPECT_FALSE(glucose_state_allows_number((glucose_display_state_t)999));
    EXPECT_FALSE(glucose_state_allows_range_hue((glucose_display_state_t)-1));
    EXPECT_FALSE(glucose_state_allows_range_hue((glucose_display_state_t)999));
}

static void test_display_model_formats_fresh_aging_and_stale_safely(void)
{
    glucose_core_t core;
    glucose_display_model_t display;
    glucose_parse_status_t parse_status;

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_EQ_INT(GLUCOSE_STATE_FRESH, display.state);
    EXPECT_TRUE(display.show_number);
    EXPECT_TRUE(display.use_range_hue);
    EXPECT_STREQ("112", display.value_text);
    EXPECT_STREQ("mg/dL", display.unit_text);
    EXPECT_STREQ("1m", display.age_text);
    EXPECT_EQ_INT(GLUCOSE_TREND_STABLE, display.trend);
    EXPECT_EQ_INT(GLUCOSE_RANGE_IN_RANGE, display.range);

    glucose_core_tick(&core, 414U);
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_EQ_INT(GLUCOSE_STATE_AGING, display.state);
    EXPECT_TRUE(display.show_number);
    EXPECT_TRUE(display.use_range_hue);

    glucose_core_tick(&core, 954U);
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_EQ_INT(GLUCOSE_STATE_STALE, display.state);
    EXPECT_FALSE(display.show_number);
    EXPECT_FALSE(display.use_range_hue);
    EXPECT_STREQ("--", display.value_text);
    EXPECT_STREQ("", display.unit_text);
    EXPECT_EQ_INT(GLUCOSE_TREND_UNKNOWN, display.trend);
    EXPECT_EQ_INT(GLUCOSE_RANGE_UNKNOWN, display.range);
    EXPECT_TRUE(strlen(display.status_text) <= GLUCOSE_STATUS_TEXT_MAX);
}

static void test_mmol_contract_boundaries_remain_displayable(void)
{
    static const double wire_values[] = {1.0, 28.0};
    static const double canonical_values[] = {18.0, 504.0};
    static const char *const expected_text[] = {"1.0", "28.0"};
    size_t index;

    for (index = 0U; index < sizeof(wire_values) / sizeof(wire_values[0]); index++) {
        char json[384];
        glucose_core_t core;
        glucose_display_model_t display;
        glucose_parse_status_t parse_status;
        int written = snprintf(
            json,
            sizeof(json),
            "{\"glucose\":%.1f,\"unit\":\"mmol/L\",\"trend\":\"stable\","
            "\"range\":\"unknown\",\"target_low\":null,\"target_high\":null,"
            "\"timestamp\":\"2026-08-22T09:15:00Z\","
            "\"libre_local_time\":\"11:15:00\",\"age_s\":47,"
            "\"server_time\":\"2026-08-22T09:15:47Z\",\"source\":\"mock\","
            "\"poll_after_s\":45,\"schema\":2}",
            wire_values[index]);

        EXPECT_TRUE(written > 0 && (size_t)written < sizeof(json));
        EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
        EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                      glucose_core_ingest_http(&core, 200U, json, (size_t)written,
                                               100U, &parse_status));
        EXPECT_EQ_INT(GLUCOSE_PARSE_OK, parse_status);
        EXPECT_NEAR(canonical_values[index], core.reading.value_mg_dl, 0.000001);
        EXPECT_TRUE(glucose_core_build_display(&core, &display));
        EXPECT_TRUE(display.show_number);
        EXPECT_STREQ(expected_text[index], display.value_text);
        EXPECT_STREQ("mmol/L", display.unit_text);
    }
}

static void test_unit_conversion_and_formatting_are_bounded(void)
{
    char buffer[GLUCOSE_VALUE_TEXT_CAPACITY];
    char tiny[2] = {'x', '\0'};
    double converted = 0.0;

    EXPECT_TRUE(glucose_convert_from_mg_dl(112.0, GLUCOSE_UNIT_MG_DL, &converted));
    EXPECT_NEAR(112.0, converted, 0.000001);
    EXPECT_TRUE(glucose_convert_from_mg_dl(112.0, GLUCOSE_UNIT_MMOL_L, &converted));
    EXPECT_NEAR(112.0 / 18.0, converted, 0.000001);
    EXPECT_TRUE(glucose_format_value(112.0, GLUCOSE_UNIT_MG_DL, buffer, sizeof(buffer)));
    EXPECT_STREQ("112", buffer);
    EXPECT_TRUE(glucose_format_value(112.0, GLUCOSE_UNIT_MMOL_L, buffer, sizeof(buffer)));
    EXPECT_STREQ("6.2", buffer);
    EXPECT_TRUE(glucose_format_value(180.0, GLUCOSE_UNIT_MMOL_L, buffer, sizeof(buffer)));
    EXPECT_STREQ("10.0", buffer);

    EXPECT_FALSE(glucose_format_value(19.0, GLUCOSE_UNIT_MG_DL, buffer, sizeof(buffer)));
    EXPECT_STREQ("", buffer);
    EXPECT_FALSE(glucose_format_value(NAN, GLUCOSE_UNIT_MG_DL, buffer, sizeof(buffer)));
    EXPECT_FALSE(glucose_format_value(112.0, (glucose_unit_t)99, buffer, sizeof(buffer)));
    EXPECT_FALSE(glucose_format_value(112.0, GLUCOSE_UNIT_MG_DL, tiny, sizeof(tiny)));
    EXPECT_STREQ("", tiny);
    EXPECT_FALSE(glucose_format_value(112.0, GLUCOSE_UNIT_MG_DL, NULL, 0U));
    EXPECT_FALSE(glucose_convert_from_mg_dl(112.0, GLUCOSE_UNIT_MG_DL, NULL));
}

static void test_age_formatting_never_overflows_the_display_buffer(void)
{
    char buffer[GLUCOSE_AGE_TEXT_CAPACITY];
    char tiny[2] = {'x', '\0'};

    EXPECT_TRUE(glucose_format_age(0U, buffer, sizeof(buffer)));
    EXPECT_STREQ("0m", buffer);
    EXPECT_TRUE(glucose_format_age(1U, buffer, sizeof(buffer)));
    EXPECT_STREQ("1m", buffer);
    EXPECT_TRUE(glucose_format_age(60U, buffer, sizeof(buffer)));
    EXPECT_STREQ("1m", buffer);
    EXPECT_TRUE(glucose_format_age(61U, buffer, sizeof(buffer)));
    EXPECT_STREQ("2m", buffer);
    EXPECT_TRUE(glucose_format_age(3600U, buffer, sizeof(buffer)));
    EXPECT_STREQ("60m", buffer);
    EXPECT_TRUE(glucose_format_age(UINT32_MAX, buffer, sizeof(buffer)));
    EXPECT_TRUE(strlen(buffer) < sizeof(buffer));
    EXPECT_FALSE(glucose_format_age(60U, tiny, sizeof(tiny)));
    EXPECT_STREQ("", tiny);
    EXPECT_FALSE(glucose_format_age(60U, NULL, 0U));
}

static void test_backoff_matches_plan_sequence_and_bounded_jitter(void)
{
    glucose_backoff_config_t config = {20U, 60U, 900U, 10U};
    static const uint32_t expected[] = {60U, 60U, 120U, 240U, 480U, 900U, 900U};
    size_t index;

    EXPECT_TRUE(glucose_backoff_config_is_valid(&config));
    for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++) {
        uint32_t delay = 0U;
        EXPECT_TRUE(glucose_backoff_delay_s(&config, (uint32_t)index,
                                            GLUCOSE_JITTER_SAMPLE_MIDPOINT, &delay));
        EXPECT_EQ_UINT(expected[index], delay);
    }

    {
        uint32_t delay = 0U;
        EXPECT_TRUE(glucose_backoff_delay_s(&config, 0U, 0U, &delay));
        EXPECT_EQ_UINT(50U, delay);
        EXPECT_TRUE(glucose_backoff_delay_s(&config, 0U,
                                            GLUCOSE_JITTER_SAMPLE_MAX, &delay));
        EXPECT_EQ_UINT(70U, delay);
        EXPECT_TRUE(glucose_backoff_delay_s(&config, UINT32_MAX,
                                            GLUCOSE_JITTER_SAMPLE_MIDPOINT, &delay));
        EXPECT_EQ_UINT(900U, delay);
    }

    {
        glucose_backoff_config_t floor_config = {20U, 20U, 900U, 10U};
        uint32_t delay = 0U;
        EXPECT_TRUE(glucose_backoff_delay_s(&floor_config, 0U, 0U, &delay));
        EXPECT_EQ_UINT(20U, delay);
    }
}

static void test_backoff_rejects_invalid_inputs_without_a_delay_default(void)
{
    glucose_backoff_config_t bad_order = {60U, 20U, 900U, 10U};
    glucose_backoff_config_t bad_cap = {20U, 60U, 50U, 10U};
    glucose_backoff_config_t bad_jitter = {20U, 60U, 900U, 901U};
    glucose_backoff_config_t valid = {20U, 60U, 900U, 10U};
    uint32_t delay = 123U;

    EXPECT_FALSE(glucose_backoff_config_is_valid(NULL));
    EXPECT_FALSE(glucose_backoff_config_is_valid(&bad_order));
    EXPECT_FALSE(glucose_backoff_config_is_valid(&bad_cap));
    EXPECT_FALSE(glucose_backoff_config_is_valid(&bad_jitter));
    EXPECT_FALSE(glucose_backoff_delay_s(&bad_order, 1U,
                                         GLUCOSE_JITTER_SAMPLE_MIDPOINT, &delay));
    EXPECT_EQ_UINT(0U, delay);
    delay = 123U;
    EXPECT_FALSE(glucose_backoff_delay_s(&valid, 1U,
                                         GLUCOSE_JITTER_SAMPLE_MAX + 1U, &delay));
    EXPECT_EQ_UINT(0U, delay);
    EXPECT_FALSE(glucose_backoff_delay_s(&valid, 1U,
                                         GLUCOSE_JITTER_SAMPLE_MIDPOINT, NULL));
}

static void test_core_defensive_paths_fail_closed(void)
{
    static const glucose_display_state_t operational_states[] = {
        GLUCOSE_STATE_BOOT,
        GLUCOSE_STATE_WIFI_CONNECTING,
        GLUCOSE_STATE_TIME_SYNCING,
        GLUCOSE_STATE_FETCHING,
        GLUCOSE_STATE_NO_DATA,
        GLUCOSE_STATE_AUTH_ERROR,
        GLUCOSE_STATE_SRV_ERROR,
        GLUCOSE_STATE_NO_WIFI
    };
    glucose_core_t core;
    glucose_display_model_t display;
    glucose_parse_status_t parse_status;
    size_t index;

    EXPECT_FALSE(glucose_core_init(NULL, &default_freshness));
    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, NULL));
    core.current_age_s = UINT32_MAX - 1U;
    core.last_monotonic_s = 100U;
    core.state = GLUCOSE_STATE_FRESH;
    glucose_core_tick(&core, 102U);
    EXPECT_EQ_UINT(UINT32_MAX, core.current_age_s);
    EXPECT_EQ_INT(GLUCOSE_STATE_NO_DATA, core.state);

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(&core, 304U, "{}", 2U, 101U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_PARSE_INVALID_VALUE, parse_status);
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
    EXPECT_FALSE(core.has_reading);

    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(&core, 500U, "{}", 2U, 102U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_PARSE_MISSING_FIELD, parse_status);
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
    EXPECT_FALSE(core.has_reading);

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    core.freshness.fresh_max_s = 0U;
    glucose_core_tick(&core, 101U);
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
    EXPECT_FALSE(core.has_reading);

    for (index = 0U; index < sizeof(operational_states) / sizeof(operational_states[0]); index++) {
        EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
        EXPECT_TRUE(glucose_core_set_operational_state(&core, operational_states[index]));
        EXPECT_TRUE(glucose_core_build_display(&core, &display));
        EXPECT_FALSE(display.show_number);
        EXPECT_STREQ("--", display.value_text);
        EXPECT_TRUE(strlen(display.status_text) <= GLUCOSE_STATUS_TEXT_MAX);
    }
    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_FALSE(glucose_core_set_operational_state(&core, (glucose_display_state_t)-1));
    EXPECT_FALSE(glucose_core_set_operational_state(&core, GLUCOSE_STATE_COUNT));
}

static void test_init_and_null_api_inputs_fail_closed(void)
{
    glucose_core_t core;
    glucose_freshness_config_t bad = {360U, 100U, 3600U};
    glucose_display_model_t display;
    glucose_parse_status_t parse_status = GLUCOSE_PARSE_OK;

    memset(&core, 0xA5, sizeof(core));
    EXPECT_FALSE(glucose_core_init(&core, &bad));
    EXPECT_EQ_INT(GLUCOSE_STATE_SRV_ERROR, core.state);
    EXPECT_FALSE(core.has_reading);
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_FALSE(display.show_number);

    EXPECT_EQ_INT(GLUCOSE_INGEST_MALFORMED,
                  glucose_core_ingest_http(NULL, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 0U, &parse_status));
    EXPECT_EQ_INT(GLUCOSE_PARSE_NULL_ARGUMENT, parse_status);
    EXPECT_FALSE(glucose_core_build_display(NULL, &display));
    EXPECT_FALSE(glucose_core_build_display(&core, NULL));
    glucose_core_tick(NULL, 100U);
    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    glucose_core_tick(&core, 100U);
    EXPECT_EQ_INT(GLUCOSE_STATE_BOOT, core.state);
    EXPECT_FALSE(core.has_reading);
    EXPECT_FALSE(glucose_core_set_operational_state(NULL, GLUCOSE_STATE_BOOT));

    core.state = (glucose_display_state_t)999;
    EXPECT_TRUE(glucose_core_build_display(&core, &display));
    EXPECT_FALSE(display.show_number);
    EXPECT_STREQ("SRV", display.status_text);

    EXPECT_TRUE(glucose_core_init(&core, &default_freshness));
    EXPECT_EQ_INT(GLUCOSE_INGEST_UPDATED,
                  glucose_core_ingest_http(&core, 200U, valid_latest_json,
                                           strlen(valid_latest_json), 100U, &parse_status));
    core.reading.unit = (glucose_unit_t)99;
    EXPECT_FALSE(glucose_core_build_display(&core, &display));
    EXPECT_FALSE(display.show_number);
    EXPECT_STREQ("--", display.value_text);
}

int main(void)
{
    run_test("parse all success contract fixtures", test_parses_all_success_contract_fixtures);
    run_test("parse mmol and null target values", test_parses_mmol_and_null_target_contract_values);
    run_test("reject malformed latest payloads", test_rejects_malformed_latest_payloads);
    run_test("reject time mismatch and invalid JSON", test_rejects_time_mismatch_unknown_duplicate_and_invalid_json);
    run_test("JSON body limits are transactional", test_json_body_limits_are_enforced_transactionally);
    run_test("error fixtures map to safe states", test_error_contract_fixtures_parse_and_map_to_safe_states);
    run_test("error parser validates every code", test_error_parser_accepts_all_codes_and_rejects_malformed_payloads);
    run_test("trend mapping never guesses", test_trend_mapping_is_complete_and_never_guesses);
    run_test("range classification uses explicit thresholds", test_range_classification_uses_explicit_thresholds);
    run_test("freshness boundaries fail closed", test_freshness_boundaries_and_invalid_configuration_fail_closed);
    run_test("monotonic age crosses boundaries", test_core_tracks_age_monotonically_and_crosses_every_boundary);
    run_test("304 and operational states are safe", test_core_handles_304_and_operational_states_without_leaking_numbers);
    run_test("metadata-aware 304 preserves monotonic age", test_metadata_aware_304_preserves_monotonic_age);
    run_test("malformed and error responses clear number", test_malformed_or_error_response_clears_previous_number_transactionally);
    run_test("number and hue state invariant", test_large_number_and_range_hue_are_impossible_outside_fresh_or_aging);
    run_test("display model formats safely", test_display_model_formats_fresh_aging_and_stale_safely);
    run_test("mmol contract boundaries display", test_mmol_contract_boundaries_remain_displayable);
    run_test("unit conversion and value formatting", test_unit_conversion_and_formatting_are_bounded);
    run_test("age formatting is bounded", test_age_formatting_never_overflows_the_display_buffer);
    run_test("backoff sequence and jitter", test_backoff_matches_plan_sequence_and_bounded_jitter);
    run_test("backoff invalid inputs", test_backoff_rejects_invalid_inputs_without_a_delay_default);
    run_test("core defensive paths", test_core_defensive_paths_fail_closed);
    run_test("null APIs fail closed", test_init_and_null_api_inputs_fail_closed);

    printf("\n%u tests, %u failed, %u assertion failures\n",
           tests_run, tests_failed, assertions_failed);
    return tests_failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
