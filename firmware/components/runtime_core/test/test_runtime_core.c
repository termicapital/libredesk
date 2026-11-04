#include "runtime_core.h"
#include "wifi_provisioning_core.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define TEST_OTA_MANIFEST_MAX_BYTES RUNTIME_OTA_MANIFEST_MAX_BYTES
#define TEST_OTA_VERSION_CAPACITY RUNTIME_OTA_VERSION_CAPACITY
#define TEST_OTA_URL_CAPACITY RUNTIME_OTA_URL_CAPACITY
#define TEST_OTA_SHA256_CAPACITY RUNTIME_OTA_SHA256_CAPACITY
#define TEST_OTA_PARSE_OK RUNTIME_OTA_PARSE_OK
#define TEST_OTA_PARSE_NULL_ARGUMENT RUNTIME_OTA_PARSE_NULL_ARGUMENT
#define TEST_OTA_PARSE_BODY_TOO_LARGE RUNTIME_OTA_PARSE_BODY_TOO_LARGE
#define TEST_OTA_PARSE_INVALID_JSON RUNTIME_OTA_PARSE_INVALID_JSON
#define TEST_OTA_PARSE_UNKNOWN_FIELD RUNTIME_OTA_PARSE_UNKNOWN_FIELD
#define TEST_OTA_PARSE_DUPLICATE_FIELD RUNTIME_OTA_PARSE_DUPLICATE_FIELD
#define TEST_OTA_PARSE_MISSING_FIELD RUNTIME_OTA_PARSE_MISSING_FIELD
#define TEST_OTA_PARSE_INVALID_VERSION RUNTIME_OTA_PARSE_INVALID_VERSION
#define TEST_OTA_PARSE_INVALID_URL RUNTIME_OTA_PARSE_INVALID_URL
#define TEST_OTA_PARSE_INVALID_SHA256 RUNTIME_OTA_PARSE_INVALID_SHA256
#define TEST_OTA_PARSE_INVALID_MIN_PREV_VERSION RUNTIME_OTA_PARSE_INVALID_MIN_PREV_VERSION
#define TEST_OTA_PARSE_INVALID_VERSION_RELATION RUNTIME_OTA_PARSE_INVALID_VERSION_RELATION
#define TEST_OTA_POLICY_UPDATE RUNTIME_OTA_POLICY_UPDATE
#define TEST_OTA_POLICY_UP_TO_DATE RUNTIME_OTA_POLICY_UP_TO_DATE
#define TEST_OTA_POLICY_DOWNGRADE RUNTIME_OTA_POLICY_DOWNGRADE
#define TEST_OTA_POLICY_REQUIRES_INTERMEDIATE RUNTIME_OTA_POLICY_REQUIRES_INTERMEDIATE
#define TEST_OTA_POLICY_INVALID RUNTIME_OTA_POLICY_INVALID

typedef runtime_ota_manifest_t test_ota_manifest_t;

static unsigned fails;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            fails++; \
        } \
    } while (0)
#define CHECK_EQ(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "%s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)(actual), (int)(expected)); \
            fails++; \
        } \
    } while (0)

static runtime_config_t valid_config(void)
{
    runtime_config_t config;

    memset(&config, 0, sizeof(config));
    strcpy(config.wifi_ssid, "Desk");
    strcpy(config.wifi_password, "password");
    strcpy(config.bridge_origin, "https://bridge.example.com:443");
    strcpy(config.device_token, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE");
    strcpy(config.tz, "EST5");
    config.unit = GLUCOSE_UNIT_MG_DL;
    config.brightness_day_percent = 100U;
    config.brightness_night_percent = 1U;
    config.night_start_minute = 0U;
    config.night_end_minute = 1439U;
    config.freshness.fresh_max_s = 360U;
    config.freshness.aging_max_s = 900U;
    config.freshness.stale_max_s = 3600U;
    config.poll_s = 20U;
    config.ota_enabled = 1U;
    return config;
}

static void expect_error(runtime_config_t *config, runtime_config_error_t expected)
{
    runtime_config_error_t actual = RUNTIME_CONFIG_ERROR_COUNT;

    CHECK(!runtime_config_validate(config, &actual));
    CHECK_EQ(expected, actual);
}

static void test_utf8_boundaries_and_controls(void)
{
    runtime_config_t config = valid_config();
    static const unsigned char overlong[] = {0xc0U, 0x80U, 0x00U};
    static const unsigned char truncated[] = {0xe2U, 0x82U, 0x00U};
    static const unsigned char surrogate[] = {0xedU, 0xa0U, 0x80U, 0x00U};
    static const unsigned char too_high[] = {0xf4U, 0x90U, 0x80U, 0x80U, 0x00U};
    static const unsigned char c1[] = {0xc2U, 0x80U, 0x00U};
    static const unsigned char del[] = {0x7fU, 0x00U};

    memset(config.wifi_ssid, 'a', 32U);
    config.wifi_ssid[32] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    config.wifi_ssid[0] = (char)0x80;
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, overlong, sizeof(overlong));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, truncated, sizeof(truncated));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, surrogate, sizeof(surrogate));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, too_high, sizeof(too_high));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, c1, sizeof(c1));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    memcpy(config.wifi_ssid, del, sizeof(del));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    config.wifi_ssid[0] = '\x01';
    config.wifi_ssid[1] = '\0';
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
}

static void test_string_lengths_tokens_and_passwords(void)
{
    runtime_config_t config = valid_config();
    size_t index;

    config = valid_config();
    config.wifi_ssid[0] = '\0';
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    config = valid_config();
    memset(config.wifi_ssid, 'x', sizeof(config.wifi_ssid));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);
    config = valid_config();
    config.wifi_ssid[32] = '\0';
    CHECK(runtime_config_validate(&config, NULL));

    config = valid_config();
    config.wifi_password[0] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    memset(config.wifi_password, 'a', 7U);
    config.wifi_password[7] = '\0';
    expect_error(&config, RUNTIME_CONFIG_ERROR_PASSWORD);
    config = valid_config();
    memset(config.wifi_password, 'a', 63U);
    config.wifi_password[63] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    config = valid_config();
    for (index = 0U; index < 64U; index++) {
        config.wifi_password[index] = "0123456789abcdef"[index % 16U];
    }
    config.wifi_password[64] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    config.wifi_password[63] = 'g';
    expect_error(&config, RUNTIME_CONFIG_ERROR_PASSWORD);
    config.wifi_password[64] = 'x';
    expect_error(&config, RUNTIME_CONFIG_ERROR_PASSWORD);

    config = valid_config();
    config.device_token[42] = '\0';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    memset(config.device_token, 'A', sizeof(config.device_token));
    config.device_token[42] = 'E';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    config.device_token[42] = '!';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    config.device_token[42] = 'B';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    config.device_token[43] = 'x';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    config.device_token[0] = '!';
    expect_error(&config, RUNTIME_CONFIG_ERROR_DEVICE_TOKEN);
    config = valid_config();
    memset(config.bridge_origin, 'x', sizeof(config.bridge_origin));
    expect_error(&config, RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN);
}

