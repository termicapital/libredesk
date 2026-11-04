#ifndef GLUCOSE_CORE_H
#define GLUCOSE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contract and display limits. Capacities include the terminating NUL. */
#define GLUCOSE_TIMESTAMP_CAPACITY 31U
#define GLUCOSE_LOCAL_TIME_CAPACITY 9U
#define GLUCOSE_VALUE_TEXT_CAPACITY 8U
#define GLUCOSE_UNIT_TEXT_CAPACITY 8U
#define GLUCOSE_AGE_TEXT_CAPACITY 9U
#define GLUCOSE_STATUS_TEXT_MAX 8U
#define GLUCOSE_STATUS_TEXT_CAPACITY (GLUCOSE_STATUS_TEXT_MAX + 1U)
#define GLUCOSE_LATEST_JSON_MAX_BYTES 2048U
#define GLUCOSE_ERROR_JSON_MAX_BYTES 512U

/* PLAN.md section 6.6 defaults; callers may supply stricter explicit values. */
#define GLUCOSE_DEFAULT_FRESH_MAX_S 360U
#define GLUCOSE_DEFAULT_AGING_MAX_S 900U
#define GLUCOSE_DEFAULT_STALE_MAX_S 3600U

/* Deterministic jitter samples span [0, 10000]; 5000 means no jitter. */
#define GLUCOSE_JITTER_SAMPLE_MAX 10000U
#define GLUCOSE_JITTER_SAMPLE_MIDPOINT 5000U

typedef enum glucose_unit {
    GLUCOSE_UNIT_MG_DL = 0,
    GLUCOSE_UNIT_MMOL_L,
    GLUCOSE_UNIT_COUNT
} glucose_unit_t;

typedef enum glucose_trend {
    GLUCOSE_TREND_RISING = 0,
    GLUCOSE_TREND_RISING_SLIGHTLY,
    GLUCOSE_TREND_STABLE,
    GLUCOSE_TREND_FALLING_SLIGHTLY,
    GLUCOSE_TREND_FALLING,
    GLUCOSE_TREND_RISING_FAST,
    GLUCOSE_TREND_FALLING_FAST,
    GLUCOSE_TREND_UNKNOWN,
    GLUCOSE_TREND_COUNT
} glucose_trend_t;

typedef enum glucose_range {
    GLUCOSE_RANGE_URGENT_LOW = 0,
    GLUCOSE_RANGE_LOW,
    GLUCOSE_RANGE_IN_RANGE,
    GLUCOSE_RANGE_HIGH,
    GLUCOSE_RANGE_UNKNOWN,
    GLUCOSE_RANGE_COUNT
} glucose_range_t;

typedef enum glucose_source {
    GLUCOSE_SOURCE_LIBRELINKUP = 0,
    GLUCOSE_SOURCE_MOCK,
    GLUCOSE_SOURCE_COUNT
} glucose_source_t;

typedef enum glucose_display_state {
    GLUCOSE_STATE_BOOT = 0,
    GLUCOSE_STATE_WIFI_CONNECTING,
    GLUCOSE_STATE_TIME_SYNCING,
    GLUCOSE_STATE_FETCHING,
    GLUCOSE_STATE_FRESH,
    GLUCOSE_STATE_AGING,
    GLUCOSE_STATE_STALE,
    GLUCOSE_STATE_NO_DATA,
    GLUCOSE_STATE_AUTH_ERROR,
    GLUCOSE_STATE_SRV_ERROR,
    GLUCOSE_STATE_NO_WIFI,
    GLUCOSE_STATE_COUNT
} glucose_display_state_t;

typedef enum glucose_parse_status {
    GLUCOSE_PARSE_OK = 0,
    GLUCOSE_PARSE_NULL_ARGUMENT,
    GLUCOSE_PARSE_INVALID_JSON,
    GLUCOSE_PARSE_MISSING_FIELD,
    GLUCOSE_PARSE_DUPLICATE_FIELD,
    GLUCOSE_PARSE_UNKNOWN_FIELD,
    /* Reserved: wrong wire types currently fail closed as INVALID_VALUE. */
    GLUCOSE_PARSE_INVALID_TYPE,
    GLUCOSE_PARSE_INVALID_VALUE,
    GLUCOSE_PARSE_UNSUPPORTED_SCHEMA,
    GLUCOSE_PARSE_TIME_MISMATCH,
    GLUCOSE_PARSE_STATUS_COUNT
} glucose_parse_status_t;

typedef enum glucose_error_code {
    GLUCOSE_ERROR_INVALID_TOKEN = 0,
    GLUCOSE_ERROR_REVOKED_TOKEN,
    GLUCOSE_ERROR_RATE_LIMITED,
    GLUCOSE_ERROR_UPSTREAM_UNAVAILABLE,
    GLUCOSE_ERROR_UPSTREAM_AUTH_FAILED,
    GLUCOSE_ERROR_INTERNAL,
    GLUCOSE_ERROR_MALFORMED_UPSTREAM,
    GLUCOSE_ERROR_TERMS_OF_USE_REQUIRED,
    GLUCOSE_ERROR_INVALID_PAYLOAD,
    GLUCOSE_ERROR_OTA_UNAVAILABLE,
    GLUCOSE_ERROR_CODE_COUNT
} glucose_error_code_t;

