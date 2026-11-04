/*
 * Local-only prototype values. Copy this file to mvp_secrets.h and replace
 * every placeholder locally; mvp_secrets.h is gitignored and must never be
 * committed. These values are intentionally invalid and are not usable
 * credentials or a usable device token.
 *
 * Prototype limitation: the Wi-Fi password and revocable device token are
 * embedded in the firmware binary. This is acceptable only for the personal
 * MVP and must be replaced after proof with secure provisioning.
 */
#define MVP_WIFI_SSID "REPLACE_WITH_LOCAL_WIFI_SSID"
#define MVP_WIFI_PASSWORD "NOT_A_REAL_WIFI_PASSWORD"
#define MVP_BRIDGE_ORIGIN "https://REPLACE_WITH_LOCAL_BRIDGE_ORIGIN"
#define MVP_DEVICE_TOKEN "NOT_A_REAL_DEVICE_TOKEN_XXXXXXXXXXXXXXXXXXXXXXXX"
