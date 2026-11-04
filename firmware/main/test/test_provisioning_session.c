#include "provisioning_session.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    return false; } } while (0)

typedef struct fake_session {
    bool connect_ok;
    config_store_result_t save_result;
    config_store_result_t reload_result;
    config_store_result_t clear_result;
    runtime_config_t reload_config;
    runtime_config_t seen_candidate;
    char order[16];
    size_t order_length;
    unsigned restarts;
} fake_session_t;

static runtime_config_t valid_config(const char *ssid, const char *password)
{
    runtime_config_t config;
    memset(&config, 0, sizeof(config));
    (void)strcpy(config.wifi_ssid, ssid);
    (void)strcpy(config.wifi_password, password);
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

static wifi_provisioning_submission_t submission(void)
{
    wifi_provisioning_submission_t value;
    memset(&value, 0, sizeof(value));
    (void)strcpy(value.ssid, "new-network");
    (void)strcpy(value.password, "new-password");
    return value;
}

static void record(fake_session_t *fake, char operation)
{
    if (fake->order_length + 1U < sizeof(fake->order)) {
        fake->order[fake->order_length++] = operation;
        fake->order[fake->order_length] = '\0';
    }
}

static bool connect_candidate(const runtime_config_t *candidate, void *context)
{
    fake_session_t *fake = context;
    record(fake, 'C');
    fake->seen_candidate = *candidate;
    return fake->connect_ok;
}

static config_store_result_t save_candidate(const runtime_config_t *candidate,
                                             void *context)
{
    fake_session_t *fake = context;
    record(fake, 'S');
    fake->seen_candidate = *candidate;
    return fake->save_result;
}

static config_store_result_t reload_active(runtime_config_t *out, void *context)
{
    fake_session_t *fake = context;
    record(fake, 'L');
    if (fake->reload_result == CONFIG_STORE_RESULT_OK) *out = fake->reload_config;
    return fake->reload_result;
}

static config_store_result_t clear_request(void *context)
{
    fake_session_t *fake = context;
    record(fake, 'M');
    return fake->clear_result;
}

static void restart(void *context)
{
    fake_session_t *fake = context;
    record(fake, 'R');
    fake->restarts++;
}

static provisioning_session_ops_t ops(fake_session_t *fake)
{
    provisioning_session_ops_t value = {
        .connect_candidate = connect_candidate,
        .save_candidate = save_candidate,
        .reload_active = reload_active,
        .clear_setup_request = clear_request,
        .restart = restart,
        .context = fake,
    };
    return value;
}

static fake_session_t successful_fake(const runtime_config_t *candidate)
{
    fake_session_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.connect_ok = true;
    fake.save_result = CONFIG_STORE_RESULT_OK;
    fake.reload_result = CONFIG_STORE_RESULT_OK;
    fake.clear_result = CONFIG_STORE_RESULT_OK;
    fake.reload_config = *candidate;
    return fake;
}

static bool test_exact_success_order_and_wifi_only_candidate(void)
{
    runtime_config_t current = valid_config("old-network", "old-password");
    wifi_provisioning_submission_t input = submission();
    runtime_config_t candidate;
    fake_session_t fake;
    provisioning_session_ops_t value;

    CHECK(wifi_provisioning_candidate_build(&current, &input, &candidate));
    fake = successful_fake(&candidate);
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_OK);
    CHECK(strcmp(fake.order, "CSLMR") == 0);
    CHECK(fake.restarts == 1U);
    CHECK(provisioning_runtime_config_equal(&fake.seen_candidate, &candidate));
    CHECK(strcmp(fake.seen_candidate.bridge_origin, current.bridge_origin) == 0);
    CHECK(strcmp(fake.seen_candidate.device_token, current.device_token) == 0);
    return true;
}

static bool test_no_ip_never_saves(void)
{
    runtime_config_t current = valid_config("old-network", "old-password");
    wifi_provisioning_submission_t input = submission();
    runtime_config_t candidate;
    fake_session_t fake;
    provisioning_session_ops_t value;

    CHECK(wifi_provisioning_candidate_build(&current, &input, &candidate));
    fake = successful_fake(&candidate);
    fake.connect_ok = false;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_NO_IP);
    CHECK(strcmp(fake.order, "C") == 0 && fake.restarts == 0U);
    return true;
}

