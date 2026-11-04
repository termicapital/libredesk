#ifndef WIFI_STARTUP_FLOW_H
#define WIFI_STARTUP_FLOW_H

#include <stdbool.h>

#include "glucose_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wifi_startup_flow_phase {
    WIFI_STARTUP_PHASE_IDLE = 0,
    WIFI_STARTUP_PHASE_WIFI_CONNECTING,
    WIFI_STARTUP_PHASE_TIME_SYNCING,
    WIFI_STARTUP_PHASE_READY,
    WIFI_STARTUP_PHASE_NO_WIFI,
    WIFI_STARTUP_PHASE_TIME_FAILED,
    WIFI_STARTUP_PHASE_SETUP_FAILED
} wifi_startup_flow_phase_t;

typedef enum wifi_startup_flow_event {
    WIFI_STARTUP_EVENT_START = 0,
    WIFI_STARTUP_EVENT_GOT_IP,
    WIFI_STARTUP_EVENT_TIME_SYNCED,
    WIFI_STARTUP_EVENT_TIME_UNAVAILABLE,
    WIFI_STARTUP_EVENT_TIME_SYNC_FAILED,
    WIFI_STARTUP_EVENT_DISCONNECTED,
    WIFI_STARTUP_EVENT_RECONNECT,
    WIFI_STARTUP_EVENT_WIFI_TIMEOUT,
    WIFI_STARTUP_EVENT_SETUP_FAILED
} wifi_startup_flow_event_t;

typedef struct wifi_startup_flow {
    wifi_startup_flow_phase_t phase;
    glucose_display_state_t display_state;
} wifi_startup_flow_t;

void wifi_startup_flow_init(wifi_startup_flow_t *flow);
bool wifi_startup_flow_apply(wifi_startup_flow_t *flow,
                             wifi_startup_flow_event_t event);
bool wifi_startup_flow_is_ready(const wifi_startup_flow_t *flow);
bool wifi_startup_flow_has_display_state(const wifi_startup_flow_t *flow,
                                         glucose_display_state_t state);

#ifdef __cplusplus
}
#endif

#endif
