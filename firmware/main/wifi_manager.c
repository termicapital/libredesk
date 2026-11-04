#include "wifi_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "wifi_startup_flow.h"
#include "wifi_setup_sequence.h"

#define WIFI_CONNECTED_BIT BIT0
#define TRUSTED_TIME_EPOCH ((time_t)1704067200)

typedef struct wifi_manager_context {
    EventGroupHandle_t event_group;
    StaticEventGroup_t event_group_storage;
    wifi_startup_flow_t flow;
    wifi_manager_state_cb_t cb;
    void *cb_ctx;
    bool initialized;
    SemaphoreHandle_t api_lock;
    StaticSemaphore_t api_lock_storage;
    volatile bool candidate_override;
} wifi_manager_context_t;

static wifi_manager_context_t s_manager;

static void publish(glucose_display_state_t state)
{
    s_manager.flow.display_state = state;
    if (s_manager.cb != NULL) {
        s_manager.cb(state, s_manager.cb_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_manager.event_group, WIFI_CONNECTED_BIT);
        if (s_manager.candidate_override) {
            return;
        }
        if (s_manager.flow.phase == WIFI_STARTUP_PHASE_WIFI_CONNECTING ||
            s_manager.flow.phase == WIFI_STARTUP_PHASE_TIME_SYNCING ||
            s_manager.flow.phase == WIFI_STARTUP_PHASE_READY) {
            (void)wifi_startup_flow_apply(&s_manager.flow,
                                          WIFI_STARTUP_EVENT_DISCONNECTED);
            publish(GLUCOSE_STATE_NO_WIFI);
        }
        if (s_manager.flow.phase == WIFI_STARTUP_PHASE_NO_WIFI) {
            (void)wifi_startup_flow_apply(&s_manager.flow,
                                          WIFI_STARTUP_EVENT_RECONNECT);
            publish(GLUCOSE_STATE_WIFI_CONNECTING);
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_manager.event_group, WIFI_CONNECTED_BIT);
        if (wifi_startup_flow_apply(&s_manager.flow, WIFI_STARTUP_EVENT_GOT_IP)) {
            publish(GLUCOSE_STATE_TIME_SYNCING);
        }
    }
}

