#include "mvp_http_flow.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char latest_body[] =
    "{\"glucose\":123,\"unit\":\"mg/dL\",\"trend\":\"stable\","
    "\"range\":\"in_range\",\"target_low\":70,\"target_high\":180,"
    "\"timestamp\":\"2026-08-24T09:15:00Z\","
    "\"libre_local_time\":\"11:15:00\",\"age_s\":10,"
    "\"server_time\":\"2026-08-24T09:15:10Z\",\"source\":\"mock\","
    "\"poll_after_s\":60,\"schema\":2}";

static const char auth_error_body[] =
    "{\"error\":\"invalid_token\",\"detail\":\"denied\",\"retry_after_s\":0}";

static glucose_core_t core(void)
{
    glucose_core_t value;
    const glucose_freshness_config_t freshness = { 60U, 120U, 300U };
    assert(glucose_core_init(&value, &freshness));
    return value;
}

static runtime_http_exchange_t exchange(unsigned status, const char *body)
{
    runtime_http_exchange_t value;
    runtime_http_exchange_init(&value);
    assert(runtime_http_exchange_set_status(&value, status));
    if (status != 304U) {
        assert(runtime_http_exchange_append_body(&value, (const uint8_t *)body, strlen(body)));
    }
    return value;
}

static void expect_neutral(const runtime_ui_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    assert(runtime_ui_snapshot_is_valid(snapshot));
    assert(!snapshot->model.show_number);
    assert(snapshot->model.state != GLUCOSE_STATE_FRESH);
    assert(snapshot->model.state != GLUCOSE_STATE_AGING);
}

static void test_fetch_and_valid_200(void)
{
    glucose_core_t value = core();
    runtime_ui_snapshot_t snapshot;
    runtime_http_exchange_t response = exchange(200U, latest_body);
    assert(mvp_http_flow_publish_fetching(&value, &snapshot));
    assert(snapshot.model.state == GLUCOSE_STATE_FETCHING);
    assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &snapshot));
    assert(runtime_ui_snapshot_is_valid(&snapshot));
    assert(snapshot.model.show_number);
    assert(strcmp(snapshot.model.value_text, "123") == 0);
}

static void test_esp_idf_callback_order_captures_before_status(void)
{
    static const char etag[] = "\"0123456789abcdef\"";
    glucose_core_t value = core();
    runtime_http_exchange_t response;
    runtime_ui_snapshot_t snapshot;

    runtime_http_exchange_init(&response);
    assert(runtime_http_exchange_add_header(&response,
                                            "etag", sizeof("etag"),
                                            etag, sizeof(etag)));
    assert(runtime_http_exchange_append_body(&response,
                                             (const uint8_t *)latest_body,
                                             strlen(latest_body)));
    assert(runtime_http_exchange_set_status(&response, 200U));
    assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &snapshot));
    assert(runtime_ui_snapshot_is_valid(&snapshot));
    assert(snapshot.model.show_number);
    assert(strcmp(snapshot.model.value_text, "123") == 0);
}

static void test_late_status_revalidates_captured_body(void)
{
    runtime_http_exchange_t response;
    uint8_t error_body[GLUCOSE_ERROR_JSON_MAX_BYTES + 1U];

    memset(error_body, 'x', sizeof(error_body));
    runtime_http_exchange_init(&response);
    assert(runtime_http_exchange_append_body(&response, error_body,
                                             sizeof(error_body)));
    assert(!runtime_http_exchange_set_status(&response, 500U));
    assert(runtime_http_exchange_finish(&response) ==
           RUNTIME_HTTP_RESULT_MALFORMED);

    runtime_http_exchange_init(&response);
    assert(runtime_http_exchange_append_body(&response,
                                             (const uint8_t *)"x", 1U));
    assert(!runtime_http_exchange_set_status(&response, 304U));
    assert(runtime_http_exchange_finish(&response) ==
           RUNTIME_HTTP_RESULT_MALFORMED);
}

static void test_rejected_responses_are_nonnumeric(void)
{
    const unsigned statuses[] = { 401U, 403U, 500U, 418U, 304U, 302U, 204U };
    glucose_core_t value = core();
    runtime_ui_snapshot_t snapshot;
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]); index++) {
        runtime_http_exchange_t response;
        const char *body = (statuses[index] == 401U || statuses[index] == 403U) ? auth_error_body : "{}";
        response = exchange(statuses[index], body);
        assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &snapshot));
        expect_neutral(&snapshot);
        if (statuses[index] == 401U || statuses[index] == 403U) {
            assert(snapshot.model.state == GLUCOSE_STATE_AUTH_ERROR);
        }
    }
    assert(mvp_http_flow_apply_transport_failure(&value, &snapshot));
    expect_neutral(&snapshot);
}

static void test_malformed_and_overflow_fail_closed(void)
{
    glucose_core_t value = core();
    runtime_ui_snapshot_t snapshot;
    runtime_http_exchange_t response = exchange(200U, "{\"glucose\":123");
    uint8_t body[RUNTIME_HTTP_BODY_CAPACITY];
    memset(body, 'x', sizeof(body));
    assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &snapshot));
    expect_neutral(&snapshot);
    runtime_http_exchange_init(&response);
    assert(runtime_http_exchange_set_status(&response, 200U));
    assert(runtime_http_exchange_append_body(&response, body, sizeof(body)));
    assert(!runtime_http_exchange_append_body(&response, body, 1U));
    assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &snapshot));
    expect_neutral(&snapshot);
    assert(!mvp_http_flow_apply_exchange(&value, NULL, 100U, &snapshot));
    expect_neutral(&snapshot);
}

static void test_tick_crosses_freshness_boundaries(void)
{
    glucose_core_t value = core();
    runtime_ui_snapshot_t first;
    runtime_ui_snapshot_t second;
    runtime_http_exchange_t response = exchange(200U, latest_body);
    assert(mvp_http_flow_apply_exchange(&value, &response, 100U, &first));
    assert(first.model.state == GLUCOSE_STATE_FRESH);
    assert(mvp_http_flow_tick(&value, 151U, &second));
    assert(second.model.state == GLUCOSE_STATE_AGING);
    assert(mvp_http_flow_tick(&value, 221U, &second));
    assert(second.model.state == GLUCOSE_STATE_STALE);
    assert(!second.model.show_number);
    assert(mvp_http_flow_tick(&value, 401U, &second));
    expect_neutral(&second);
    assert(!mvp_http_flow_tick(NULL, 1U, &second));
}

int main(void)
{
    test_fetch_and_valid_200();
    test_esp_idf_callback_order_captures_before_status();
    test_late_status_revalidates_captured_body();
    test_rejected_responses_are_nonnumeric();
    test_malformed_and_overflow_fail_closed();
    test_tick_crosses_freshness_boundaries();
    puts("mvp http flow tests: PASS");
    return 0;
}
