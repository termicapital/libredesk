#include "glucose_core.h"

#include <math.h>

static const char *const trend_names[GLUCOSE_TREND_COUNT] = {
    "rising",
    "rising_slightly",
    "stable",
    "falling_slightly",
    "falling",
    "rising_fast",
    "falling_fast",
    "unknown"
};

static const char *const range_names[GLUCOSE_RANGE_COUNT] = {
    "urgent_low",
    "low",
    "in_range",
    "high",
    "unknown"
};

static const char *const state_names[GLUCOSE_STATE_COUNT] = {
    "boot",
    "wifi",
    "time",
    "fetch",
    "fresh",
    "aging",
    "stale",
    "no_data",
    "auth",
    "server",
    "no_wifi"
};

static const char *const error_names[GLUCOSE_ERROR_CODE_COUNT] = {
    "invalid_token",
    "revoked_token",
    "rate_limited",
    "upstream_unavailable",
    "upstream_auth_failed",
    "internal",
    "malformed_upstream",
    "terms_of_use_required",
    "invalid_payload",
    "ota_unavailable"
};

static const char *const parse_status_names[GLUCOSE_PARSE_STATUS_COUNT] = {
    "ok",
    "null_argument",
    "invalid_json",
    "missing_field",
    "duplicate_field",
    "unknown_field",
    "invalid_type",
    "invalid_value",
    "unsupported_schema",
    "time_mismatch"
};

glucose_trend_t glucose_trend_from_libre_arrow(int trend_arrow)
{
    switch (trend_arrow) {
    case 1:
        return GLUCOSE_TREND_FALLING;
    case 2:
        return GLUCOSE_TREND_FALLING_SLIGHTLY;
    case 3:
        return GLUCOSE_TREND_STABLE;
    case 4:
        return GLUCOSE_TREND_RISING_SLIGHTLY;
    case 5:
        return GLUCOSE_TREND_RISING;
    default:
        return GLUCOSE_TREND_UNKNOWN;
    }
}

const char *glucose_trend_name(glucose_trend_t trend)
{
    if ((int)trend < 0 || trend >= GLUCOSE_TREND_COUNT) {
        return trend_names[GLUCOSE_TREND_UNKNOWN];
    }
    return trend_names[trend];
}

const char *glucose_range_name(glucose_range_t range)
{
    if ((int)range < 0 || range >= GLUCOSE_RANGE_COUNT) {
        return range_names[GLUCOSE_RANGE_UNKNOWN];
    }
    return range_names[range];
}

const char *glucose_error_code_name(glucose_error_code_t code)
{
    if ((int)code < 0 || code >= GLUCOSE_ERROR_CODE_COUNT) {
        return "unknown";
    }
    return error_names[code];
}

const char *glucose_state_name(glucose_display_state_t state)
{
    if ((int)state < 0 || state >= GLUCOSE_STATE_COUNT) {
        return "unknown";
    }
    return state_names[state];
}

const char *glucose_parse_status_name(glucose_parse_status_t status)
{
    if ((int)status < 0 || status >= GLUCOSE_PARSE_STATUS_COUNT) {
        return "unknown";
    }
    return parse_status_names[status];
}

bool glucose_range_classify_mg_dl(double glucose_mg_dl,
                                  double urgent_low_mg_dl,
                                  double target_low_mg_dl,
                                  double target_high_mg_dl,
                                  glucose_range_t *range_out)
{
    if (range_out == NULL) {
        return false;
    }
    *range_out = GLUCOSE_RANGE_UNKNOWN;
    if (!isfinite(glucose_mg_dl) || !isfinite(urgent_low_mg_dl) ||
        !isfinite(target_low_mg_dl) || !isfinite(target_high_mg_dl) ||
        glucose_mg_dl < 20.0 || glucose_mg_dl > 500.0 ||
        urgent_low_mg_dl < 40.0 || urgent_low_mg_dl > 100.0 ||
        urgent_low_mg_dl > target_low_mg_dl ||
        target_low_mg_dl > target_high_mg_dl || target_high_mg_dl > 500.0) {
        return false;
    }
    if (glucose_mg_dl <= urgent_low_mg_dl) {
        *range_out = GLUCOSE_RANGE_URGENT_LOW;
    } else if (glucose_mg_dl < target_low_mg_dl) {
        *range_out = GLUCOSE_RANGE_LOW;
    } else if (glucose_mg_dl <= target_high_mg_dl) {
        *range_out = GLUCOSE_RANGE_IN_RANGE;
    } else {
        *range_out = GLUCOSE_RANGE_HIGH;
    }
    return true;
}

bool glucose_freshness_config_is_valid(const glucose_freshness_config_t *config)
{
    return config != NULL && config->fresh_max_s > 0U &&
           config->fresh_max_s <= config->aging_max_s &&
           config->aging_max_s <= config->stale_max_s;
}

bool glucose_freshness_classify(uint32_t age_s,
                                const glucose_freshness_config_t *config,
                                glucose_display_state_t *state_out)
{
    if (state_out == NULL) {
        return false;
    }
    if (!glucose_freshness_config_is_valid(config)) {
        *state_out = GLUCOSE_STATE_SRV_ERROR;
        return false;
    }
    if (age_s <= config->fresh_max_s) {
        *state_out = GLUCOSE_STATE_FRESH;
    } else if (age_s <= config->aging_max_s) {
        *state_out = GLUCOSE_STATE_AGING;
    } else if (age_s <= config->stale_max_s) {
        *state_out = GLUCOSE_STATE_STALE;
    } else {
        *state_out = GLUCOSE_STATE_NO_DATA;
    }
    return true;
}

bool glucose_state_allows_number(glucose_display_state_t state)
{
    return state == GLUCOSE_STATE_FRESH || state == GLUCOSE_STATE_AGING;
}

bool glucose_state_allows_range_hue(glucose_display_state_t state)
{
    return glucose_state_allows_number(state);
}