static void test_url_numeric_host_matrix(void)
{
    char maximum_origin[RUNTIME_BRIDGE_ORIGIN_CAPACITY];
    size_t index;

    CHECK(!runtime_url_is_https_origin(NULL, 1U));
    CHECK(!runtime_url_is_https_origin("https://a.cd", 0U));
    CHECK(!runtime_url_is_https_origin("https://a.cd", 257U));
    CHECK(runtime_url_is_https_origin("https://ab.cd", 14U));
    CHECK(runtime_url_is_https_origin("https://a.cd:1", 16U));
    CHECK(!runtime_url_is_https_origin("https://a.cd/", 14U));
    CHECK(!runtime_url_is_https_origin("https://a..cd", 14U));
    CHECK(!runtime_url_is_https_origin("https://-a.cd", 13U));
    CHECK(!runtime_url_is_https_origin("https://a.cd:00080", 19U));
    CHECK(!runtime_url_is_https_origin("https://a.cd:65536", 19U));
    CHECK(!runtime_url_is_https_origin("https://a.cd/user", 18U));
    CHECK(!runtime_url_is_https_origin("https://a.cd@x.y", 17U));
    CHECK(!runtime_url_is_https_origin("https://a.cd%40x.y", 19U));
    CHECK(!runtime_url_is_https_origin("https://a.cd\r\n", 15U));
    CHECK(!runtime_url_is_https_origin("http://a.cd", 12U));
#if defined(RUNTIME_CORE_HOST_TEST) && defined(RUNTIME_ALLOW_IP_LITERAL)
    CHECK(runtime_url_is_https_origin("https://127.0.0.1", 20U));
    CHECK(!runtime_url_is_https_origin("https://127.1", 14U));
    CHECK(!runtime_url_is_https_origin("https://1.2.3", 16U));
    CHECK(runtime_url_is_https_origin("https://192.168.1.42:443", 25U));
#else
    CHECK(!runtime_url_is_https_origin("https://127.0.0.1", 20U));
    CHECK(!runtime_url_is_https_origin("https://127.1", 14U));
    CHECK(!runtime_url_is_https_origin("https://1.2.3", 16U));
#endif
    CHECK(!runtime_url_is_https_origin("https://0x7f.0.0.1", 20U));
    CHECK(!runtime_url_is_https_origin("https://0x7f.1", 16U));
    CHECK(!runtime_url_is_https_origin("https://0177.0.0.1", 20U));
    CHECK(!runtime_url_is_https_origin("https://127.0x0.0.1", 20U));
    CHECK(!runtime_url_is_https_origin("https://0x7f.0x0.0x0.0x1", 26U));
    CHECK(runtime_url_is_https_origin("https://dead.beef", 18U));
    CHECK(runtime_url_is_https_origin("https://0x7f.example", 21U));
    CHECK(!runtime_url_is_https_origin("https://999.999.999.999", 24U));
    CHECK(!runtime_url_is_https_origin("https://01.2.3.4", 17U));
    CHECK(!runtime_url_is_https_origin("https://127..0.1", 17U));
    CHECK(!runtime_url_is_https_origin("https://[::1]", 14U));
    CHECK(runtime_url_is_https_origin("https://a.b", 12U));
    CHECK(!runtime_url_is_https_origin("https://a.b:0", 14U));
    CHECK(runtime_url_is_https_origin("https://a.b:65535", 18U));
    CHECK(!runtime_url_is_https_origin("https://a.b:+1", 15U));
    CHECK(!runtime_url_is_https_origin("https://a.b:", 13U));

    memset(maximum_origin, 'a', sizeof(maximum_origin));
    memcpy(maximum_origin, "https://", 8U);
    maximum_origin[8U + 62U] = '.';
    maximum_origin[8U + 63U + 1U + 62U] = '\0';
    CHECK(runtime_url_is_https_origin(maximum_origin, sizeof(maximum_origin)));
    for (index = 0U; index < sizeof(maximum_origin); index++) {
        if (maximum_origin[index] == '\0') {
            maximum_origin[index] = 'a';
            break;
        }
    }
    CHECK(!runtime_url_is_https_origin(maximum_origin, sizeof(maximum_origin)));
}

static void test_tz_matrix_and_cursor(void)
{
    runtime_config_t config = valid_config();
    static const char *const valid[] = {
        "UTC0", "<-03>3", "CET-1CEST,M3.5.0,M10.5.0/3",
        "CET-1CEST,M3.5.0/2:01,M10.5.0/2",
        "EST24EDT,M3.2.0/2:01,M11.1.0/2",
        "AAAAAAAAAAAAAAAA-24:00:00BBBBBBBBBBBBBBBB,M1.1.0/2:01,M1.1.0/2"
    };
    static const char *const invalid[] = {
        "EST5EDT", "EST5,M3.2.0,M11.1.0",
        "EST5EDT,M3.6.0,M11.1.0", "EST5EDT,M3.2.7,M11.1.0",
        "EST5EDT,M3.2.0/+2,M11.1.0", "EST5EDT,M03.2.0,M11.1.0",
        "EST5EDT,M3.2.0/2:1,M11.1.0", "Europe/Berlin",
        "EST5:foo,M3.2.0,M11.1.0", "EST5EDT,M3.2.0,M11.1.0,M12.1.0",
        "AB5,M3.2.0,M11.1.0", "ABCDEFGHIJKLMNOPQ5,M3.2.0,M11.1.0",
        "EST25,M3.2.0,M11.1.0", "EST5:01,M3.2.0,M11.1.0",
        "EST5EDT", "EST5,M3.2.0,M11.1.0", "EST5EDT,M0.2.0,M11.1.0",
        "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M3.0.0,M11.1.0",
        "EST5EDT,M3.2.7,M11.1.0", "EST5EDT,M3.2.0/02,M11.1.0",
        "EST5EDT,M3.2.0/-2,M11.1.0", "EST5EDT,M3.2.0/24:01,M11.1.0",
        "EST5EDT,M3.2.0/2:60,M11.1.0", "EST5EDT,M3.2.0,M11.1.0/2/3"
    };
    size_t index;

    for (index = 0U; index < sizeof(valid) / sizeof(valid[0]); index++) {
        strcpy(config.tz, valid[index]);
        CHECK(runtime_config_validate(&config, NULL));
    }
    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        strcpy(config.tz, invalid[index]);
        expect_error(&config, RUNTIME_CONFIG_ERROR_TZ);
    }
    config = valid_config();
    memset(config.tz, 'x', sizeof(config.tz));
    expect_error(&config, RUNTIME_CONFIG_ERROR_TZ);
}

