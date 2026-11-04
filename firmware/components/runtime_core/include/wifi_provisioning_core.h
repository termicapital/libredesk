#ifndef WIFI_PROVISIONING_CORE_H
#define WIFI_PROVISIONING_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROVISIONING_FORM_MAX_BYTES 256U
#define WIFI_PROVISIONING_AP_SSID_CAPACITY 17U
#define WIFI_PROVISIONING_AP_PASSWORD_CAPACITY 13U

typedef struct wifi_provisioning_submission {
    char ssid[RUNTIME_WIFI_SSID_CAPACITY];
    char password[RUNTIME_WIFI_PASSWORD_CAPACITY];
} wifi_provisioning_submission_t;

typedef enum wifi_provisioning_result {
    WIFI_PROVISIONING_OK = 0,
    WIFI_PROVISIONING_NULL_ARGUMENT,
    WIFI_PROVISIONING_BODY_TOO_LARGE,
    WIFI_PROVISIONING_MALFORMED,
    WIFI_PROVISIONING_RESULT_COUNT
} wifi_provisioning_result_t;

bool wifi_provisioning_ap_credentials(
    const uint8_t *mac, size_t mac_capacity,
    const uint8_t *random, size_t random_capacity,
    char *ssid_out, size_t ssid_capacity,
    char *password_out, size_t password_capacity);

wifi_provisioning_result_t wifi_provisioning_parse_form(
    const uint8_t *body, size_t length,
    wifi_provisioning_submission_t *submission_out);

bool wifi_provisioning_candidate_build(
    const runtime_config_t *active,
    const wifi_provisioning_submission_t *submission,
    runtime_config_t *candidate_out);

#ifdef __cplusplus
}
#endif

#endif
