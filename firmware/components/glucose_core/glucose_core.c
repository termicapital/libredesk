#include "glucose_core.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void clear_reading(glucose_core_t *core)
{
    memset(&core->reading, 0, sizeof(core->reading));
    core->has_reading = false;
    core->last_monotonic_s = 0U;
    core->current_age_s = 0U;
}

static bool freshness_state(glucose_display_state_t state)
{
    return state == GLUCOSE_STATE_FRESH || state == GLUCOSE_STATE_AGING ||
           state == GLUCOSE_STATE_STALE || state == GLUCOSE_STATE_NO_DATA;
}

static void classify_core_freshness(glucose_core_t *core)
{
    glucose_display_state_t state;
    if (!core->has_reading) {
        core->state = GLUCOSE_STATE_NO_DATA;
        return;
    }
    if (!glucose_freshness_classify(core->current_age_s, &core->freshness, &state)) {
        core->state = GLUCOSE_STATE_SRV_ERROR;
        clear_reading(core);
        return;
    }
    core->state = state;
}

static glucose_display_state_t state_for_error(glucose_error_code_t code)
{
    switch (code) {
    case GLUCOSE_ERROR_INVALID_TOKEN:
    case GLUCOSE_ERROR_REVOKED_TOKEN:
    case GLUCOSE_ERROR_UPSTREAM_AUTH_FAILED:
    case GLUCOSE_ERROR_TERMS_OF_USE_REQUIRED:
        return GLUCOSE_STATE_AUTH_ERROR;
    case GLUCOSE_ERROR_UPSTREAM_UNAVAILABLE:
        return GLUCOSE_STATE_NO_DATA;
    default:
        return GLUCOSE_STATE_SRV_ERROR;
    }
}

static void set_parse_status(glucose_parse_status_t *output, glucose_parse_status_t status)
{
    if (output != NULL) {
        *output = status;
    }
}

bool glucose_core_init(glucose_core_t *core,
                       const glucose_freshness_config_t *freshness)
{
    if (core == NULL) {
        return false;
    }
    memset(core, 0, sizeof(*core));
    if (!glucose_freshness_config_is_valid(freshness)) {
        core->state = GLUCOSE_STATE_SRV_ERROR;
        return false;
    }
    core->freshness = *freshness;
    core->state = GLUCOSE_STATE_BOOT;
    return true;
}

glucose_ingest_result_t glucose_core_ingest_http(glucose_core_t *core,
                                                  unsigned int http_status,
                                                  const char *body,
                                                  size_t body_length,
                                                  uint64_t monotonic_now_s,
                                                  glucose_parse_status_t *parse_status_out)
{
    glucose_parse_status_t parse_status;

    if (core == NULL) {
        set_parse_status(parse_status_out, GLUCOSE_PARSE_NULL_ARGUMENT);
        return GLUCOSE_INGEST_MALFORMED;
    }

    if (http_status == 200U) {
        glucose_reading_t reading;
        parse_status = glucose_parse_latest_json(body, body_length, &reading);
        set_parse_status(parse_status_out, parse_status);
        if (parse_status != GLUCOSE_PARSE_OK) {
            clear_reading(core);
            core->retry_after_s = 0U;
            core->state = GLUCOSE_STATE_SRV_ERROR;
            return GLUCOSE_INGEST_MALFORMED;
        }
        core->reading = reading;
        core->has_reading = true;
        core->last_monotonic_s = monotonic_now_s;
        core->current_age_s = reading.age_s;
        core->retry_after_s = 0U;
        classify_core_freshness(core);
        return GLUCOSE_INGEST_UPDATED;
    }

    if (http_status == 304U) {
        clear_reading(core);
        core->retry_after_s = 0U;
        core->state = GLUCOSE_STATE_SRV_ERROR;
        set_parse_status(parse_status_out, GLUCOSE_PARSE_INVALID_VALUE);
        return GLUCOSE_INGEST_MALFORMED;
    }

    {
        glucose_error_payload_t error_payload;
        parse_status = glucose_parse_error_json(body, body_length, &error_payload);
        set_parse_status(parse_status_out, parse_status);
        clear_reading(core);
        if (parse_status != GLUCOSE_PARSE_OK) {
            core->retry_after_s = 0U;
            core->state = GLUCOSE_STATE_SRV_ERROR;
            return GLUCOSE_INGEST_MALFORMED;
        }
        core->retry_after_s = error_payload.retry_after_s;
        core->state = state_for_error(error_payload.code);
        return GLUCOSE_INGEST_ERROR_PAYLOAD;
    }
}

glucose_ingest_result_t glucose_core_ingest_not_modified(
    glucose_core_t *core,
    uint32_t server_age_s,
    uint64_t monotonic_now_s)
{
    if (core == NULL || !core->has_reading) {
        return GLUCOSE_INGEST_MALFORMED;
    }
    if (server_age_s > core->current_age_s) {
        core->current_age_s = server_age_s;
    }
    core->last_monotonic_s = monotonic_now_s;
    core->retry_after_s = 0U;
    classify_core_freshness(core);
    return GLUCOSE_INGEST_NOT_MODIFIED;
}