static esp_err_t initialize_once(void)
{
    esp_err_t err;
    if (s_manager.initialized) {
        return ESP_OK;
    }

    s_manager.api_lock = xSemaphoreCreateMutexStatic(&s_manager.api_lock_storage);
    s_manager.event_group = xEventGroupCreateStatic(&s_manager.event_group_storage);
    if (s_manager.api_lock == NULL || s_manager.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s_manager.initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_prepare(void)
{
    return initialize_once();
}

static bool station_config_build(const runtime_config_t *config,
                                 wifi_config_t *station_out)
{
    size_t ssid_length;
    size_t password_length;

    if (config == NULL || station_out == NULL ||
        !runtime_config_validate(config, NULL)) {
        return false;
    }
    ssid_length = strlen(config->wifi_ssid);
    password_length = strlen(config->wifi_password);
    if (ssid_length > sizeof(station_out->sta.ssid) ||
        password_length > sizeof(station_out->sta.password)) {
        return false;
    }
    memset(station_out, 0, sizeof(*station_out));
    memcpy(station_out->sta.ssid, config->wifi_ssid, ssid_length);
    memcpy(station_out->sta.password, config->wifi_password, password_length);
    station_out->sta.threshold.authmode = WIFI_AUTH_OPEN;
    return true;
}

static esp_err_t restore_fallback_locked(wifi_config_t *fallback)
{
    esp_err_t error;

    (void)esp_wifi_disconnect();
    xEventGroupClearBits(s_manager.event_group, WIFI_CONNECTED_BIT);
    error = esp_wifi_set_config(WIFI_IF_STA, fallback);
    if (error != ESP_OK) return error;
    s_manager.candidate_override = false;
    error = esp_wifi_connect();
    if (error == ESP_ERR_WIFI_CONN) error = ESP_OK;
    return error;
}

static bool connected_ssid_matches(const runtime_config_t *candidate)
{
    wifi_ap_record_t access_point;
    size_t ssid_length = strlen(candidate->wifi_ssid);

    memset(&access_point, 0, sizeof(access_point));
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK ||
        memcmp(access_point.ssid, candidate->wifi_ssid, ssid_length) != 0) {
        memset(&access_point, 0, sizeof(access_point));
        return false;
    }
    if (ssid_length < sizeof(access_point.ssid) &&
        access_point.ssid[ssid_length] != '\0') {
        memset(&access_point, 0, sizeof(access_point));
        return false;
    }
    memset(&access_point, 0, sizeof(access_point));
    return true;
}

esp_err_t wifi_manager_try_candidate(const runtime_config_t *candidate,
                                     const runtime_config_t *fallback,
                                     uint32_t timeout_ms)
{
    wifi_config_t station;
    wifi_config_t fallback_station;
    EventBits_t bits;
    TickType_t started_at;
    TickType_t timeout_ticks;
    TickType_t elapsed;
    esp_err_t error;
    esp_err_t restore_error;

    memset(&station, 0, sizeof(station));
    memset(&fallback_station, 0, sizeof(fallback_station));
    if (timeout_ms == 0U || !station_config_build(candidate, &station) ||
        !station_config_build(fallback, &fallback_station)) {
        error = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    error = initialize_once();
    if (error != ESP_OK) {
        goto cleanup;
    }
    if (xSemaphoreTake(s_manager.api_lock, portMAX_DELAY) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    s_manager.candidate_override = true;
    xEventGroupClearBits(s_manager.event_group, WIFI_CONNECTED_BIT);
    (void)esp_wifi_disconnect();
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (error == ESP_OK) {
        error = esp_wifi_start();
        if (error == ESP_ERR_WIFI_CONN) error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_wifi_connect();
        if (error == ESP_ERR_WIFI_CONN) error = ESP_OK;
    }
    if (error != ESP_OK) {
        restore_error = restore_fallback_locked(&fallback_station);
        if (restore_error != ESP_OK) error = restore_error;
    }
    (void)xSemaphoreGive(s_manager.api_lock);
    if (error != ESP_OK) {
        goto cleanup;
    }

    started_at = xTaskGetTickCount();
    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    do {
        elapsed = xTaskGetTickCount() - started_at;
        if (elapsed >= timeout_ticks) break;
        bits = xEventGroupWaitBits(s_manager.event_group, WIFI_CONNECTED_BIT,
                                   pdFALSE, pdTRUE, timeout_ticks - elapsed);
        if ((bits & WIFI_CONNECTED_BIT) == 0U) break;
        if (connected_ssid_matches(candidate)) goto candidate_connected;
        xEventGroupClearBits(s_manager.event_group, WIFI_CONNECTED_BIT);
    } while (true);

    if (xSemaphoreTake(s_manager.api_lock, portMAX_DELAY) == pdTRUE) {
        restore_error = restore_fallback_locked(&fallback_station);
        (void)xSemaphoreGive(s_manager.api_lock);
    } else {
        restore_error = ESP_ERR_TIMEOUT;
    }
    error = restore_error == ESP_OK ? ESP_ERR_TIMEOUT : restore_error;
    goto cleanup;

candidate_connected:
    if (xSemaphoreTake(s_manager.api_lock, portMAX_DELAY) != pdTRUE) {
        error = ESP_FAIL;
        goto cleanup;
    }
    s_manager.candidate_override = false;
    (void)xSemaphoreGive(s_manager.api_lock);
    error = ESP_OK;

cleanup:
    memset(&station, 0, sizeof(station));
    memset(&fallback_station, 0, sizeof(fallback_station));
    return error;
}

static esp_err_t perform_wifi_setup(wifi_setup_operation_t operation,
                                    void *context)
{
    wifi_config_t *wifi_config = (wifi_config_t *)context;

    switch (operation) {
    case WIFI_SETUP_OPERATION_INITIALIZE:
        return initialize_once();
    case WIFI_SETUP_OPERATION_SET_MODE:
        return esp_wifi_set_mode(WIFI_MODE_STA);
    case WIFI_SETUP_OPERATION_SET_CONFIG:
        return wifi_config == NULL
                   ? ESP_ERR_INVALID_ARG
                   : esp_wifi_set_config(WIFI_IF_STA, wifi_config);
    case WIFI_SETUP_OPERATION_START:
        return esp_wifi_start();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static bool trusted_time(void)
{
    return time(NULL) >= TRUSTED_TIME_EPOCH;
}

static void publish_time_failure(void)
{
    if (wifi_startup_flow_apply(&s_manager.flow,
                                WIFI_STARTUP_EVENT_TIME_SYNC_FAILED)) {
        publish(s_manager.flow.display_state);
    }
}

static void publish_setup_failure(void *context)
{
    (void)context;
    if (wifi_startup_flow_apply(&s_manager.flow,
                                WIFI_STARTUP_EVENT_SETUP_FAILED)) {
        publish(s_manager.flow.display_state);
        return;
    }
    publish(GLUCOSE_STATE_SRV_ERROR);
}

esp_err_t wifi_manager_connect_and_sync(const runtime_config_t *config,
                                        wifi_manager_state_cb_t cb,
                                        void *ctx,
                                        uint32_t wifi_timeout_ms,
                                        uint32_t sntp_timeout_ms)
{
    esp_err_t err;
    wifi_config_t wifi_config = { 0 };
    EventBits_t bits;

    if (config == NULL || cb == NULL ||
        !station_config_build(config, &wifi_config)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_manager.cb = cb;
    s_manager.cb_ctx = ctx;
    wifi_startup_flow_init(&s_manager.flow);
    (void)wifi_startup_flow_apply(&s_manager.flow, WIFI_STARTUP_EVENT_START);
    publish(GLUCOSE_STATE_WIFI_CONNECTING);

    err = initialize_once();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_manager.api_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_manager.candidate_override) {
        err = ESP_OK;
    } else {
        xEventGroupClearBits(s_manager.event_group, WIFI_CONNECTED_BIT);
        err = wifi_setup_sequence_run(perform_wifi_setup, &wifi_config,
                                      ESP_ERR_WIFI_CONN,
                                      publish_setup_failure, NULL);
    }
    (void)xSemaphoreGive(s_manager.api_lock);
    if (err != ESP_OK) {
        return err;
    }

    bits = xEventGroupWaitBits(s_manager.event_group, WIFI_CONNECTED_BIT,
                               pdFALSE, pdTRUE, pdMS_TO_TICKS(wifi_timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) == 0U) {
        if (s_manager.flow.phase == WIFI_STARTUP_PHASE_WIFI_CONNECTING) {
            (void)wifi_startup_flow_apply(&s_manager.flow,
                                          WIFI_STARTUP_EVENT_WIFI_TIMEOUT);
            publish(GLUCOSE_STATE_NO_WIFI);
        }
        return ESP_ERR_TIMEOUT;
    }

    if (setenv("TZ", config->tz, 1) != 0) {
        publish_time_failure();
        return ESP_FAIL;
    }
    tzset();
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("time.google.com", "time.cloudflare.com",
                                "pool.ntp.org"));
    err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        return wifi_startup_flow_apply(&s_manager.flow,
                                       WIFI_STARTUP_EVENT_TIME_UNAVAILABLE)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(sntp_timeout_ms));
    if (err != ESP_OK || !trusted_time()) {
        return wifi_startup_flow_apply(&s_manager.flow,
                                       WIFI_STARTUP_EVENT_TIME_UNAVAILABLE)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    if (!wifi_startup_flow_apply(&s_manager.flow,
                                 WIFI_STARTUP_EVENT_TIME_SYNCED)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
