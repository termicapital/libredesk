#ifndef RUNTIME_CORE_H
#define RUNTIME_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "glucose_core.h"
#include "ui_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RUNTIME_CLOCK_TEXT_CAPACITY 6U

typedef struct runtime_ui_snapshot {
    ui_screen_kind_t screen;
    glucose_display_model_t model;
    char clock_text[RUNTIME_CLOCK_TEXT_CAPACITY];
} runtime_ui_snapshot_t;

bool runtime_ui_snapshot_build(const glucose_core_t *core,
                               runtime_ui_snapshot_t *snapshot_out);
void runtime_ui_snapshot_make_neutral(runtime_ui_snapshot_t *snapshot_out);
bool runtime_ui_snapshot_is_valid(const runtime_ui_snapshot_t *snapshot);
bool runtime_ui_snapshot_equal(const runtime_ui_snapshot_t *left,
                               const runtime_ui_snapshot_t *right);

#define RUNTIME_WIFI_SSID_CAPACITY 33U
#define RUNTIME_WIFI_PASSWORD_CAPACITY 65U
#define RUNTIME_BRIDGE_ORIGIN_CAPACITY 256U
#define RUNTIME_DEVICE_TOKEN_CAPACITY 44U
#define RUNTIME_POSIX_TZ_CAPACITY 65U
#define RUNTIME_AUTHORIZATION_CAPACITY 51U
#define RUNTIME_HTTP_BODY_CAPACITY GLUCOSE_LATEST_JSON_MAX_BYTES
#define RUNTIME_HTTP_ETAG_CAPACITY 129U
#define RUNTIME_HTTP_SERVER_TIME_CAPACITY GLUCOSE_TIMESTAMP_CAPACITY
#define RUNTIME_OTA_MANIFEST_MAX_BYTES 1024U
#define RUNTIME_OTA_VERSION_CAPACITY 32U
#define RUNTIME_OTA_URL_CAPACITY 512U
#define RUNTIME_OTA_SHA256_CAPACITY 65U

typedef struct runtime_ota_manifest {
    char version[RUNTIME_OTA_VERSION_CAPACITY];
    char url[RUNTIME_OTA_URL_CAPACITY];
    char sha256[RUNTIME_OTA_SHA256_CAPACITY];
    char min_prev_version[RUNTIME_OTA_VERSION_CAPACITY];
} runtime_ota_manifest_t;

typedef enum runtime_ota_parse_result {
    RUNTIME_OTA_PARSE_OK = 0,
    RUNTIME_OTA_PARSE_NULL_ARGUMENT,
    RUNTIME_OTA_PARSE_BODY_TOO_LARGE,
    RUNTIME_OTA_PARSE_INVALID_JSON,
    RUNTIME_OTA_PARSE_UNKNOWN_FIELD,
    RUNTIME_OTA_PARSE_DUPLICATE_FIELD,
    RUNTIME_OTA_PARSE_MISSING_FIELD,
    RUNTIME_OTA_PARSE_INVALID_VERSION,
    RUNTIME_OTA_PARSE_INVALID_URL,
    RUNTIME_OTA_PARSE_INVALID_SHA256,
    RUNTIME_OTA_PARSE_INVALID_MIN_PREV_VERSION,
    RUNTIME_OTA_PARSE_INVALID_VERSION_RELATION,
    RUNTIME_OTA_PARSE_RESULT_COUNT
} runtime_ota_parse_result_t;

typedef enum runtime_ota_policy {
    RUNTIME_OTA_POLICY_UPDATE = 0,
    RUNTIME_OTA_POLICY_UP_TO_DATE,
    RUNTIME_OTA_POLICY_DOWNGRADE,
    RUNTIME_OTA_POLICY_REQUIRES_INTERMEDIATE,
    RUNTIME_OTA_POLICY_INVALID,
    RUNTIME_OTA_POLICY_COUNT
} runtime_ota_policy_t;

runtime_ota_parse_result_t runtime_ota_manifest_parse(
    const uint8_t *json, size_t length, runtime_ota_manifest_t *manifest_out);