static void test_precedence_numeric_ota_and_clamp(void)
{
    runtime_config_t config = valid_config();
    runtime_config_error_t error = RUNTIME_CONFIG_ERROR_COUNT;

    CHECK(runtime_config_validate(&config, &error));
    CHECK_EQ(RUNTIME_CONFIG_ERROR_NONE, error);
    CHECK(!runtime_config_validate(NULL, &error));
    CHECK_EQ(RUNTIME_CONFIG_ERROR_NULL_CONFIG, error);
    CHECK(!runtime_config_validate(NULL, NULL));

    config = valid_config();
    config.unit = GLUCOSE_UNIT_COUNT;
    expect_error(&config, RUNTIME_CONFIG_ERROR_UNIT);
    config = valid_config();
    config.brightness_day_percent = 0U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_BRIGHTNESS_DAY);
    config = valid_config();
    config.brightness_night_percent = 101U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_BRIGHTNESS_NIGHT);
    config = valid_config();
    config.night_end_minute = 1440U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE);
    config = valid_config();
    config.night_end_minute = config.night_start_minute;
    expect_error(&config, RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE);
    config = valid_config();
    config.freshness.aging_max_s = 1U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_FRESHNESS);
    config = valid_config();
    config.freshness.stale_max_s = 900U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_FRESHNESS);
    config = valid_config();
    config.poll_s = 19U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_POLL);
    config = valid_config();
    config.ota_enabled = 2U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_OTA_FLAG);
    config = valid_config();
    config.wifi_ssid[0] = '\0';
    config.unit = GLUCOSE_UNIT_COUNT;
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);

    CHECK_EQ(20U, runtime_poll_interval_clamp(0U));
    CHECK_EQ(20U, runtime_poll_interval_clamp(19U));
    CHECK_EQ(20U, runtime_poll_interval_clamp(20U));
    CHECK_EQ(300U, runtime_poll_interval_clamp(300U));
    CHECK_EQ(300U, runtime_poll_interval_clamp(301U));
    CHECK_EQ(300U, runtime_poll_interval_clamp(UINT32_MAX));
}

static void test_exact_string_boundaries_and_precedence(void)
{
    runtime_config_t config;
    runtime_config_error_t expected;
    size_t index;

    config = valid_config();
    config.wifi_ssid[0] = 'x';
    config.wifi_ssid[1] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    memset(config.wifi_ssid, 'x', sizeof(config.wifi_ssid));
    expect_error(&config, RUNTIME_CONFIG_ERROR_SSID);

    config = valid_config();
    memset(config.wifi_password, 'x', 8U);
    config.wifi_password[8] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    config.wifi_password[0] = 'x';
    config.wifi_password[1] = '\0';
    expect_error(&config, RUNTIME_CONFIG_ERROR_PASSWORD);
    memset(config.wifi_password, 'x', sizeof(config.wifi_password));
    expect_error(&config, RUNTIME_CONFIG_ERROR_PASSWORD);

    config = valid_config();
    memset(config.bridge_origin, 'a', sizeof(config.bridge_origin));
    memcpy(config.bridge_origin, "https://", 8U);
    for (index = 8U; index < 255U; index++) {
        config.bridge_origin[index] = 'a';
    }
    config.bridge_origin[8U + 63U] = '.';
    config.bridge_origin[8U + 63U + 1U + 63U] = '.';
    config.bridge_origin[8U + 63U + 1U + 63U + 1U + 63U] = '.';
    config.bridge_origin[255U] = '\0';
    CHECK(runtime_config_validate(&config, NULL));
    config.bridge_origin[255U] = 'x';
    expect_error(&config, RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN);

    config = valid_config();
    config.brightness_day_percent = 1U;
    config.brightness_night_percent = 1U;
    config.night_start_minute = 0U;
    config.night_end_minute = 1439U;
    config.freshness.fresh_max_s = 1U;
    config.freshness.aging_max_s = 2U;
    config.freshness.stale_max_s = 901U;
    config.poll_s = 20U;
    config.ota_enabled = 0U;
    CHECK(runtime_config_validate(&config, NULL));
    config = valid_config();
    config.brightness_day_percent = 100U;
    config.brightness_night_percent = 100U;
    config.night_start_minute = 1439U;
    config.night_end_minute = 0U;
    config.freshness.fresh_max_s = 899U;
    config.freshness.aging_max_s = 900U;
    config.freshness.stale_max_s = 86400U;
    config.poll_s = 300U;
    config.ota_enabled = 1U;
    CHECK(runtime_config_validate(&config, NULL));
    config = valid_config();
    config.freshness.fresh_max_s = 900U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_FRESHNESS);
    config = valid_config();
    config.freshness.stale_max_s = 900U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_FRESHNESS);
    config = valid_config();
    config.freshness.stale_max_s = 86401U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_FRESHNESS);
    config = valid_config();
    config.poll_s = 301U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_POLL);
    config = valid_config();
    config.ota_enabled = 2U;
    expect_error(&config, RUNTIME_CONFIG_ERROR_OTA_FLAG);

    for (expected = RUNTIME_CONFIG_ERROR_SSID;
         expected < RUNTIME_CONFIG_ERROR_COUNT; expected++) {
        config = valid_config();
        switch (expected) {
        case RUNTIME_CONFIG_ERROR_SSID: config.wifi_ssid[0] = '\0'; break;
        case RUNTIME_CONFIG_ERROR_PASSWORD: config.wifi_password[0] = 'x'; config.wifi_password[1] = '\0'; break;
        case RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN: config.bridge_origin[0] = '\0'; break;
        case RUNTIME_CONFIG_ERROR_DEVICE_TOKEN: config.device_token[42] = '='; break;
        case RUNTIME_CONFIG_ERROR_UNIT: config.unit = GLUCOSE_UNIT_COUNT; break;
        case RUNTIME_CONFIG_ERROR_BRIGHTNESS_DAY: config.brightness_day_percent = 0U; break;
        case RUNTIME_CONFIG_ERROR_BRIGHTNESS_NIGHT: config.brightness_night_percent = 101U; break;
        case RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE: config.night_end_minute = 1440U; break;
        case RUNTIME_CONFIG_ERROR_TZ: config.tz[0] = '\0'; break;
        case RUNTIME_CONFIG_ERROR_FRESHNESS: config.freshness.aging_max_s = 1U; break;
        case RUNTIME_CONFIG_ERROR_POLL: config.poll_s = 19U; break;
        case RUNTIME_CONFIG_ERROR_OTA_FLAG: config.ota_enabled = 2U; break;
        default: break;
        }
        expect_error(&config, expected);
    }

    {
        runtime_config_t clean = valid_config();
        config = valid_config();
        config.wifi_ssid[0] = '\0';
        config.wifi_password[0] = 'x'; config.wifi_password[1] = '\0';
        config.bridge_origin[0] = '\0';
        config.device_token[42] = '=';
        config.unit = GLUCOSE_UNIT_COUNT;
        config.brightness_day_percent = 0U;
        config.brightness_night_percent = 101U;
        config.night_end_minute = 1440U;
        config.tz[0] = '\0';
        config.freshness.aging_max_s = 1U;
        config.poll_s = 19U;
        config.ota_enabled = 2U;
        for (expected = RUNTIME_CONFIG_ERROR_SSID;
             expected < RUNTIME_CONFIG_ERROR_COUNT; expected++) {
            expect_error(&config, expected);
            switch (expected) {
            case RUNTIME_CONFIG_ERROR_SSID: strcpy(config.wifi_ssid, clean.wifi_ssid); break;
            case RUNTIME_CONFIG_ERROR_PASSWORD: strcpy(config.wifi_password, clean.wifi_password); break;
            case RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN: strcpy(config.bridge_origin, clean.bridge_origin); break;
            case RUNTIME_CONFIG_ERROR_DEVICE_TOKEN: strcpy(config.device_token, clean.device_token); break;
            case RUNTIME_CONFIG_ERROR_UNIT: config.unit = clean.unit; break;
            case RUNTIME_CONFIG_ERROR_BRIGHTNESS_DAY: config.brightness_day_percent = clean.brightness_day_percent; break;
            case RUNTIME_CONFIG_ERROR_BRIGHTNESS_NIGHT: config.brightness_night_percent = clean.brightness_night_percent; break;
            case RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE: config.night_end_minute = clean.night_end_minute; break;
            case RUNTIME_CONFIG_ERROR_TZ: strcpy(config.tz, clean.tz); break;
            case RUNTIME_CONFIG_ERROR_FRESHNESS: config.freshness.aging_max_s = clean.freshness.aging_max_s; break;
            case RUNTIME_CONFIG_ERROR_POLL: config.poll_s = clean.poll_s; break;
            case RUNTIME_CONFIG_ERROR_OTA_FLAG: config.ota_enabled = clean.ota_enabled; break;
            default: break;
            }
        }
    }
}

