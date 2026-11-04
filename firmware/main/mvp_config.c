#include "mvp_config.h"

#include <string.h>

#ifndef MVP_CONFIG_SECRETS_HEADER
#define MVP_CONFIG_SECRETS_HEADER "mvp_secrets.h"
#endif
#include MVP_CONFIG_SECRETS_HEADER

#ifndef MVP_WIFI_SSID
#error "MVP_WIFI_SSID is required; copy firmware/main/mvp_secrets.example.h to firmware/main/mvp_secrets.h"
#endif
#ifndef MVP_WIFI_PASSWORD
#error "MVP_WIFI_PASSWORD is required; copy firmware/main/mvp_secrets.example.h to firmware/main/mvp_secrets.h"
#endif
#ifndef MVP_BRIDGE_ORIGIN
#error "MVP_BRIDGE_ORIGIN is required; copy firmware/main/mvp_secrets.example.h to firmware/main/mvp_secrets.h"
#endif
#ifndef MVP_DEVICE_TOKEN
#error "MVP_DEVICE_TOKEN is required; copy firmware/main/mvp_secrets.example.h to firmware/main/mvp_secrets.h"
#endif

static bool copy_bounded(char *destination, size_t destination_capacity,
                         const char *source, size_t source_capacity)
{
    const char *terminator;
    size_t length;

    if (source == NULL || source_capacity == 0U) {
        return false;
    }
    terminator = (const char *)memchr(source, '\0', source_capacity);
    if (terminator == NULL) {
        return false;
    }
    length = (size_t)(terminator - source);
    if (length >= destination_capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

bool mvp_config_build(runtime_config_t *config_out,
                      const char *wifi_ssid, size_t wifi_ssid_capacity,
                      const char *wifi_password, size_t wifi_password_capacity,
                      const char *bridge_origin, size_t bridge_origin_capacity,
                      const char *device_token, size_t device_token_capacity)
{
    runtime_config_t staged;

    if (config_out == NULL) {
        return false;
    }
    memset(config_out, 0, sizeof(*config_out));
    memset(&staged, 0, sizeof(staged));

    if (!copy_bounded(staged.wifi_ssid, sizeof(staged.wifi_ssid), wifi_ssid,
                      wifi_ssid_capacity) ||
        !copy_bounded(staged.wifi_password, sizeof(staged.wifi_password),
                      wifi_password, wifi_password_capacity) ||
        !copy_bounded(staged.bridge_origin, sizeof(staged.bridge_origin),
                      bridge_origin, bridge_origin_capacity) ||
        !copy_bounded(staged.device_token, sizeof(staged.device_token),
                      device_token, device_token_capacity)) {
        return false;
    }

    memcpy(staged.tz, "CET-1CEST,M3.5.0,M10.5.0/3",
           sizeof("CET-1CEST,M3.5.0,M10.5.0/3"));
    staged.unit = GLUCOSE_UNIT_MG_DL;
    staged.brightness_day_percent = 100U;
    staged.brightness_night_percent = 25U;
    staged.night_start_minute = 22U * 60U;
    staged.night_end_minute = 7U * 60U;
    staged.freshness.fresh_max_s = 360U;
    staged.freshness.aging_max_s = 900U;
    staged.freshness.stale_max_s = 3600U;
    staged.poll_s = 60U;
    staged.ota_enabled = 0U;

    if (!runtime_config_validate(&staged, NULL)) {
        return false;
    }
    *config_out = staged;
    return true;
}

bool mvp_config_load(runtime_config_t *config_out)
{
    if (config_out == NULL) {
        return false;
    }
    return mvp_config_build(config_out,
                            MVP_WIFI_SSID, sizeof(MVP_WIFI_SSID),
                            MVP_WIFI_PASSWORD, sizeof(MVP_WIFI_PASSWORD),
                            MVP_BRIDGE_ORIGIN, sizeof(MVP_BRIDGE_ORIGIN),
                            MVP_DEVICE_TOKEN, sizeof(MVP_DEVICE_TOKEN));
}
