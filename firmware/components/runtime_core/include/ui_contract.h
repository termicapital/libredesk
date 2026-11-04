#ifndef UI_CONTRACT_H
#define UI_CONTRACT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_screen_kind {
    UI_SCREEN_BOOT = 0,
    UI_SCREEN_PROVISIONING,
    UI_SCREEN_WIFI_CONNECTING,
    UI_SCREEN_TIME_SYNCING,
    UI_SCREEN_FETCHING,
    UI_SCREEN_FRESH,
    UI_SCREEN_AGING,
    UI_SCREEN_STALE,
    UI_SCREEN_NO_DATA,
    UI_SCREEN_AUTH_ERROR,
    UI_SCREEN_TERMS_REQUIRED,
    UI_SCREEN_SERVER_ERROR,
    UI_SCREEN_NO_WIFI,
    UI_SCREEN_OTA_UPDATING,
    UI_SCREEN_COUNT
} ui_screen_kind_t;

#ifdef __cplusplus
}
#endif
#endif