runtime_ota_policy_t runtime_ota_manifest_policy(
    const runtime_ota_manifest_t *manifest,
    const char *current_version, size_t current_version_capacity);
bool runtime_ota_url_same_origin(
    const char *url, size_t url_capacity,
    const char *bridge_origin, size_t bridge_origin_capacity);

typedef enum runtime_http_result {
    RUNTIME_HTTP_RESULT_BODY = 0,
    RUNTIME_HTTP_RESULT_NOT_MODIFIED,
    RUNTIME_HTTP_RESULT_ERROR_BODY,
    RUNTIME_HTTP_RESULT_MALFORMED,
    RUNTIME_HTTP_RESULT_UNKNOWN_STATUS
} runtime_http_result_t;

typedef struct runtime_http_exchange {
    unsigned status;
    bool status_set;
    bool failed;
    uint8_t body[RUNTIME_HTTP_BODY_CAPACITY];
    size_t body_length;
    char etag[RUNTIME_HTTP_ETAG_CAPACITY];
    bool has_etag;
    uint32_t retry_after_s;
    bool has_retry_after;
    uint32_t reading_age_s;
    bool has_reading_age;
    char reading_server_time[RUNTIME_HTTP_SERVER_TIME_CAPACITY];
    bool has_reading_server_time;
} runtime_http_exchange_t;

void runtime_http_exchange_init(runtime_http_exchange_t *exchange);
bool runtime_http_exchange_set_status(runtime_http_exchange_t *exchange, unsigned status);
bool runtime_http_exchange_add_header(runtime_http_exchange_t *exchange,
                                      const char *name, size_t name_capacity,
                                      const char *value, size_t value_capacity);
bool runtime_http_exchange_append_body(runtime_http_exchange_t *exchange,
                                       const uint8_t *data, size_t length);
runtime_http_result_t runtime_http_exchange_finish(runtime_http_exchange_t *exchange);

typedef struct runtime_config {
    char wifi_ssid[RUNTIME_WIFI_SSID_CAPACITY];
    char wifi_password[RUNTIME_WIFI_PASSWORD_CAPACITY];
    char bridge_origin[RUNTIME_BRIDGE_ORIGIN_CAPACITY];
    char device_token[RUNTIME_DEVICE_TOKEN_CAPACITY];
    char tz[RUNTIME_POSIX_TZ_CAPACITY];
    glucose_unit_t unit;
    uint8_t brightness_day_percent;
    uint8_t brightness_night_percent;
    uint16_t night_start_minute;
    uint16_t night_end_minute;
    glucose_freshness_config_t freshness;
    uint32_t poll_s;
    uint8_t ota_enabled;
} runtime_config_t;

typedef enum runtime_config_error {
    RUNTIME_CONFIG_ERROR_NONE = 0,
    RUNTIME_CONFIG_ERROR_NULL_CONFIG,
    RUNTIME_CONFIG_ERROR_SSID,
    RUNTIME_CONFIG_ERROR_PASSWORD,
    RUNTIME_CONFIG_ERROR_BRIDGE_ORIGIN,
    RUNTIME_CONFIG_ERROR_DEVICE_TOKEN,
    RUNTIME_CONFIG_ERROR_UNIT,
    RUNTIME_CONFIG_ERROR_BRIGHTNESS_DAY,
    RUNTIME_CONFIG_ERROR_BRIGHTNESS_NIGHT,
    RUNTIME_CONFIG_ERROR_NIGHT_SCHEDULE,
    RUNTIME_CONFIG_ERROR_TZ,
    RUNTIME_CONFIG_ERROR_FRESHNESS,
    RUNTIME_CONFIG_ERROR_POLL,
    RUNTIME_CONFIG_ERROR_OTA_FLAG,
    RUNTIME_CONFIG_ERROR_COUNT
} runtime_config_error_t;

bool runtime_config_validate(const runtime_config_t *config, runtime_config_error_t *error_out);
bool runtime_url_is_https_origin(const char *origin, size_t capacity);
uint32_t runtime_poll_interval_clamp(uint32_t poll_s);

#ifdef __cplusplus
}
#endif
#endif
