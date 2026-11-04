#ifndef MVP_HTTP_FLOW_H
#define MVP_HTTP_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure, fixed-size adapter for one completed HTTP exchange. It owns no
 * storage and never creates a numeric display model except through
 * glucose_core_ingest_http() accepting HTTP 200.
 */
bool mvp_http_flow_publish_fetching(glucose_core_t *core,
                                    runtime_ui_snapshot_t *snapshot_out);
bool mvp_http_flow_apply_exchange(glucose_core_t *core,
                                  const runtime_http_exchange_t *exchange,
                                  uint64_t monotonic_now_s,
                                  runtime_ui_snapshot_t *snapshot_out);
bool mvp_http_flow_apply_transport_failure(glucose_core_t *core,
                                           runtime_ui_snapshot_t *snapshot_out);
bool mvp_http_flow_tick(glucose_core_t *core,
                        uint64_t monotonic_now_s,
                        runtime_ui_snapshot_t *snapshot_out);

#ifdef __cplusplus
}
#endif

#endif