static void test_http_exchange_contract(void)
{
    runtime_http_exchange_t exchange;
    static const uint8_t body[] = "{\"ok\":true}";
    static const uint8_t full_latest_body[RUNTIME_HTTP_BODY_CAPACITY] = {0};
    static const uint8_t full_error_body[GLUCOSE_ERROR_JSON_MAX_BYTES] = {0};
    static const uint8_t one_byte = 0U;
    unsigned status;

    runtime_http_exchange_init(&exchange);
    CHECK(!runtime_http_exchange_set_status(&exchange, 99U));
    CHECK(exchange.failed);
    runtime_http_exchange_init(&exchange);
    CHECK(!runtime_http_exchange_set_status(&exchange, 600U));
    CHECK(exchange.failed);

    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 200U));
    CHECK(runtime_http_exchange_append_body(&exchange, full_latest_body,
                                            sizeof(full_latest_body)));
    CHECK_EQ(RUNTIME_HTTP_RESULT_BODY, runtime_http_exchange_finish(&exchange));
    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 200U));
    CHECK(runtime_http_exchange_append_body(&exchange, full_latest_body,
                                            sizeof(full_latest_body)));
    CHECK(!runtime_http_exchange_append_body(&exchange, &one_byte, 1U));
    CHECK(exchange.failed);

    for (status = 400U; status <= 599U; status += 199U) {
        runtime_http_exchange_init(&exchange);
        CHECK(runtime_http_exchange_set_status(&exchange, status));
        CHECK(runtime_http_exchange_append_body(&exchange, full_error_body,
                                                sizeof(full_error_body)));
        CHECK_EQ(RUNTIME_HTTP_RESULT_ERROR_BODY, runtime_http_exchange_finish(&exchange));
        runtime_http_exchange_init(&exchange);
        CHECK(runtime_http_exchange_set_status(&exchange, status));
        CHECK(runtime_http_exchange_append_body(&exchange, full_error_body,
                                                sizeof(full_error_body)));
        CHECK(!runtime_http_exchange_append_body(&exchange, &one_byte, 1U));
        CHECK(exchange.failed);
    }

    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 200U));
    CHECK(runtime_http_exchange_add_header(&exchange, "eTaG", 5U, "\"abc\"", 6U));
    CHECK(runtime_http_exchange_add_header(&exchange, "Retry-After", 12U, "30", 3U));
    CHECK(runtime_http_exchange_append_body(&exchange, body, sizeof(body) - 1U));
    CHECK_EQ(RUNTIME_HTTP_RESULT_BODY, runtime_http_exchange_finish(&exchange));
    CHECK(exchange.has_etag);
    CHECK(strcmp(exchange.etag, "\"abc\"") == 0);
    CHECK_EQ(30U, exchange.retry_after_s);

    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 304U));
    CHECK(runtime_http_exchange_append_body(&exchange, NULL, 0U));
    CHECK(runtime_http_exchange_add_header(&exchange, "X-Reading-Age-S", 16U, "47", 3U));
    CHECK(runtime_http_exchange_add_header(&exchange, "x-reading-server-time", 22U,
                                           "2026-08-22T09:15:47Z", 21U));
    CHECK_EQ(RUNTIME_HTTP_RESULT_NOT_MODIFIED, runtime_http_exchange_finish(&exchange));

    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 304U));
    CHECK(runtime_http_exchange_add_header(&exchange, "x-reading-age-s", 16U, "47", 3U));
    CHECK(!runtime_http_exchange_add_header(&exchange, "x-reading-server-time", 22U,
                                            "1969-12-31T23:59:59Z", 21U));
    CHECK(exchange.failed);

    runtime_http_exchange_init(&exchange);
    CHECK(runtime_http_exchange_set_status(&exchange, 304U));
    CHECK(runtime_http_exchange_add_header(&exchange, "x-reading-age-s", 16U, "47", 3U));
    CHECK_EQ(RUNTIME_HTTP_RESULT_MALFORMED, runtime_http_exchange_finish(&exchange));
    CHECK(exchange.failed);
    CHECK(!exchange.status_set);
}

static int test_ota_json(char *json, size_t capacity,
                         const char *version, const char *url,
                         const char *sha256, const char *minimum)
{
    int written = snprintf(json, capacity,
                           "{\"version\":\"%s\",\"url\":\"%s\","
                           "\"sha256\":\"%s\",\"min_prev_version\":\"%s\"}",
                           version, url, sha256, minimum);
    CHECK(written >= 0);
    CHECK((size_t)written < capacity);
    return written;
}

