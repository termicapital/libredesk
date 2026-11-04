#include "net_task.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mvp_http_flow.h"

#define NET_TASK_NAME "net_task"
#define NET_HTTP_TASK_NAME "net_http"
#define NET_TASK_STACK 6144U
#define NET_HTTP_TASK_STACK 6144U
#define NET_TASK_PRIORITY 6U
#define NET_HTTP_TASK_PRIORITY 5U
#define NET_TASK_TIMEOUT_MS 10000
#define NET_TASK_TICK_MS 1000U
#define NET_TASK_URL_CAPACITY (RUNTIME_BRIDGE_ORIGIN_CAPACITY + 32U)
#define NET_TASK_AUTH_CAPACITY RUNTIME_AUTHORIZATION_CAPACITY

typedef struct net_task_context {
    char url[NET_TASK_URL_CAPACITY];
    char authorization[NET_TASK_AUTH_CAPACITY];
    uint32_t poll_s;
    runtime_ui_snapshot_t last_published_snapshot;
    bool has_last_published_snapshot;
    uint64_t next_poll_monotonic_s;
    glucose_core_t *core;
    ui_task_t *ui;
    runtime_http_exchange_t exchange;
    TaskHandle_t runtime_task;
    TaskHandle_t http_task;
    /* The HTTP worker exclusively writes exchange/exchange_succeeded while a request is
     * in flight. Its task notification transfers ownership to the runtime worker, which
     * exclusively mutates glucose_core/UI state and cannot start the next request until
     * the completed exchange has been consumed. */
    volatile bool exchange_succeeded;
    volatile bool request_in_flight;
    bool task_started;
} net_task_context_t;

static net_task_context_t g_net;

static bool bounded_length(const char *value, size_t capacity, size_t *length_out)
{
    size_t index;
    if (value == NULL || length_out == NULL) return false;
    for (index = 0U; index < capacity; index++) {
        if (value[index] == '\0') {
            *length_out = index;
            return true;
        }
    }
    return false;
}

static void clear_context(void)
{
    volatile unsigned char *bytes = (volatile unsigned char *)&g_net;
    size_t index;
    for (index = 0U; index < sizeof(g_net); index++) bytes[index] = 0U;
}

static bool publish_snapshot(net_task_context_t *context,
                             runtime_ui_snapshot_t *snapshot)
{
    if (context == NULL || snapshot == NULL ||
        !runtime_ui_snapshot_is_valid(snapshot)) {
        return false;
    }
    if (context->has_last_published_snapshot &&
        runtime_ui_snapshot_equal(&context->last_published_snapshot, snapshot)) {
        return true;
    }
    if (!ui_task_publish(context->ui, snapshot)) {
        return false;
    }
    context->last_published_snapshot = *snapshot;
    context->has_last_published_snapshot = true;
    return true;
}

static uint64_t monotonic_now_s(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000000LL);
}

static uint64_t poll_deadline(uint64_t now_s, uint32_t poll_s)
{
    uint64_t interval_s = (uint64_t)poll_s;
    return UINT64_MAX - now_s < interval_s ? UINT64_MAX : now_s + interval_s;
}

static esp_err_t forward_header(esp_http_client_event_t *event)
{
    size_t name_length;
    size_t value_length;
    bool recognized;
    if (event == NULL || event->header_key == NULL || event->header_value == NULL ||
        !bounded_length(event->header_key, 128U, &name_length)) {
        g_net.exchange.failed = true;
        return ESP_FAIL;
    }
    recognized = (name_length == 4U && strncasecmp(event->header_key, "etag", 4U) == 0) ||
                 (name_length == 11U && strncasecmp(event->header_key, "retry-after", 11U) == 0) ||
                 (name_length == 15U && strncasecmp(event->header_key, "x-reading-age-s", 15U) == 0) ||
                 (name_length == 21U && strncasecmp(event->header_key, "x-reading-server-time", 21U) == 0);
    if (!recognized) {
        return ESP_OK;
    }
    if (!bounded_length(event->header_value, 256U, &value_length)) {
        g_net.exchange.failed = true;
        return ESP_FAIL;
    }
    if (!runtime_http_exchange_add_header(&g_net.exchange,
                                          event->header_key, name_length + 1U,
                                          event->header_value, value_length + 1U)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data != &g_net) return ESP_ERR_INVALID_ARG;
    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        return forward_header(event);
    case HTTP_EVENT_ON_DATA:
        if (event->data_len < 0 ||
            !runtime_http_exchange_append_body(&g_net.exchange,
                                               (const uint8_t *)event->data,
                                               (size_t)event->data_len)) {
            return ESP_FAIL;
        }
        return ESP_OK;
    default:
        return ESP_OK;
    }
}

