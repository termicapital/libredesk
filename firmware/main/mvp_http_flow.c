#include "mvp_http_flow.h"

#include <string.h>

static bool build_snapshot(const glucose_core_t *core,
                           runtime_ui_snapshot_t *snapshot_out)
{
    if (snapshot_out == NULL) {
        return false;
    }
    runtime_ui_snapshot_make_neutral(snapshot_out);
    if (core == NULL || !runtime_ui_snapshot_build(core, snapshot_out) ||
        !runtime_ui_snapshot_is_valid(snapshot_out)) {
        runtime_ui_snapshot_make_neutral(snapshot_out);
        return false;
    }
    return true;
}

static bool set_state_and_build(glucose_core_t *core,
                                glucose_display_state_t state,
                                runtime_ui_snapshot_t *snapshot_out)
{
    if (core == NULL || !glucose_core_set_operational_state(core, state)) {
        if (snapshot_out != NULL) runtime_ui_snapshot_make_neutral(snapshot_out);
        return false;
    }
    return build_snapshot(core, snapshot_out);
}

bool mvp_http_flow_publish_fetching(glucose_core_t *core,
                                    runtime_ui_snapshot_t *snapshot_out)
{
    return set_state_and_build(core, GLUCOSE_STATE_FETCHING, snapshot_out);
}

bool mvp_http_flow_apply_exchange(glucose_core_t *core,
                                  const runtime_http_exchange_t *exchange,
                                  uint64_t monotonic_now_s,
                                  runtime_ui_snapshot_t *snapshot_out)
{
    runtime_http_result_t result;
    runtime_http_exchange_t completed;
    glucose_parse_status_t parse_status;

    if (core == NULL || exchange == NULL || snapshot_out == NULL) {
        if (snapshot_out != NULL) runtime_ui_snapshot_make_neutral(snapshot_out);
        return false;
    }
    completed = *exchange;
    result = runtime_http_exchange_finish(&completed);
    if (result == RUNTIME_HTTP_RESULT_BODY || result == RUNTIME_HTTP_RESULT_ERROR_BODY) {
        (void)glucose_core_ingest_http(core, exchange->status,
                                       (const char *)exchange->body,
                                       exchange->body_length,
                                       monotonic_now_s, &parse_status);
        return build_snapshot(core, snapshot_out);
    }
    return set_state_and_build(core, GLUCOSE_STATE_SRV_ERROR, snapshot_out);
}

bool mvp_http_flow_apply_transport_failure(glucose_core_t *core,
                                           runtime_ui_snapshot_t *snapshot_out)
{
    return set_state_and_build(core, GLUCOSE_STATE_SRV_ERROR, snapshot_out);
}

bool mvp_http_flow_tick(glucose_core_t *core,
                        uint64_t monotonic_now_s,
                        runtime_ui_snapshot_t *snapshot_out)
{
    if (core == NULL || snapshot_out == NULL) {
        if (snapshot_out != NULL) runtime_ui_snapshot_make_neutral(snapshot_out);
        return false;
    }
    glucose_core_tick(core, monotonic_now_s);
    return build_snapshot(core, snapshot_out);
}