static void test_ota_manifest_contract(void)
{
    static const char sha256[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char valid_version[] = "0.3.0";
    static const char valid_url[] = "https://bridge.example.com/firmware.bin";
    static const char minimum[] = "0.2.0-runtime-foundation";
    char json[TEST_OTA_MANIFEST_MAX_BYTES + 2U];
    char maximum_url[TEST_OTA_URL_CAPACITY];
    char maximum_version[TEST_OTA_VERSION_CAPACITY];
    char over_version[TEST_OTA_VERSION_CAPACITY + 1U];
    char current_version[TEST_OTA_VERSION_CAPACITY];
    test_ota_manifest_t manifest;
    size_t length;
    size_t index;

    memset(&manifest, 0xa5, sizeof(manifest));
    CHECK_EQ(TEST_OTA_PARSE_NULL_ARGUMENT,
             runtime_ota_manifest_parse(NULL, 0U, &manifest));
    for (index = 0U; index < sizeof(manifest); index++) {
        CHECK(((const unsigned char *)&manifest)[index] == 0U);
    }
    CHECK_EQ(TEST_OTA_PARSE_NULL_ARGUMENT,
             runtime_ota_manifest_parse((const unsigned char *)"{}", 2U, NULL));
    CHECK_EQ(TEST_OTA_PARSE_INVALID_JSON,
             runtime_ota_manifest_parse((const unsigned char *)"", 0U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_BODY_TOO_LARGE,
             runtime_ota_manifest_parse((const unsigned char *)"{}", TEST_OTA_MANIFEST_MAX_BYTES + 1U,
                                         &manifest));

    length = (size_t)test_ota_json(json, sizeof(json), valid_version, valid_url,
                                   sha256, minimum);
    while (length < TEST_OTA_MANIFEST_MAX_BYTES) {
        json[length++] = ' ';
    }
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    json[length] = ' ';
    CHECK_EQ(TEST_OTA_PARSE_BODY_TOO_LARGE,
             runtime_ota_manifest_parse((const unsigned char *)json, length + 1U, &manifest));

    CHECK_EQ(TEST_OTA_PARSE_MISSING_FIELD,
             runtime_ota_manifest_parse((const unsigned char *)"{\"version\":\"0.3.0\"}",
                                         sizeof("{\"version\":\"0.3.0\"}") - 1U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_DUPLICATE_FIELD,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"version\":\"0.3.0\",\"version\":\"0.3.0\"}",
                                         sizeof("{\"version\":\"0.3.0\",\"version\":\"0.3.0\"}") - 1U,
                                         &manifest));
    CHECK_EQ(TEST_OTA_PARSE_UNKNOWN_FIELD,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"unknown\":\"x\"}",
                                         sizeof("{\"unknown\":\"x\"}") - 1U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_UNKNOWN_FIELD,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"abcdefghijklmnopqrst\":\"x\"}",
                                         sizeof("{\"abcdefghijklmnopqrst\":\"x\"}") - 1U,
                                         &manifest));
    CHECK_EQ(TEST_OTA_PARSE_INVALID_JSON,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"version\":1}",
                                         sizeof("{\"version\":1}") - 1U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_INVALID_JSON,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"version\":\"0.3.0\\n\"}",
                                         sizeof("{\"version\":\"0.3.0\\n\"}") - 1U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_INVALID_JSON,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"version\":{}}",
                                         sizeof("{\"version\":{}}") - 1U, &manifest));
    CHECK_EQ(TEST_OTA_PARSE_INVALID_JSON,
             runtime_ota_manifest_parse((const unsigned char *)
                                         "{\"version\":\"0.3.0\"}x",
                                         sizeof("{\"version\":\"0.3.0\"}x") - 1U, &manifest));
    length = (size_t)test_ota_json(json, sizeof(json), valid_version,
                                   "http://bridge.example.com/firmware.bin",
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_INVALID_URL,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    length = (size_t)test_ota_json(json, sizeof(json), valid_version, valid_url,
                                   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeG",
                                   minimum);
    CHECK_EQ(TEST_OTA_PARSE_INVALID_SHA256,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    length = (size_t)test_ota_json(json, sizeof(json), valid_version, valid_url,
                                   sha256, "0.3.0");
    CHECK_EQ(TEST_OTA_PARSE_INVALID_VERSION_RELATION,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    length = (size_t)test_ota_json(json, sizeof(json), valid_version, valid_url,
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));

    memset(maximum_version, 'a', sizeof(maximum_version));
    memcpy(maximum_version, "1.2.3-", sizeof("1.2.3-") - 1U);
    maximum_version[31U] = '\0';
    length = (size_t)test_ota_json(json, sizeof(json), maximum_version, valid_url,
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    memcpy(over_version, maximum_version, sizeof(maximum_version) - 1U);
    over_version[sizeof(maximum_version) - 1U] = 'a';
    over_version[sizeof(over_version) - 1U] = '\0';
    length = (size_t)test_ota_json(json, sizeof(json), over_version, valid_url,
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_INVALID_VERSION,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));

    memcpy(maximum_url, "https://a.b/", sizeof("https://a.b/") - 1U);
    for (index = sizeof("https://a.b/") - 1U; index < sizeof(maximum_url) - 1U; index++) {
        maximum_url[index] = 'a';
    }
    maximum_url[sizeof(maximum_url) - 1U] = '\0';
    length = (size_t)test_ota_json(json, sizeof(json), valid_version, maximum_url,
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    CHECK(runtime_ota_url_same_origin(maximum_url, sizeof(maximum_url),
                                      "https://a.b", sizeof("https://a.b")));
    maximum_url[sizeof(maximum_url) - 1U] = 'a';
    CHECK(!runtime_ota_url_same_origin(maximum_url, sizeof(maximum_url),
                                       "https://a.b", sizeof("https://a.b")));

    CHECK(runtime_ota_url_same_origin("https://a.b/path", sizeof("https://a.b/path"),
                                      "https://a.b", sizeof("https://a.b")));
    CHECK(runtime_ota_url_same_origin("https://a.b:443/path", sizeof("https://a.b:443/path"),
                                      "https://a.b", sizeof("https://a.b")));
    CHECK(!runtime_ota_url_same_origin("https://a.b:444/path", sizeof("https://a.b:444/path"),
                                       "https://a.b", sizeof("https://a.b")));
    CHECK(!runtime_ota_url_same_origin("https://a.b.evil/path", sizeof("https://a.b.evil/path"),
                                       "https://a.b", sizeof("https://a.b")));
    CHECK(!runtime_ota_url_same_origin("https://evil-a.b/path", sizeof("https://evil-a.b/path"),
                                       "https://a.b", sizeof("https://a.b")));
    CHECK(!runtime_ota_url_same_origin("https://a.b/path", sizeof("https://a.b/path"),
                                       "https://a.b", 0U));

    length = (size_t)test_ota_json(json, sizeof(json), "0.3.0", valid_url,
                                   sha256, minimum);
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    CHECK_EQ(TEST_OTA_POLICY_UPDATE,
             runtime_ota_manifest_policy(&manifest, minimum, sizeof(minimum)));
    CHECK_EQ(TEST_OTA_POLICY_UP_TO_DATE,
             runtime_ota_manifest_policy(&manifest, "0.3.0", sizeof("0.3.0")));
    CHECK_EQ(TEST_OTA_POLICY_DOWNGRADE,
             runtime_ota_manifest_policy(&manifest, "0.4.0", sizeof("0.4.0")));
    CHECK_EQ(TEST_OTA_POLICY_REQUIRES_INTERMEDIATE,
             runtime_ota_manifest_policy(&manifest, "0.1.0", sizeof("0.1.0")));
    length = (size_t)test_ota_json(json, sizeof(json), "1.0.0-alpha.10", valid_url,
                                   sha256, "1.0.0-alpha.2");
    CHECK_EQ(TEST_OTA_PARSE_OK,
             runtime_ota_manifest_parse((const unsigned char *)json, length, &manifest));
    CHECK_EQ(TEST_OTA_POLICY_UPDATE,
             runtime_ota_manifest_policy(&manifest, "1.0.0-alpha.2",
                                         sizeof("1.0.0-alpha.2")));
    CHECK_EQ(TEST_OTA_POLICY_INVALID,
             runtime_ota_manifest_policy(&manifest, "0.2.0", 0U));
    memcpy(current_version, "0.2.0", sizeof("0.2.0"));
    memset(current_version + sizeof("0.2.0") - 1U, 'x',
           sizeof(current_version) - sizeof("0.2.0") + 1U);
    CHECK_EQ(TEST_OTA_POLICY_INVALID,
             runtime_ota_manifest_policy(&manifest, current_version, sizeof(current_version)));
    memset(manifest.version, 'x', sizeof(manifest.version));
    CHECK_EQ(TEST_OTA_POLICY_INVALID,
             runtime_ota_manifest_policy(&manifest, "0.2.0", sizeof("0.2.0")));
}

static void test_ui_snapshot_neutral_and_mapping(void)
{
    glucose_core_t core;
    runtime_ui_snapshot_t snapshot;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};
    glucose_display_state_t states[] = {
        GLUCOSE_STATE_BOOT, GLUCOSE_STATE_WIFI_CONNECTING,
        GLUCOSE_STATE_TIME_SYNCING, GLUCOSE_STATE_FETCHING,
        GLUCOSE_STATE_FRESH, GLUCOSE_STATE_AGING, GLUCOSE_STATE_STALE,
        GLUCOSE_STATE_NO_DATA, GLUCOSE_STATE_AUTH_ERROR,
        GLUCOSE_STATE_SRV_ERROR, GLUCOSE_STATE_NO_WIFI
    };
    ui_screen_kind_t screens[] = {
        UI_SCREEN_BOOT, UI_SCREEN_WIFI_CONNECTING, UI_SCREEN_TIME_SYNCING,
        UI_SCREEN_FETCHING, UI_SCREEN_FRESH, UI_SCREEN_AGING,
        UI_SCREEN_STALE, UI_SCREEN_NO_DATA, UI_SCREEN_AUTH_ERROR,
        UI_SCREEN_SERVER_ERROR, UI_SCREEN_NO_WIFI
    };
    size_t index;

    runtime_ui_snapshot_make_neutral(&snapshot);
    CHECK(snapshot.screen == UI_SCREEN_NO_DATA);
    CHECK(!snapshot.model.show_number && !snapshot.model.use_range_hue);
    CHECK(snapshot.model.trend == GLUCOSE_TREND_UNKNOWN);
    CHECK(snapshot.model.range == GLUCOSE_RANGE_UNKNOWN);
    CHECK(strcmp(snapshot.model.value_text, "--") == 0);
    CHECK(strcmp(snapshot.clock_text, "") == 0);
    CHECK(runtime_ui_snapshot_is_valid(&snapshot));

    CHECK(glucose_core_init(&core, &freshness));
    for (index = 0U; index < sizeof(states) / sizeof(states[0]); index++) {
        core.state = states[index];
        if (states[index] == GLUCOSE_STATE_FRESH || states[index] == GLUCOSE_STATE_AGING) {
            core.has_reading = true;
            core.reading.value_mg_dl = 112.0;
            core.reading.unit = GLUCOSE_UNIT_MG_DL;
            core.reading.trend = GLUCOSE_TREND_STABLE;
            core.reading.range = GLUCOSE_RANGE_IN_RANGE;
            core.reading.libre_local_seconds = 23U * 3600U + 59U * 60U + 50U;
            core.current_age_s = 20U;
        }
        CHECK(runtime_ui_snapshot_build(&core, &snapshot));
        CHECK(snapshot.screen == screens[index]);
        CHECK(runtime_ui_snapshot_is_valid(&snapshot));
        CHECK(strcmp(snapshot.clock_text,
                     states[index] == GLUCOSE_STATE_FRESH ||
                     states[index] == GLUCOSE_STATE_AGING ? "00:00" : "") == 0);
    }
}

static void test_ui_snapshot_fail_closed_and_copy(void)
{
    glucose_core_t core;
    runtime_ui_snapshot_t snapshot;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};

    CHECK(glucose_core_init(&core, &freshness));
    CHECK(glucose_core_set_operational_state(&core, GLUCOSE_STATE_NO_WIFI));
    CHECK(runtime_ui_snapshot_build(&core, &snapshot));
    CHECK(!snapshot.model.show_number && !snapshot.model.use_range_hue);
    snapshot.screen = (ui_screen_kind_t)UI_SCREEN_COUNT;
    CHECK(!runtime_ui_snapshot_is_valid(&snapshot));
    CHECK(!runtime_ui_snapshot_build(NULL, &snapshot));
    CHECK(snapshot.screen == UI_SCREEN_NO_DATA);
    CHECK(!runtime_ui_snapshot_build(&core, NULL));
}

static runtime_ui_snapshot_t fresh_snapshot_for_mutation(void)
{
    glucose_core_t core;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};
    runtime_ui_snapshot_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    CHECK(glucose_core_init(&core, &freshness));
    core.state = GLUCOSE_STATE_FRESH;
    core.has_reading = true;
    core.reading.value_mg_dl = 112.0;
    core.reading.unit = GLUCOSE_UNIT_MG_DL;
    core.reading.trend = GLUCOSE_TREND_STABLE;
    core.reading.range = GLUCOSE_RANGE_IN_RANGE;
    core.reading.libre_local_seconds = 9U * 3600U + 38U * 60U + 30U;
    core.current_age_s = 10U;
    CHECK(runtime_ui_snapshot_build(&core, &snapshot));
    CHECK(strcmp(snapshot.clock_text, "09:38") == 0);
    return snapshot;
}

