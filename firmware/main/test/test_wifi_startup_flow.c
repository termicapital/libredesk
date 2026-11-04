#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "wifi_startup_flow.h"

static void test_start_connects(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(flow.phase == WIFI_STARTUP_PHASE_IDLE);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(flow.phase == WIFI_STARTUP_PHASE_WIFI_CONNECTING);
    assert(flow.display_state == GLUCOSE_STATE_WIFI_CONNECTING);
}

static void test_ip_then_sync_then_ready(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(flow.phase == WIFI_STARTUP_PHASE_TIME_SYNCING);
    assert(flow.display_state == GLUCOSE_STATE_TIME_SYNCING);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_SYNCED));
    assert(flow.phase == WIFI_STARTUP_PHASE_READY);
    assert(!wifi_startup_flow_has_display_state(&flow, GLUCOSE_STATE_NO_WIFI));
}

static void test_disconnect_from_each_active_phase_is_no_wifi(void)
{
    const wifi_startup_flow_event_t events[] = {
        WIFI_STARTUP_EVENT_START,
        WIFI_STARTUP_EVENT_DISCONNECTED,
        WIFI_STARTUP_EVENT_RECONNECT,
        WIFI_STARTUP_EVENT_GOT_IP,
        WIFI_STARTUP_EVENT_DISCONNECTED,
        WIFI_STARTUP_EVENT_RECONNECT,
        WIFI_STARTUP_EVENT_GOT_IP,
        WIFI_STARTUP_EVENT_TIME_SYNCED,
        WIFI_STARTUP_EVENT_DISCONNECTED,
    };
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        assert(wifi_startup_flow_apply(&flow, events[i]));
        if (events[i] == WIFI_STARTUP_EVENT_DISCONNECTED) {
            assert(flow.phase == WIFI_STARTUP_PHASE_NO_WIFI);
            assert(flow.display_state == GLUCOSE_STATE_NO_WIFI);
        }
    }
}

static void test_reconnect_from_no_wifi_reaches_syncing(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_DISCONNECTED));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_RECONNECT));
    assert(flow.phase == WIFI_STARTUP_PHASE_WIFI_CONNECTING);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(flow.phase == WIFI_STARTUP_PHASE_TIME_SYNCING);
}

static void test_timeout_and_sync_failure_never_ready(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_WIFI_TIMEOUT));
    assert(flow.phase == WIFI_STARTUP_PHASE_NO_WIFI);
    assert(!wifi_startup_flow_is_ready(&flow));

    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_SYNC_FAILED));
    assert(flow.phase == WIFI_STARTUP_PHASE_TIME_FAILED);
    assert(flow.display_state == GLUCOSE_STATE_SRV_ERROR);
    assert(!wifi_startup_flow_is_ready(&flow));
}

static void test_time_unavailable_continues_to_network_ready(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_UNAVAILABLE));
    assert(flow.phase == WIFI_STARTUP_PHASE_READY);
    assert(wifi_startup_flow_is_ready(&flow));
    assert(flow.display_state == GLUCOSE_STATE_TIME_SYNCING);
}

static void test_setup_failure_from_each_in_progress_phase_is_terminal(void)
{
    wifi_startup_flow_t flow;

    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_SETUP_FAILED));
    assert(flow.phase == WIFI_STARTUP_PHASE_SETUP_FAILED);
    assert(flow.display_state == GLUCOSE_STATE_SRV_ERROR);
    assert(!wifi_startup_flow_is_ready(&flow));

    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_SETUP_FAILED));
    assert(flow.phase == WIFI_STARTUP_PHASE_SETUP_FAILED);
    assert(flow.display_state == GLUCOSE_STATE_SRV_ERROR);
    assert(!wifi_startup_flow_is_ready(&flow));
}

static void test_invalid_transitions_are_rejected(void)
{
    wifi_startup_flow_t flow;
    wifi_startup_flow_init(&flow);
    assert(!wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(!wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_SYNCED));
    assert(!wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_UNAVAILABLE));
    assert(!wifi_startup_flow_is_ready(&flow));

    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_GOT_IP));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_SYNCED));
    assert(!wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_UNAVAILABLE));

    wifi_startup_flow_init(&flow);
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_START));
    assert(wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_WIFI_TIMEOUT));
    assert(!wifi_startup_flow_apply(&flow, WIFI_STARTUP_EVENT_TIME_UNAVAILABLE));
}

int main(void)
{
    test_start_connects();
    test_ip_then_sync_then_ready();
    test_disconnect_from_each_active_phase_is_no_wifi();
    test_reconnect_from_no_wifi_reaches_syncing();
    test_timeout_and_sync_failure_never_ready();
    test_time_unavailable_continues_to_network_ready();
    test_setup_failure_from_each_in_progress_phase_is_terminal();
    test_invalid_transitions_are_rejected();
    puts("wifi startup flow tests: PASS");
    return 0;
}