static bool perform_http_exchange(net_task_context_t *context)
{
    esp_http_client_config_t config = {
        .url = context->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = NET_TASK_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = context,
    };
    esp_http_client_handle_t client;
    esp_err_t error;
    int status;
    bool succeeded;

    runtime_http_exchange_init(&context->exchange);
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }
    if (esp_http_client_set_header(client, "Authorization", context->authorization) != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK) {
        (void)esp_http_client_cleanup(client);
        return false;
    }
    error = esp_http_client_perform(client);
    if (error == ESP_OK && !context->exchange.status_set) {
        status = esp_http_client_get_status_code(client);
        if (status < 100 || status > 599 ||
            !runtime_http_exchange_set_status(&context->exchange, (unsigned)status)) {
            context->exchange.failed = true;
        }
    }
    succeeded = error == ESP_OK && !context->exchange.failed &&
                context->exchange.status_set;
    (void)esp_http_client_cleanup(client);
    return succeeded;
}

static void http_task_worker(void *argument)
{
    net_task_context_t *context = (net_task_context_t *)argument;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        context->exchange_succeeded = perform_http_exchange(context);
        xTaskNotifyGive(context->runtime_task);
    }
}

static void net_task_worker(void *argument)
{
    net_task_context_t *context = (net_task_context_t *)argument;
    runtime_ui_snapshot_t snapshot;
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t now_s;

    if (!mvp_http_flow_publish_fetching(context->core, &snapshot)) {
        vTaskDelete(context->http_task);
        context->http_task = NULL;
        context->task_started = false;
        vTaskDelete(NULL);
        return;
    }
    (void)publish_snapshot(context, &snapshot);
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0U) > 0U) {
            context->request_in_flight = false;
            if (context->exchange_succeeded) {
                (void)mvp_http_flow_apply_exchange(context->core, &context->exchange,
                                                   monotonic_now_s(), &snapshot);
            } else {
                (void)mvp_http_flow_apply_transport_failure(context->core, &snapshot);
            }
            (void)publish_snapshot(context, &snapshot);
            context->next_poll_monotonic_s = poll_deadline(monotonic_now_s(),
                                                            context->poll_s);
        }

        now_s = monotonic_now_s();
        if (!context->request_in_flight &&
            (context->next_poll_monotonic_s == 0U ||
             now_s >= context->next_poll_monotonic_s)) {
            context->request_in_flight = true;
            xTaskNotifyGive(context->http_task);
        }
        if (mvp_http_flow_tick(context->core, now_s, &snapshot)) {
            (void)publish_snapshot(context, &snapshot);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(NET_TASK_TICK_MS));
    }
}

esp_err_t net_task_start(const runtime_config_t *config,
                         glucose_core_t *core,
                         ui_task_t *ui)
{
    int written;
    BaseType_t created;
    if (config == NULL || core == NULL || ui == NULL || g_net.task_started) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&g_net, 0, sizeof(g_net));
    if (!runtime_config_validate(config, NULL) ||
        !bounded_length(config->bridge_origin, sizeof(config->bridge_origin), &(size_t){0}) ||
        !bounded_length(config->device_token, sizeof(config->device_token), &(size_t){0})) {
        clear_context();
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(g_net.url, sizeof(g_net.url), "%s/api/v2/latest",
                       config->bridge_origin);
    if (written < 0 || (size_t)written >= sizeof(g_net.url)) {
        clear_context();
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(g_net.authorization, sizeof(g_net.authorization),
                       "Bearer %s", config->device_token);
    if (written < 0 || (size_t)written >= sizeof(g_net.authorization)) {
        clear_context();
        return ESP_ERR_INVALID_SIZE;
    }
    g_net.poll_s = runtime_poll_interval_clamp(config->poll_s);
    g_net.core = core;
    g_net.ui = ui;
    created = xTaskCreate(http_task_worker, NET_HTTP_TASK_NAME, NET_HTTP_TASK_STACK,
                          &g_net, NET_HTTP_TASK_PRIORITY, &g_net.http_task);
    if (created != pdPASS) {
        clear_context();
        return ESP_ERR_NO_MEM;
    }
    g_net.task_started = true;
    created = xTaskCreate(net_task_worker, NET_TASK_NAME, NET_TASK_STACK,
                          &g_net, NET_TASK_PRIORITY, &g_net.runtime_task);
    if (created != pdPASS) {
        vTaskDelete(g_net.http_task);
        clear_context();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