static void expect_snapshot_invalid(const runtime_ui_snapshot_t *snapshot)
{
    CHECK(!runtime_ui_snapshot_is_valid(snapshot));
}

static void test_ui_snapshot_mutation_matrix(void)
{
    runtime_ui_snapshot_t snapshot;
    ui_screen_kind_t screen;
    glucose_display_state_t state;
    const ui_screen_kind_t mapped[] = {
        UI_SCREEN_BOOT, UI_SCREEN_WIFI_CONNECTING, UI_SCREEN_TIME_SYNCING,
        UI_SCREEN_FETCHING, UI_SCREEN_FRESH, UI_SCREEN_AGING, UI_SCREEN_STALE,
        UI_SCREEN_NO_DATA, UI_SCREEN_AUTH_ERROR, UI_SCREEN_SERVER_ERROR,
        UI_SCREEN_NO_WIFI
    };

    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.unit_text[0] = 'x'; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    memset(snapshot.model.unit_text, 'x', sizeof(snapshot.model.unit_text));
    expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.age_text[0] = 'x'; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    strcpy(snapshot.model.age_text, "1h"); expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.status_text[0] = 'x'; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.trend = (glucose_trend_t)-1; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.trend = GLUCOSE_TREND_COUNT; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.range = (glucose_range_t)-1; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.range = GLUCOSE_RANGE_COUNT; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.show_number = false; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    snapshot.model.use_range_hue = false; expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    strcpy(snapshot.model.value_text, "1"); expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    strcpy(snapshot.model.value_text, "999"); expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    strcpy(snapshot.clock_text, "9:38"); expect_snapshot_invalid(&snapshot);
    snapshot = fresh_snapshot_for_mutation();
    strcpy(snapshot.clock_text, "24:00"); expect_snapshot_invalid(&snapshot);

    for (screen = UI_SCREEN_BOOT; screen < UI_SCREEN_COUNT; screen++) {
        for (state = GLUCOSE_STATE_BOOT; state < GLUCOSE_STATE_COUNT; state++) {
            snapshot = fresh_snapshot_for_mutation();
            snapshot.screen = screen;
            snapshot.model.state = state;
            if (screen != UI_SCREEN_FRESH && screen != UI_SCREEN_AGING) {
                snapshot.model.show_number = false;
                snapshot.model.use_range_hue = false;
                strcpy(snapshot.model.value_text, "--");
                strcpy(snapshot.model.unit_text, "--");
                snapshot.clock_text[0] = '\0';
            }
            if (state < (sizeof(mapped) / sizeof(mapped[0])) &&
                screen != mapped[state]) {
                expect_snapshot_invalid(&snapshot);
            }
        }
    }
}