static bool test_save_error_is_classified_by_reload(void)
{
    runtime_config_t current = valid_config("old-network", "old-password");
    wifi_provisioning_submission_t input = submission();
    runtime_config_t candidate;
    runtime_config_t mixed;
    fake_session_t fake;
    provisioning_session_ops_t value;

    CHECK(wifi_provisioning_candidate_build(&current, &input, &candidate));

    fake = successful_fake(&candidate);
    fake.save_result = CONFIG_STORE_RESULT_IO_ERROR;
    fake.reload_config = current;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_SAVE_FAILED);
    CHECK(strcmp(fake.order, "CSL") == 0 && fake.restarts == 0U);

    fake = successful_fake(&candidate);
    fake.save_result = CONFIG_STORE_RESULT_IO_ERROR;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_OK);
    CHECK(strcmp(fake.order, "CSLMR") == 0 && fake.restarts == 1U);

    mixed = current;
    (void)strcpy(mixed.wifi_ssid, "third-network");
    fake = successful_fake(&candidate);
    fake.save_result = CONFIG_STORE_RESULT_IO_ERROR;
    fake.reload_config = mixed;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_SAVE_UNCERTAIN);
    CHECK(strcmp(fake.order, "CSL") == 0 && fake.restarts == 0U);
    return true;
}

static bool test_reload_verify_and_marker_failures_never_restart(void)
{
    runtime_config_t current = valid_config("old-network", "old-password");
    wifi_provisioning_submission_t input = submission();
    runtime_config_t candidate;
    fake_session_t fake;
    provisioning_session_ops_t value;

    CHECK(wifi_provisioning_candidate_build(&current, &input, &candidate));

    fake = successful_fake(&candidate);
    fake.reload_result = CONFIG_STORE_RESULT_IO_ERROR;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_RELOAD_FAILED);
    CHECK(strcmp(fake.order, "CSL") == 0 && fake.restarts == 0U);

    fake = successful_fake(&candidate);
    fake.reload_config = current;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_VERIFY_FAILED);
    CHECK(strcmp(fake.order, "CSL") == 0 && fake.restarts == 0U);

    fake = successful_fake(&candidate);
    fake.clear_result = CONFIG_STORE_RESULT_IO_ERROR;
    value = ops(&fake);
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_MARKER_CLEAR_FAILED);
    CHECK(strcmp(fake.order, "CSLM") == 0 && fake.restarts == 0U);
    return true;
}

static bool test_bad_arguments_and_invalid_candidate_do_nothing(void)
{
    runtime_config_t current = valid_config("old-network", "old-password");
    wifi_provisioning_submission_t input = submission();
    runtime_config_t candidate;
    fake_session_t fake;
    provisioning_session_ops_t value;

    CHECK(wifi_provisioning_candidate_build(&current, &input, &candidate));
    fake = successful_fake(&candidate);
    value = ops(&fake);
    CHECK(provisioning_session_apply(NULL, &input, &value) ==
          PROVISIONING_SESSION_RESULT_BAD_ARGUMENT);
    CHECK(provisioning_session_apply(&current, NULL, &value) ==
          PROVISIONING_SESSION_RESULT_BAD_ARGUMENT);
    CHECK(provisioning_session_apply(&current, &input, NULL) ==
          PROVISIONING_SESSION_RESULT_BAD_ARGUMENT);
    CHECK(fake.order_length == 0U);

    input.ssid[0] = '\0';
    CHECK(provisioning_session_apply(&current, &input, &value) ==
          PROVISIONING_SESSION_RESULT_INVALID_CANDIDATE);
    CHECK(fake.order_length == 0U);
    return true;
}

int main(void)
{
    if (!test_exact_success_order_and_wifi_only_candidate() ||
        !test_no_ip_never_saves() ||
        !test_save_error_is_classified_by_reload() ||
        !test_reload_verify_and_marker_failures_never_restart() ||
        !test_bad_arguments_and_invalid_candidate_do_nothing()) {
        return 1;
    }
    puts("provisioning session tests passed");
    return 0;
}
