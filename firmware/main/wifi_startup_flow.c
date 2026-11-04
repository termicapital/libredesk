#include "wifi_startup_flow.h"

void wifi_startup_flow_init(wifi_startup_flow_t *flow)
{
    if (flow == NULL) {
        return;
    }
    flow->phase = WIFI_STARTUP_PHASE_IDLE;
    flow->display_state = GLUCOSE_STATE_BOOT;
}

bool wifi_startup_flow_apply(wifi_startup_flow_t *flow,
                             wifi_startup_flow_event_t event)
{
    if (flow == NULL) {
        return false;
    }

    switch (event) {
    case WIFI_STARTUP_EVENT_START:
        if (flow->phase != WIFI_STARTUP_PHASE_IDLE) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_WIFI_CONNECTING;
        flow->display_state = GLUCOSE_STATE_WIFI_CONNECTING;
        return true;
    case WIFI_STARTUP_EVENT_GOT_IP:
        if (flow->phase != WIFI_STARTUP_PHASE_WIFI_CONNECTING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_TIME_SYNCING;
        flow->display_state = GLUCOSE_STATE_TIME_SYNCING;
        return true;
    case WIFI_STARTUP_EVENT_TIME_SYNCED:
        if (flow->phase != WIFI_STARTUP_PHASE_TIME_SYNCING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_READY;
        return true;
    case WIFI_STARTUP_EVENT_TIME_UNAVAILABLE:
        if (flow->phase != WIFI_STARTUP_PHASE_TIME_SYNCING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_READY;
        return true;
    case WIFI_STARTUP_EVENT_TIME_SYNC_FAILED:
        if (flow->phase != WIFI_STARTUP_PHASE_TIME_SYNCING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_TIME_FAILED;
        flow->display_state = GLUCOSE_STATE_SRV_ERROR;
        return true;
    case WIFI_STARTUP_EVENT_SETUP_FAILED:
        if (flow->phase != WIFI_STARTUP_PHASE_WIFI_CONNECTING &&
            flow->phase != WIFI_STARTUP_PHASE_TIME_SYNCING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_SETUP_FAILED;
        flow->display_state = GLUCOSE_STATE_SRV_ERROR;
        return true;
    case WIFI_STARTUP_EVENT_DISCONNECTED:
        if (flow->phase != WIFI_STARTUP_PHASE_WIFI_CONNECTING &&
            flow->phase != WIFI_STARTUP_PHASE_TIME_SYNCING &&
            flow->phase != WIFI_STARTUP_PHASE_READY) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_NO_WIFI;
        flow->display_state = GLUCOSE_STATE_NO_WIFI;
        return true;
    case WIFI_STARTUP_EVENT_RECONNECT:
        if (flow->phase != WIFI_STARTUP_PHASE_NO_WIFI) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_WIFI_CONNECTING;
        flow->display_state = GLUCOSE_STATE_WIFI_CONNECTING;
        return true;
    case WIFI_STARTUP_EVENT_WIFI_TIMEOUT:
        if (flow->phase != WIFI_STARTUP_PHASE_WIFI_CONNECTING) {
            return false;
        }
        flow->phase = WIFI_STARTUP_PHASE_NO_WIFI;
        flow->display_state = GLUCOSE_STATE_NO_WIFI;
        return true;
    default:
        return false;
    }
}

bool wifi_startup_flow_is_ready(const wifi_startup_flow_t *flow)
{
    return flow != NULL && flow->phase == WIFI_STARTUP_PHASE_READY;
}

bool wifi_startup_flow_has_display_state(const wifi_startup_flow_t *flow,
                                         glucose_display_state_t state)
{
    return flow != NULL && flow->display_state == state;
}
