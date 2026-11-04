#include "mvp_config.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void test_builds_valid_bounded_config(void)
{
    runtime_config_t config;
    static const char ssid[] = "MVP WiFi";
    static const char password[] = "password123";
    static const char origin[] = "https://bridge.example.test";
    static const char token[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE";

    CHECK(mvp_config_build(&config, ssid, sizeof(ssid), password, sizeof(password),
                          origin, sizeof(origin), token, sizeof(token)));
    CHECK(strcmp(config.wifi_ssid, ssid) == 0);
    CHECK(strcmp(config.wifi_password, password) == 0);
    CHECK(strcmp(config.bridge_origin, origin) == 0);
    CHECK(strcmp(config.device_token, token) == 0);
    CHECK(config.poll_s == 60U);
    CHECK(config.ota_enabled == 0U);
    CHECK(runtime_config_validate(&config, NULL));
}

static void test_rejects_unterminated_or_oversize_input_and_clears_output(void)
{
    runtime_config_t config;
    static const char oversize_ssid[RUNTIME_WIFI_SSID_CAPACITY] =
        "123456789012345678901234567890123";
    static const char unterminated[] = {'x', 'x', 'x'};
    static const char password[] = "password123";
    static const char origin[] = "https://bridge.example.test";
    static const char token[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE";

    memset(&config, 0xa5, sizeof(config));
    CHECK(!mvp_config_build(&config, unterminated, sizeof(unterminated),
                           password, sizeof(password), origin, sizeof(origin),
                           token, sizeof(token)));
    CHECK(config.wifi_ssid[0] == '\0');
    CHECK(config.bridge_origin[0] == '\0');
    CHECK(!mvp_config_build(&config, oversize_ssid, sizeof(oversize_ssid),
                           password, sizeof(password), origin, sizeof(origin),
                           token, sizeof(token)));
    CHECK(config.wifi_ssid[0] == '\0');
    CHECK(config.device_token[0] == '\0');
}

static void test_rejects_null_inputs_without_writing(void)
{
    runtime_config_t config;
    static const char password[] = "password123";
    static const char origin[] = "https://bridge.example.test";
    static const char token[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE";

    memset(&config, 0xa5, sizeof(config));
    CHECK(!mvp_config_build(&config, NULL, 0U, password, sizeof(password),
                           origin, sizeof(origin), token, sizeof(token)));
    CHECK(config.wifi_ssid[0] == '\0');
    CHECK(config.wifi_password[0] == '\0');
}

static void test_loads_compile_time_fixture(void)
{
    runtime_config_t config;

    CHECK(mvp_config_load(&config));
    CHECK(strcmp(config.wifi_ssid, "fixture-ssid") == 0);
    CHECK(strcmp(config.wifi_password, "fixture-password") == 0);
    CHECK(strcmp(config.bridge_origin, "https://fixture.example.test") == 0);
    CHECK(strcmp(config.device_token,
                  "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBE") == 0);
    CHECK(strcmp(config.tz, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);
    CHECK(config.unit == GLUCOSE_UNIT_MG_DL);
    CHECK(config.freshness.fresh_max_s == 360U);
    CHECK(config.freshness.aging_max_s == 900U);
    CHECK(config.freshness.stale_max_s == 3600U);
}

int main(void)
{
    test_builds_valid_bounded_config();
    test_rejects_unterminated_or_oversize_input_and_clears_output();
    test_rejects_null_inputs_without_writing();
    test_loads_compile_time_fixture();
    return failures == 0U ? 0 : 1;
}
