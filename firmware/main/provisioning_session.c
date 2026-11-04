#include "provisioning_session.h"

#include <string.h>

bool provisioning_runtime_config_equal(const runtime_config_t *left,
                                       const runtime_config_t *right)
{
    if (left == NULL || right == NULL ||
        !runtime_config_validate(left, NULL) ||
        !runtime_config_validate(right, NULL)) {
        return false;
    }
    return strcmp(left->wifi_ssid, right->wifi_ssid) == 0 &&
           strcmp(left->wifi_password, right->wifi_password) == 0 &&
           strcmp(left->bridge_origin, right->bridge_origin) == 0 &&
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

provisioning_session_result_t provisioning_session_apply(
    const runtime_config_t *current,
    const wifi_provisioning_submission_t *submission,
    const provisioning_session_ops_t *ops)
{
    runtime_config_t candidate;
    runtime_config_t reloaded;
    config_store_result_t save_result;
    config_store_result_t reload_result;
    provisioning_session_result_t result;

    memset(&candidate, 0, sizeof(candidate));
    memset(&reloaded, 0, sizeof(reloaded));
    if (current == NULL || submission == NULL || ops == NULL ||
        ops->connect_candidate == NULL || ops->save_candidate == NULL ||
        ops->reload_active == NULL || ops->clear_setup_request == NULL ||
        ops->restart == NULL) {
        return PROVISIONING_SESSION_RESULT_BAD_ARGUMENT;
    }
    if (!wifi_provisioning_candidate_build(current, submission, &candidate)) {
        return PROVISIONING_SESSION_RESULT_INVALID_CANDIDATE;
    }
    if (!ops->connect_candidate(&candidate, ops->context)) {
        result = PROVISIONING_SESSION_RESULT_NO_IP;
        goto cleanup;
    }

    save_result = ops->save_candidate(&candidate, ops->context);
    reload_result = ops->reload_active(&reloaded, ops->context);
    if (save_result != CONFIG_STORE_RESULT_OK) {
        if (reload_result != CONFIG_STORE_RESULT_OK ||
            !runtime_config_validate(&reloaded, NULL)) {
            result = PROVISIONING_SESSION_RESULT_SAVE_UNCERTAIN;
            goto cleanup;
        }
        if (provisioning_runtime_config_equal(&reloaded, current)) {
            result = PROVISIONING_SESSION_RESULT_SAVE_FAILED;
            goto cleanup;
        }
        if (!provisioning_runtime_config_equal(&reloaded, &candidate)) {
            result = PROVISIONING_SESSION_RESULT_SAVE_UNCERTAIN;
            goto cleanup;
        }
    } else {
        if (reload_result != CONFIG_STORE_RESULT_OK) {
            result = PROVISIONING_SESSION_RESULT_RELOAD_FAILED;
            goto cleanup;
        }
        if (!provisioning_runtime_config_equal(&reloaded, &candidate)) {
            result = PROVISIONING_SESSION_RESULT_VERIFY_FAILED;
            goto cleanup;
        }
    }

    if (ops->clear_setup_request(ops->context) != CONFIG_STORE_RESULT_OK) {
        result = PROVISIONING_SESSION_RESULT_MARKER_CLEAR_FAILED;
        goto cleanup;
    }
    ops->restart(ops->context);
    result = PROVISIONING_SESSION_RESULT_OK;

cleanup:
    memset(&candidate, 0, sizeof(candidate));
    memset(&reloaded, 0, sizeof(reloaded));
    return result;
}