typedef enum glucose_ingest_result {
    GLUCOSE_INGEST_UPDATED = 0,
    GLUCOSE_INGEST_NOT_MODIFIED,
    GLUCOSE_INGEST_ERROR_PAYLOAD,
    GLUCOSE_INGEST_MALFORMED
} glucose_ingest_result_t;

typedef struct glucose_reading {
    /* Canonical internal representation. No value is populated on parse failure. */
    double value_mg_dl;
    glucose_unit_t unit;
    glucose_trend_t trend;
    glucose_range_t range;
    bool has_target_range;
    double target_low_mg_dl;
    double target_high_mg_dl;
    char timestamp[GLUCOSE_TIMESTAMP_CAPACITY];
    uint32_t libre_local_seconds;
    uint32_t age_s;
    char server_time[GLUCOSE_TIMESTAMP_CAPACITY];
    glucose_source_t source;
    uint32_t poll_after_s;
} glucose_reading_t;

typedef struct glucose_error_payload {
    glucose_error_code_t code;
    uint32_t retry_after_s;
} glucose_error_payload_t;

typedef struct glucose_freshness_config {
    uint32_t fresh_max_s;
    uint32_t aging_max_s;
    uint32_t stale_max_s;
} glucose_freshness_config_t;

typedef struct glucose_backoff_config {
    uint32_t minimum_s;
    uint32_t base_s;
    uint32_t maximum_s;
    uint32_t jitter_s;
} glucose_backoff_config_t;

typedef struct glucose_core {
    glucose_freshness_config_t freshness;
    glucose_reading_t reading;
    bool has_reading;
    glucose_display_state_t state;
    uint64_t last_monotonic_s;
    uint32_t current_age_s;
    uint32_t retry_after_s;
} glucose_core_t;

typedef struct glucose_display_model {
    glucose_display_state_t state;
    bool show_number;
    bool use_range_hue;
    glucose_trend_t trend;
    glucose_range_t range;
    char value_text[GLUCOSE_VALUE_TEXT_CAPACITY];
    char unit_text[GLUCOSE_UNIT_TEXT_CAPACITY];
    char age_text[GLUCOSE_AGE_TEXT_CAPACITY];
    char status_text[GLUCOSE_STATUS_TEXT_CAPACITY];
} glucose_display_model_t;

/* Strict, transactional parsers for the frozen flat JSON contracts. */
glucose_parse_status_t glucose_parse_latest_json(const char *json,
                                                  size_t length,
                                                  glucose_reading_t *reading_out);
glucose_parse_status_t glucose_parse_error_json(const char *json,
                                                 size_t length,
                                                 glucose_error_payload_t *error_out);
const char *glucose_parse_status_name(glucose_parse_status_t status);

/* Trend/range mapping. Invalid or absent inputs map to UNKNOWN, never a guess. */
glucose_trend_t glucose_trend_from_libre_arrow(int trend_arrow);
const char *glucose_trend_name(glucose_trend_t trend);
const char *glucose_range_name(glucose_range_t range);
const char *glucose_error_code_name(glucose_error_code_t code);
const char *glucose_state_name(glucose_display_state_t state);
bool glucose_range_classify_mg_dl(double glucose_mg_dl,
                                  double urgent_low_mg_dl,
                                  double target_low_mg_dl,
                                  double target_high_mg_dl,
                                  glucose_range_t *range_out);

/* Freshness state machine and the two safety gates used by rendering code. */
bool glucose_freshness_config_is_valid(const glucose_freshness_config_t *config);
bool glucose_freshness_classify(uint32_t age_s,
                                const glucose_freshness_config_t *config,
                                glucose_display_state_t *state_out);
bool glucose_state_allows_number(glucose_display_state_t state);
bool glucose_state_allows_range_hue(glucose_display_state_t state);

/* Bounded formatting and conversion. Output buffers are cleared on failure. */
bool glucose_convert_from_mg_dl(double glucose_mg_dl,
                                glucose_unit_t unit,
                                double *converted_out);
bool glucose_format_value(double glucose_mg_dl,
                           glucose_unit_t unit,
                           char *buffer,
                           size_t buffer_size);
bool glucose_format_age(uint32_t age_s, char *buffer, size_t buffer_size);

/* Saturating exponential backoff with caller-provided deterministic jitter. */
bool glucose_backoff_config_is_valid(const glucose_backoff_config_t *config);
bool glucose_backoff_delay_s(const glucose_backoff_config_t *config,
                             uint32_t consecutive_failures,
                             uint32_t jitter_sample,
                             uint32_t *delay_out);

/* Stateful facade. Any malformed/error response clears the displayable value. */
bool glucose_core_init(glucose_core_t *core,
                       const glucose_freshness_config_t *freshness);
glucose_ingest_result_t glucose_core_ingest_http(glucose_core_t *core,
                                                  unsigned int http_status,
                                                  const char *body,
                                                  size_t body_length,
                                                  uint64_t monotonic_now_s,
                                                  glucose_parse_status_t *parse_status_out);
glucose_ingest_result_t glucose_core_ingest_not_modified(
    glucose_core_t *core,
    uint32_t server_age_s,
    uint64_t monotonic_now_s);
void glucose_core_tick(glucose_core_t *core, uint64_t monotonic_now_s);
bool glucose_core_set_operational_state(glucose_core_t *core,
                                        glucose_display_state_t state);
bool glucose_core_build_display(const glucose_core_t *core,
                                glucose_display_model_t *display_out);

#ifdef __cplusplus
}
#endif

#endif /* GLUCOSE_CORE_H */