static void test_ui_snapshot_output_clears_every_build_failure(void)
{
    glucose_core_t core;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};
    runtime_ui_snapshot_t snapshot;
    CHECK(glucose_core_init(&core, &freshness));
    core.state = (glucose_display_state_t)GLUCOSE_STATE_COUNT;
    memset(&snapshot, 0xCC, sizeof(snapshot));
    CHECK(!runtime_ui_snapshot_build(&core, &snapshot));
    CHECK(snapshot.screen == UI_SCREEN_NO_DATA);
    CHECK(runtime_ui_snapshot_is_valid(&snapshot));
    memset(&snapshot, 0xCC, sizeof(snapshot));
    CHECK(!runtime_ui_snapshot_build(NULL, &snapshot));
    CHECK(runtime_ui_snapshot_is_valid(&snapshot));
}

static void test_ui_snapshot_clock_boundaries_and_semantic_equality(void)
{
    glucose_core_t core;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};
    runtime_ui_snapshot_t snapshot;
    runtime_ui_snapshot_t other;

    CHECK(glucose_core_init(&core, &freshness));
    core.state = GLUCOSE_STATE_FRESH;
    core.has_reading = true;
    core.reading.value_mg_dl = 112.0;
    core.reading.unit = GLUCOSE_UNIT_MG_DL;
    core.reading.trend = GLUCOSE_TREND_STABLE;
    core.reading.range = GLUCOSE_RANGE_IN_RANGE;

    core.reading.libre_local_seconds = 9U * 3600U + 59U * 60U + 59U;
    core.current_age_s = 1U;
    CHECK(runtime_ui_snapshot_build(&core, &snapshot));
    CHECK(strcmp(snapshot.clock_text, "10:00") == 0);

    core.reading.libre_local_seconds = 23U * 3600U + 59U * 60U + 59U;
    CHECK(runtime_ui_snapshot_build(&core, &snapshot));
    CHECK(strcmp(snapshot.clock_text, "00:00") == 0);

    other = snapshot;
    CHECK(runtime_ui_snapshot_equal(&snapshot, &other));
    CHECK(!runtime_ui_snapshot_equal(NULL, &other));
    CHECK(!runtime_ui_snapshot_equal(&snapshot, NULL));

#define CHECK_DIFFERENT(statement) do { \
    other = snapshot; \
    statement; \
    CHECK(!runtime_ui_snapshot_equal(&snapshot, &other)); \
} while (0)
    CHECK_DIFFERENT(other.screen = UI_SCREEN_AGING);
    CHECK_DIFFERENT(other.model.state = GLUCOSE_STATE_AGING);
    CHECK_DIFFERENT(other.model.show_number = false);
    CHECK_DIFFERENT(other.model.use_range_hue = false);
    CHECK_DIFFERENT(other.model.trend = GLUCOSE_TREND_RISING);
    CHECK_DIFFERENT(other.model.range = GLUCOSE_RANGE_HIGH);
    CHECK_DIFFERENT(other.model.value_text[0] = '9');
    CHECK_DIFFERENT(other.model.unit_text[0] = 'x');
    CHECK_DIFFERENT(other.model.age_text[0] = 'x');
    CHECK_DIFFERENT(other.model.status_text[0] = 'x');
    CHECK_DIFFERENT(other.clock_text[0] = '1');
#undef CHECK_DIFFERENT
}

static bool provisioning_non_wifi_equal(const runtime_config_t *left,
                                         const runtime_config_t *right)
{
    return strcmp(left->bridge_origin, right->bridge_origin) == 0 &&
           strcmp(left->device_token, right->device_token) == 0 &&
           strcmp(left->tz, right->tz) == 0 &&
           left->unit == right->unit &&
           left->brightness_day_percent == right->brightness_day_percent &&
           left->brightness_night_percent == right->brightness_night_percent &&
           left->night_start_minute == right->night_start_minute &&
           left->night_end_minute == right->night_end_minute &&
           left->freshness.fresh_max_s == right->freshness.fresh_max_s &&
           left->freshness.aging_max_s == right->freshness.aging_max_s &&
           left->freshness.stale_max_s == right->freshness.stale_max_s &&
           left->poll_s == right->poll_s &&
           left->ota_enabled == right->ota_enabled;
}

static void test_wifi_provisioning_ap_credentials(void)
{
    static const uint8_t mac[6] = {0xaaU, 0xbbU, 0xccU, 0x12U, 0x34U, 0x56U};
    static const uint8_t random[8] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
    };
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char ssid[WIFI_PROVISIONING_AP_SSID_CAPACITY];
    char password[WIFI_PROVISIONING_AP_PASSWORD_CAPACITY];
    size_t index;

    memset(ssid, 0xa5, sizeof(ssid));
    memset(password, 0xa5, sizeof(password));
    CHECK(wifi_provisioning_ap_credentials(mac, sizeof(mac), random, sizeof(random),
                                           ssid, sizeof(ssid),
                                           password, sizeof(password)));
    CHECK(strcmp(ssid, "LibreDesk-123456") == 0);
    CHECK(strcmp(password, "AAAAAAAAAAAA") == 0);
    CHECK(strlen(password) == 12U);
    for (index = 0U; index < strlen(password); index++) {
        CHECK(strchr(alphabet, password[index]) != NULL);
    }

    memset(ssid, 0xa5, sizeof(ssid));
    memset(password, 0xa5, sizeof(password));
    CHECK(!wifi_provisioning_ap_credentials(NULL, sizeof(mac), random, sizeof(random),
                                            ssid, sizeof(ssid), password, sizeof(password)));
    CHECK(ssid[0] == '\0' && password[0] == '\0');
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac) - 1U,
                                            random, sizeof(random), ssid, sizeof(ssid),
                                            password, sizeof(password)));
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac),
                                            random, sizeof(random) - 1U,
                                            ssid, sizeof(ssid), password, sizeof(password)));
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac), random, sizeof(random),
                                            ssid, sizeof(ssid) - 1U,
                                            password, sizeof(password)));
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac), random, sizeof(random),
                                            ssid, sizeof(ssid),
                                            password, sizeof(password) - 1U));
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac), random, sizeof(random),
                                            NULL, sizeof(ssid), password, sizeof(password)));
    CHECK(!wifi_provisioning_ap_credentials(mac, sizeof(mac), random, sizeof(random),
                                            ssid, sizeof(ssid), NULL, sizeof(password)));
}

