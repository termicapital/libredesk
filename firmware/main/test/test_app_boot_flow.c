#include "app_boot_flow.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    return false; } } while (0)

typedef struct fake_context {
    runtime_config_t embedded;
    runtime_config_t persisted;
    bool embedded_ok;
    config_store_result_t persisted_result;
    config_store_result_t marker_result;
    bool marker;
    unsigned embedded_calls;
    unsigned persisted_calls;
    unsigned marker_calls;
} fake_context_t;

static runtime_config_t make_config(const char *ssid)
{
    runtime_config_t config;
    memset(&config, 0, sizeof(config));
    (void)strcpy(config.wifi_ssid, ssid);
    (void)strcpy(config.wifi_password, "valid-password");
    (void)strcpy(config.bridge_origin, "https://bridge.example.test");
    (void)strcpy(config.device_token,
                 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE");
    (void)strcpy(config.tz, "EST5");
    config.unit = GLUCOSE_UNIT_MG_DL;
    config.brightness_day_percent = 75U;
    config.brightness_night_percent = 25U;
    config.night_start_minute = 1320U;
    config.night_end_minute = 420U;
    config.freshness.fresh_max_s = 360U;
    config.freshness.aging_max_s = 900U;
    config.freshness.stale_max_s = 3600U;
    config.poll_s = 20U;
    config.ota_enabled = 1U;
    return config;
}

static bool load_embedded(runtime_config_t *out, void *context)
{
    fake_context_t *fake = context;
    fake->embedded_calls++;
    if (!fake->embedded_ok) return false;
    *out = fake->embedded;
    return true;
}

static config_store_result_t load_persisted(runtime_config_t *out, void *context)
{
    fake_context_t *fake = context;
    fake->persisted_calls++;
    if (fake->persisted_result == CONFIG_STORE_RESULT_OK) *out = fake->persisted;
    return fake->persisted_result;
}

static config_store_result_t get_marker(bool *out, void *context)
{
    fake_context_t *fake = context;
    fake->marker_calls++;
    if (fake->marker_result == CONFIG_STORE_RESULT_OK) *out = fake->marker;
    return fake->marker_result;
}

static app_boot_flow_ops_t ops(fake_context_t *fake)
{
    app_boot_flow_ops_t value = {
        .load_embedded = load_embedded,
        .load_persisted = load_persisted,
        .get_setup_request = get_marker,
        .context = fake,
    };
    return value;
}

static fake_context_t valid_fake(void)
{
    fake_context_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.embedded = make_config("embedded-network");
    fake.persisted = make_config("persisted-network");
    fake.embedded_ok = true;
    fake.persisted_result = CONFIG_STORE_RESULT_ABSENT;
    fake.marker_result = CONFIG_STORE_RESULT_OK;
    return fake;
}

static bool outputs_are_cleared(const runtime_config_t *config, app_boot_mode_t mode)
{
    runtime_config_t zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(config, &zero, sizeof(zero)) == 0 && mode == APP_BOOT_MODE_NORMAL;
}

static bool test_argument_and_embedded_failures(void)
{
    fake_context_t fake = valid_fake();
    app_boot_flow_ops_t value = ops(&fake);
    runtime_config_t out;
    app_boot_mode_t mode;

    memset(&out, 0xa5, sizeof(out)); mode = APP_BOOT_MODE_SETUP;
    CHECK(app_boot_flow_select(NULL, &out, &mode) == APP_BOOT_RESULT_BAD_ARGUMENT);
    CHECK(outputs_are_cleared(&out, mode));
    CHECK(app_boot_flow_select(&value, NULL, &mode) == APP_BOOT_RESULT_BAD_ARGUMENT);
    CHECK(app_boot_flow_select(&value, &out, NULL) == APP_BOOT_RESULT_BAD_ARGUMENT);

    fake.embedded_ok = false;
    memset(&out, 0xa5, sizeof(out)); mode = APP_BOOT_MODE_SETUP;
    CHECK(app_boot_flow_select(&value, &out, &mode) ==
          APP_BOOT_RESULT_EMBEDDED_CONFIG_INVALID);
    CHECK(outputs_are_cleared(&out, mode));
    CHECK(fake.persisted_calls == 0U && fake.marker_calls == 0U);

    fake = valid_fake(); value = ops(&fake);
    fake.embedded.wifi_ssid[0] = '\0';
    CHECK(app_boot_flow_select(&value, &out, &mode) ==
          APP_BOOT_RESULT_EMBEDDED_CONFIG_INVALID);
    CHECK(outputs_are_cleared(&out, mode));
    return true;
}

static bool test_absent_persisted_uses_embedded_and_marker_selects_mode(void)
{
    fake_context_t fake = valid_fake();
    app_boot_flow_ops_t value = ops(&fake);
    runtime_config_t out;
    app_boot_mode_t mode;

    CHECK(app_boot_flow_select(&value, &out, &mode) == APP_BOOT_RESULT_OK);
    CHECK(mode == APP_BOOT_MODE_NORMAL);
    CHECK(strcmp(out.wifi_ssid, "embedded-network") == 0);

    fake.marker = true;
    CHECK(app_boot_flow_select(&value, &out, &mode) == APP_BOOT_RESULT_OK);
    CHECK(mode == APP_BOOT_MODE_SETUP);
    CHECK(strcmp(out.wifi_ssid, "embedded-network") == 0);
    return true;
}

static bool test_persisted_record_preserves_user_fields_and_refreshes_embedded_secrets(void)
{
    fake_context_t fake = valid_fake();
    app_boot_flow_ops_t value = ops(&fake);
    runtime_config_t expected;
    runtime_config_t out;
    app_boot_mode_t mode;

    (void)strcpy(fake.embedded.bridge_origin, "https://current.example.test");
    (void)strcpy(fake.embedded.device_token,
                 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE");
    (void)strcpy(fake.persisted.bridge_origin, "https://retired.example.test");
    (void)strcpy(fake.persisted.device_token,
                 "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBE");
    fake.persisted.brightness_day_percent = 42U;
    expected = fake.persisted;
    (void)strcpy(expected.bridge_origin, fake.embedded.bridge_origin);
    (void)strcpy(expected.device_token, fake.embedded.device_token);

    fake.persisted_result = CONFIG_STORE_RESULT_OK;
    fake.marker = true;
    CHECK(app_boot_flow_select(&value, &out, &mode) == APP_BOOT_RESULT_OK);
    CHECK(mode == APP_BOOT_MODE_SETUP);
    CHECK(memcmp(&out, &expected, sizeof(out)) == 0);
    CHECK(strcmp(out.wifi_ssid, "persisted-network") == 0);
    CHECK(out.brightness_day_percent == 42U);
    CHECK(strcmp(out.bridge_origin, "https://current.example.test") == 0);
    CHECK(strcmp(out.device_token, fake.embedded.device_token) == 0);
    CHECK(fake.embedded_calls == 1U && fake.persisted_calls == 1U &&
          fake.marker_calls == 1U);
    return true;
}

static bool test_persisted_and_marker_errors_fail_closed(void)
{
    static const config_store_result_t persisted_failures[] = {
        CONFIG_STORE_RESULT_INVALID, CONFIG_STORE_RESULT_IO_ERROR,
        CONFIG_STORE_RESULT_BAD_ARGUMENT, CONFIG_STORE_RESULT_COUNT,
    };
    static const config_store_result_t marker_failures[] = {
        CONFIG_STORE_RESULT_ABSENT, CONFIG_STORE_RESULT_INVALID,
        CONFIG_STORE_RESULT_IO_ERROR, CONFIG_STORE_RESULT_BAD_ARGUMENT,
        CONFIG_STORE_RESULT_COUNT,
    };
    size_t index;

    for (index = 0U; index < sizeof(persisted_failures) / sizeof(persisted_failures[0]); ++index) {
        fake_context_t fake = valid_fake();
        app_boot_flow_ops_t value = ops(&fake);
        runtime_config_t out;
        app_boot_mode_t mode = APP_BOOT_MODE_SETUP;
        fake.persisted_result = persisted_failures[index];
        memset(&out, 0xa5, sizeof(out));
        CHECK(app_boot_flow_select(&value, &out, &mode) ==
              (persisted_failures[index] == CONFIG_STORE_RESULT_INVALID
                   ? APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID
                   : APP_BOOT_RESULT_IO_ERROR));
        CHECK(outputs_are_cleared(&out, mode));
        CHECK(fake.marker_calls == 0U);
    }

    for (index = 0U; index < sizeof(marker_failures) / sizeof(marker_failures[0]); ++index) {
        fake_context_t fake = valid_fake();
        app_boot_flow_ops_t value = ops(&fake);
        runtime_config_t out;
        app_boot_mode_t mode = APP_BOOT_MODE_SETUP;
        fake.marker_result = marker_failures[index];
        memset(&out, 0xa5, sizeof(out));
        CHECK(app_boot_flow_select(&value, &out, &mode) ==
              (marker_failures[index] == CONFIG_STORE_RESULT_INVALID
                   ? APP_BOOT_RESULT_PERSISTED_CONFIG_INVALID
                   : APP_BOOT_RESULT_IO_ERROR));
        CHECK(outputs_are_cleared(&out, mode));
    }
    return true;
}

int main(void)
{
    if (!test_argument_and_embedded_failures() ||
        !test_absent_persisted_uses_embedded_and_marker_selects_mode() ||
        !test_persisted_record_preserves_user_fields_and_refreshes_embedded_secrets() ||
        !test_persisted_and_marker_errors_fail_closed()) {
        return 1;
    }
    puts("app boot flow tests passed");
    return 0;
}