void glucose_core_tick(glucose_core_t *core, uint64_t monotonic_now_s)
{
    uint64_t elapsed;

    if (core == NULL || !core->has_reading || monotonic_now_s <= core->last_monotonic_s) {
        return;
    }
    elapsed = monotonic_now_s - core->last_monotonic_s;
    core->last_monotonic_s = monotonic_now_s;
    if (elapsed >= (uint64_t)UINT32_MAX - core->current_age_s) {
        core->current_age_s = UINT32_MAX;
    } else {
        core->current_age_s += (uint32_t)elapsed;
    }
    if (freshness_state(core->state)) {
        classify_core_freshness(core);
    }
}

bool glucose_core_set_operational_state(glucose_core_t *core,
                                        glucose_display_state_t state)
{
    bool allowed;

    if (core == NULL || (int)state < 0 || state >= GLUCOSE_STATE_COUNT) {
        return false;
    }
    allowed = state == GLUCOSE_STATE_BOOT || state == GLUCOSE_STATE_WIFI_CONNECTING ||
              state == GLUCOSE_STATE_TIME_SYNCING || state == GLUCOSE_STATE_FETCHING ||
              state == GLUCOSE_STATE_NO_DATA || state == GLUCOSE_STATE_AUTH_ERROR ||
              state == GLUCOSE_STATE_SRV_ERROR || state == GLUCOSE_STATE_NO_WIFI;
    if (!allowed) {
        return false;
    }
    if (state != GLUCOSE_STATE_NO_WIFI) {
        clear_reading(core);
    }
    core->state = state;
    return true;
}

static const char *status_text(glucose_display_state_t state)
{
    switch (state) {
    case GLUCOSE_STATE_BOOT:
        return "starting";
    case GLUCOSE_STATE_WIFI_CONNECTING:
        return "Wi-Fi";
    case GLUCOSE_STATE_TIME_SYNCING:
        return "Time";
    case GLUCOSE_STATE_FETCHING:
        return "...";
    case GLUCOSE_STATE_FRESH:
        return "fresh";
    case GLUCOSE_STATE_AGING:
        return "aging";
    case GLUCOSE_STATE_STALE:
        return "stale";
    case GLUCOSE_STATE_NO_DATA:
        return "no data";
    case GLUCOSE_STATE_AUTH_ERROR:
        return "AUTH";
    case GLUCOSE_STATE_SRV_ERROR:
        return "SRV";
    case GLUCOSE_STATE_NO_WIFI:
        return "no wifi";
    default:
        return "SRV";
    }
}

bool glucose_core_build_display(const glucose_core_t *core,
                                glucose_display_model_t *display_out)
{
    bool show_number;
    int written;

    if (display_out == NULL || core == NULL) {
        if (display_out != NULL) {
            memset(display_out, 0, sizeof(*display_out));
        }
        return false;
    }
    memset(display_out, 0, sizeof(*display_out));
    display_out->state = core->state;
    display_out->trend = GLUCOSE_TREND_UNKNOWN;
    display_out->range = GLUCOSE_RANGE_UNKNOWN;
    (void)snprintf(display_out->value_text, sizeof(display_out->value_text), "--");
    written = snprintf(display_out->status_text, sizeof(display_out->status_text),
                       "%s", status_text(core->state));
    if (written < 0 || (size_t)written >= sizeof(display_out->status_text)) {
        memset(display_out, 0, sizeof(*display_out));
        return false;
    }

    show_number = core->has_reading && glucose_state_allows_number(core->state);
    if (!show_number) {
        if (core->has_reading &&
            !glucose_format_age(core->current_age_s, display_out->age_text,
                                sizeof(display_out->age_text))) {
            memset(display_out, 0, sizeof(*display_out));
            return false;
        }
        return true;
    }

    if (!glucose_format_value(core->reading.value_mg_dl, core->reading.unit,
                              display_out->value_text, sizeof(display_out->value_text)) ||
        !glucose_format_age(core->current_age_s, display_out->age_text,
                            sizeof(display_out->age_text))) {
        memset(display_out, 0, sizeof(*display_out));
        (void)snprintf(display_out->value_text, sizeof(display_out->value_text), "--");
        return false;
    }
    written = snprintf(display_out->unit_text, sizeof(display_out->unit_text), "%s",
                       core->reading.unit == GLUCOSE_UNIT_MG_DL ? "mg/dL" : "mmol/L");
    if (written < 0 || (size_t)written >= sizeof(display_out->unit_text)) {
        memset(display_out, 0, sizeof(*display_out));
        (void)snprintf(display_out->value_text, sizeof(display_out->value_text), "--");
        return false;
    }
    display_out->show_number = true;
    display_out->use_range_hue = glucose_state_allows_range_hue(core->state);
    display_out->trend = core->reading.trend;
    display_out->range = core->reading.range;
    return true;
}