static void expect_provisioning_form_error(const uint8_t *body, size_t length,
                                           wifi_provisioning_result_t expected)
{
    wifi_provisioning_submission_t output;
    memset(&output, 0xa5, sizeof(output));
    CHECK_EQ(expected, wifi_provisioning_parse_form(body, length, &output));
    CHECK(output.ssid[0] == '\0');
    CHECK(output.password[0] == '\0');
}

static void test_wifi_provisioning_form_and_candidate(void)
{
    static const char valid[] = "ssid=New+Desk&password=newpass1";
    static const char open[] = "password=&ssid=Cafe%20Wi-Fi";
    static const char utf8[] = "ssid=Cafe-%E2%98%95&password=abcdefgh";
    static const char hex64[] =
        "ssid=Hex&password=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char *const malformed[] = {
        "", "ssid=Desk", "password=abcdefgh", "ssid=A&ssid=B&password=abcdefgh",
        "ssid=A&password=abcdefgh&password=ijklmnop",
        "ssid=A&password=abcdefgh&token=secret", "foo=A&password=abcdefgh",
        "ssid=A%&password=abcdefgh", "ssid=A%0&password=abcdefgh",
        "ssid=A%GG&password=abcdefgh", "ssid=A&&password=abcdefgh",
        "=A&password=abcdefgh", "ssid=A&password=abcdefgh&"
    };
    static const uint8_t nul_body[] = {
        's','s','i','d','=','A','\0','&','p','a','s','s','w','o','r','d','=','a','b','c','d','e','f','g','h'
    };
    wifi_provisioning_submission_t submission;
    runtime_config_t base = valid_config();
    runtime_config_t candidate;
    uint8_t oversized[WIFI_PROVISIONING_FORM_MAX_BYTES + 1U];
    size_t index;

    CHECK_EQ(WIFI_PROVISIONING_OK,
             wifi_provisioning_parse_form((const uint8_t *)valid,
                                          sizeof(valid) - 1U, &submission));
    CHECK(strcmp(submission.ssid, "New Desk") == 0);
    CHECK(strcmp(submission.password, "newpass1") == 0);
    CHECK(wifi_provisioning_candidate_build(&base, &submission, &candidate));
    CHECK(strcmp(candidate.wifi_ssid, "New Desk") == 0);
    CHECK(strcmp(candidate.wifi_password, "newpass1") == 0);
    CHECK(provisioning_non_wifi_equal(&base, &candidate));
    CHECK(runtime_config_validate(&candidate, NULL));

    CHECK_EQ(WIFI_PROVISIONING_OK,
             wifi_provisioning_parse_form((const uint8_t *)open,
                                          sizeof(open) - 1U, &submission));
    CHECK(strcmp(submission.ssid, "Cafe Wi-Fi") == 0);
    CHECK(submission.password[0] == '\0');
    CHECK(wifi_provisioning_candidate_build(&base, &submission, &candidate));

    CHECK_EQ(WIFI_PROVISIONING_OK,
             wifi_provisioning_parse_form((const uint8_t *)utf8,
                                          sizeof(utf8) - 1U, &submission));
    CHECK(wifi_provisioning_candidate_build(&base, &submission, &candidate));
    CHECK_EQ(WIFI_PROVISIONING_OK,
             wifi_provisioning_parse_form((const uint8_t *)hex64,
                                          sizeof(hex64) - 1U, &submission));
    CHECK(strlen(submission.password) == 64U);
    CHECK(wifi_provisioning_candidate_build(&base, &submission, &candidate));

    for (index = 0U; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        expect_provisioning_form_error((const uint8_t *)malformed[index],
                                       strlen(malformed[index]),
                                       WIFI_PROVISIONING_MALFORMED);
    }
    expect_provisioning_form_error(nul_body, sizeof(nul_body),
                                   WIFI_PROVISIONING_MALFORMED);
    memset(oversized, 'x', sizeof(oversized));
    expect_provisioning_form_error(oversized, sizeof(oversized),
                                   WIFI_PROVISIONING_BODY_TOO_LARGE);
    expect_provisioning_form_error(NULL, 1U, WIFI_PROVISIONING_NULL_ARGUMENT);
    CHECK_EQ(WIFI_PROVISIONING_NULL_ARGUMENT,
             wifi_provisioning_parse_form((const uint8_t *)valid,
                                          sizeof(valid) - 1U, NULL));

    memset(&submission, 0, sizeof(submission));
    strcpy(submission.ssid, "Desk");
    strcpy(submission.password, "short");
    memset(&candidate, 0xa5, sizeof(candidate));
    CHECK(!wifi_provisioning_candidate_build(&base, &submission, &candidate));
    CHECK(candidate.wifi_ssid[0] == '\0');
    CHECK(candidate.device_token[0] == '\0');
    CHECK(!wifi_provisioning_candidate_build(NULL, &submission, &candidate));
    CHECK(!wifi_provisioning_candidate_build(&base, NULL, &candidate));
    CHECK(!wifi_provisioning_candidate_build(&base, &submission, NULL));
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--silence") == 0) {
        runtime_config_t sentinel = valid_config();
        runtime_config_error_t error = RUNTIME_CONFIG_ERROR_COUNT;
        size_t index;

        strcpy(sentinel.wifi_ssid, "SENTINEL_SSID");
        strcpy(sentinel.wifi_password, "SENTINEL_PASSWORD");
        strcpy(sentinel.bridge_origin, "https://sentinel.example");
        for (index = 0U; index < 42U; index++) {
            sentinel.device_token[index] = 'B';
        }
        sentinel.device_token[42] = 'E';
        (void)runtime_config_validate(&sentinel, &error);
        return 0;
    }
    test_utf8_boundaries_and_controls();
    test_string_lengths_tokens_and_passwords();
    test_url_numeric_host_matrix();
    test_tz_matrix_and_cursor();
    test_precedence_numeric_ota_and_clamp();
    test_exact_string_boundaries_and_precedence();
    test_http_exchange_contract();
    test_ota_manifest_contract();
    test_ui_snapshot_neutral_and_mapping();
    test_ui_snapshot_fail_closed_and_copy();
    test_ui_snapshot_mutation_matrix();
    test_ui_snapshot_output_clears_every_build_failure();
    test_ui_snapshot_clock_boundaries_and_semantic_equality();
    test_wifi_provisioning_ap_credentials();
    test_wifi_provisioning_form_and_candidate();
    if (fails != 0U) {
        fprintf(stderr, "%u failures\n", fails);
        return 1;
    }
    puts("runtime_core tests passed");
    return 0;
}
