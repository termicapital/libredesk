#include "app_boot_flow.h"
#include "config_store.h"
#include "display_port.h"

#include "esp_err.h"
#include "esp_log.h"
#include "glucose_core.h"
#include "improv_wifi.h"
#include "mvp_config.h"
#include "net_task.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "provisioning_portal.h"
#include "runtime_core.h"
#include "ui_task.h"
#include "wifi_manager.h"

#define MVP_WIFI_TIMEOUT_MS 30000U
#define MVP_SNTP_TIMEOUT_MS 5000U

static const char *TAG = "libre_desk_mvp";
static const glucose_freshness_config_t FALLBACK_FRESHNESS = {
    .fresh_max_s = GLUCOSE_DEFAULT_FRESH_MAX_S,
    .aging_max_s = GLUCOSE_DEFAULT_AGING_MAX_S,
    .stale_max_s = GLUCOSE_DEFAULT_STALE_MAX_S,
};

static display_port_t s_display_port;
static ui_task_t *s_ui;
static runtime_config_t s_config;
static glucose_core_t s_core;
static bool s_persisted_config_present;

static bool publish_operational_state(ui_task_t *ui,
                                      const glucose_freshness_config_t *freshness,
                                      glucose_display_state_t state)
{
    glucose_core_t model_core;
    runtime_ui_snapshot_t snapshot;

    if (ui == NULL || !glucose_core_init(&model_core, freshness) ||
        (state != GLUCOSE_STATE_BOOT &&
         !glucose_core_set_operational_state(&model_core, state)) ||
        !runtime_ui_snapshot_build(&model_core, &snapshot)) {
        return false;
    }
    return ui_task_publish(ui, &snapshot);
}

static void wifi_state_changed(glucose_display_state_t state, void *context)
{
    ui_task_t *ui = (ui_task_t *)context;
    (void)publish_operational_state(ui, &s_config.freshness, state);
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    return error;
}

static bool boot_load_embedded(runtime_config_t *config_out, void *context)
{
    (void)context;
    return mvp_config_load(config_out);
}

static config_store_result_t boot_load_persisted(runtime_config_t *config_out,
                                                   void *context)
{
    config_store_result_t result;

    (void)context;
    result = config_store_load(config_out);
    s_persisted_config_present = result == CONFIG_STORE_RESULT_OK;
    return result;
}

static config_store_result_t boot_get_setup_request(bool *requested_out,
                                                     void *context)
{
    (void)context;
    return config_store_setup_request_get(requested_out);
}

static void publish_startup_failure(glucose_display_state_t state)
{
    const glucose_freshness_config_t *freshness =
        glucose_freshness_config_is_valid(&s_config.freshness)
            ? &s_config.freshness
            : &FALLBACK_FRESHNESS;
    (void)publish_operational_state(s_ui, freshness, state);
}

void app_main(void)
{
    static const app_boot_flow_ops_t boot_ops = {
        .load_embedded = boot_load_embedded,
        .load_persisted = boot_load_persisted,
        .get_setup_request = boot_get_setup_request,
        .context = NULL,
    };
    app_boot_mode_t boot_mode = APP_BOOT_MODE_NORMAL;
    app_boot_result_t boot_result;
    esp_err_t error;

    ESP_LOGI(TAG, "starting live-data display MVP");

    error = display_port_start(&s_display_port, false);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "display startup failed: %s", esp_err_to_name(error));
        return;
    }
    error = ui_task_start(&s_ui, s_display_port.display);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "UI startup failed: %s", esp_err_to_name(error));
        return;
    }
    error = initialize_nvs();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NVS startup failed: %s", esp_err_to_name(error));
        publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
        return;
    }
    if (config_store_init() != CONFIG_STORE_RESULT_OK) {
        ESP_LOGE(TAG, "configuration store startup failed");
        publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
        return;
    }
    boot_result = app_boot_flow_select(&boot_ops, &s_config, &boot_mode);
    if (boot_result != APP_BOOT_RESULT_OK) {
        ESP_LOGE(TAG, "boot configuration selection failed (code %d)",
                 (int)boot_result);
        publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
        return;
    }
    if (!glucose_core_init(&s_core, &s_config.freshness)) {
        ESP_LOGE(TAG, "glucose core initialization failed");
        publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
        return;
    }
    (void)publish_operational_state(s_ui, &s_config.freshness,
                                    GLUCOSE_STATE_BOOT);

    if (boot_mode == APP_BOOT_MODE_SETUP) {
        error = provisioning_portal_start(
            &s_config, s_ui, PROVISIONING_PORTAL_DEFAULT_SESSION_TIMEOUT_MS);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "local setup startup failed: %s",
                     esp_err_to_name(error));
            publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
        }
        return;
    }

    error = wifi_manager_prepare();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi manager preparation failed: %s",
                 esp_err_to_name(error));
    }
    error = improv_wifi_start(&s_config, s_persisted_config_present);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "USB Wi-Fi provisioning startup failed: %s",
                 esp_err_to_name(error));
    }

    if (provisioning_reset_monitor_start() == PROVISIONING_RESULT_IO_ERROR) {
        ESP_LOGE(TAG, "BOOT-button monitor startup failed");
    }

    error = wifi_manager_connect_and_sync(&s_config, wifi_state_changed, s_ui,
                                          MVP_WIFI_TIMEOUT_MS,
                                          MVP_SNTP_TIMEOUT_MS);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "network/time startup failed: %s", esp_err_to_name(error));
        return;
    }
    error = net_task_start(&s_config, &s_core, s_ui);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS poller startup failed: %s", esp_err_to_name(error));
        publish_startup_failure(GLUCOSE_STATE_SRV_ERROR);
    }
}
