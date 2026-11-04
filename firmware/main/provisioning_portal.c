#include "provisioning_portal.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "config_store.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "provisioning_session.h"
#include "wifi_provisioning_core.h"

#define PORTAL_GOT_IP_BIT BIT0
#define PORTAL_WORKER_STACK 4096U
#define PORTAL_DNS_STACK 3072U
#define PORTAL_TASK_PRIORITY 5U
#define PORTAL_DNS_PORT 53
#define PORTAL_DNS_PACKET_CAPACITY 512U
#define PORTAL_HTTP_CONTENT_TYPE_CAPACITY 64U
#define PORTAL_HTTP_RECV_TIMEOUT_LIMIT 3U

static const char *const TAG = "wifi_setup";
static const char PORTAL_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"Cache-Control\" content=\"no-store\">"
    "<title>Libre Desk Wi-Fi setup</title><style>"
    "body{font:16px system-ui;margin:2rem auto;max-width:28rem;padding:0 1rem;"
    "background:#061010;color:#eff7f7}label{display:block;margin:1rem 0 .35rem}"
    "input,button{box-sizing:border-box;width:100%;padding:.8rem;font:inherit}"
    "button{margin-top:1.2rem}small{color:#9caaaa}</style></head><body>"
    "<h1>Libre Desk Wi-Fi</h1><p>Enter the new 2.4 GHz network. "
    "The device tests it before saving.</p>"
    "<form method=post action=/configure autocomplete=off>"
    "<label for=s>Wi-Fi name</label><input id=s name=ssid maxlength=32 required>"
    "<label for=p>Wi-Fi password</label>"
    "<input id=p name=password type=password maxlength=63 required>"
    "<button type=submit>Test and save</button></form>"
    "<p><small>After submitting, watch the Libre Desk screen. "
    "The temporary network may disconnect while the new network is tested.</small></p>"
    "</body></html>";
static const char PORTAL_ACCEPTED[] =
    "Request accepted. Watch the Libre Desk screen; this page may disconnect.";

typedef struct provisioning_portal_context {
    runtime_config_t current;
    ui_task_t *ui;
    uint32_t session_timeout_ms;
    char ap_ssid[WIFI_PROVISIONING_AP_SSID_CAPACITY];
    char ap_password[WIFI_PROVISIONING_AP_PASSWORD_CAPACITY];
    wifi_provisioning_submission_t pending;
    bool pending_ready;
    bool validating;
    bool started;
    bool dns_stop;
    int dns_socket;
    httpd_handle_t http;
    TaskHandle_t worker_task;
    TaskHandle_t dns_task;
    EventGroupHandle_t events;
    StaticEventGroup_t events_storage;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    bool wifi_initialized;
    bool wifi_handler_registered;
    bool ip_handler_registered;
} provisioning_portal_context_t;

static provisioning_portal_context_t s_portal = {.dns_socket = -1};

static void secure_zero(void *buffer, size_t length)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;
    while (length > 0U) {
        *cursor++ = 0U;
        --length;
    }
}

static bool lock_portal(provisioning_portal_context_t *portal)
{
    return portal->lock != NULL && xSemaphoreTake(portal->lock, portMAX_DELAY) == pdTRUE;
}

static void unlock_portal(provisioning_portal_context_t *portal)
{
    (void)xSemaphoreGive(portal->lock);
}

static esp_err_t send_response(httpd_req_t *request, const char *status,
                               const char *type, const char *body)
{
    esp_err_t result;
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    result = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    return result;
}

static esp_err_t get_handler(httpd_req_t *request)
{
    return send_response(request, "200 OK", "text/html; charset=utf-8",
                         PORTAL_HTML);
}

static bool form_content_type(httpd_req_t *request)
{
    char value[PORTAL_HTTP_CONTENT_TYPE_CAPACITY];
    const char expected[] = "application/x-www-form-urlencoded";
    size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");

    memset(value, 0, sizeof(value));
    if (length == 0U || length >= sizeof(value) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", value,
                                    sizeof(value)) != ESP_OK ||
        strncasecmp(value, expected, sizeof(expected) - 1U) != 0) {
        return false;
    }
    return value[sizeof(expected) - 1U] == '\0' ||
           value[sizeof(expected) - 1U] == ';';
}

static esp_err_t post_handler(httpd_req_t *request)
{
    provisioning_portal_context_t *portal = request->user_ctx;
    uint8_t body[WIFI_PROVISIONING_FORM_MAX_BYTES];
    wifi_provisioning_submission_t submission;
    size_t offset = 0U;
    unsigned timeouts = 0U;
    esp_err_t response_result;

    memset(body, 0, sizeof(body));
    memset(&submission, 0, sizeof(submission));
    if (portal == NULL || request->content_len <= 0 ||
        (size_t)request->content_len > sizeof(body) || !form_content_type(request)) {
        secure_zero(body, sizeof(body));
        return send_response(request, "400 Bad Request", "text/plain", "Invalid request.");
    }

    while (offset < (size_t)request->content_len) {
        int received = httpd_req_recv(request, (char *)&body[offset],
                                      (size_t)request->content_len - offset);
        if (received == HTTPD_SOCK_ERR_TIMEOUT &&
            timeouts++ < PORTAL_HTTP_RECV_TIMEOUT_LIMIT) {
            continue;
        }
        if (received <= 0) {
            secure_zero(body, sizeof(body));
            return ESP_FAIL;
        }
        offset += (size_t)received;
    }

    if (wifi_provisioning_parse_form(body, offset, &submission) !=
        WIFI_PROVISIONING_OK) {
        secure_zero(body, sizeof(body));
        secure_zero(&submission, sizeof(submission));
        return send_response(request, "400 Bad Request", "text/plain", "Invalid request.");
    }
    secure_zero(body, sizeof(body));

    if (!lock_portal(portal)) {
        secure_zero(&submission, sizeof(submission));
        return send_response(request, "503 Service Unavailable", "text/plain",
                             "Setup is temporarily unavailable.");
    }
    if (portal->pending_ready || portal->validating) {
        unlock_portal(portal);
        secure_zero(&submission, sizeof(submission));
        return send_response(request, "409 Conflict", "text/plain",
                             "A network is already being tested.");
    }
    portal->pending = submission;
    portal->pending_ready = true;
    secure_zero(&submission, sizeof(submission));
    unlock_portal(portal);

    response_result = send_response(request, "202 Accepted", "text/plain",
                                    PORTAL_ACCEPTED);
    if (portal->worker_task != NULL) {
        xTaskNotifyGive(portal->worker_task);
    }
    return response_result;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    provisioning_portal_context_t *portal = argument;
    (void)event_data;
    if (portal == NULL || portal->events == NULL) {
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(portal->events, PORTAL_GOT_IP_BIT);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(portal->events, PORTAL_GOT_IP_BIT);
    }
}

static bool dns_question_end(const uint8_t *packet, size_t length,
                             size_t *question_end_out)
{
    size_t cursor = 12U;
    if (packet == NULL || question_end_out == NULL || length < 17U ||
        (packet[2] & UINT8_C(0x80)) != 0U || packet[4] != 0U || packet[5] != 1U) {
        return false;
    }
    while (cursor < length) {
        uint8_t label_length = packet[cursor++];
        if (label_length == 0U) {
            if (cursor + 4U > length) return false;
            *question_end_out = cursor + 4U;
            return true;
        }
        if (label_length > 63U || cursor + label_length > length) return false;
        cursor += label_length;
    }
    return false;
}

static size_t dns_response(const uint8_t *query, size_t query_length,
                           uint8_t *response, size_t response_capacity)
{
    static const uint8_t answer[] = {
        0xc0U, 0x0cU, 0x00U, 0x01U, 0x00U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
        192U, 168U, 4U, 1U,
    };
    size_t question_end;
    if (!dns_question_end(query, query_length, &question_end) ||
        question_end + sizeof(answer) > response_capacity) {
        return 0U;
    }
    memcpy(response, query, question_end);
    response[2] = UINT8_C(0x81);
    response[3] = UINT8_C(0x80);
    response[6] = 0U;
    response[7] = 1U;
    response[8] = 0U;
    response[9] = 0U;
    response[10] = 0U;
    response[11] = 0U;
    memcpy(&response[question_end], answer, sizeof(answer));
    return question_end + sizeof(answer);
}

static void dns_worker(void *argument)
{
    provisioning_portal_context_t *portal = argument;
    uint8_t query[PORTAL_DNS_PACKET_CAPACITY];
    uint8_t response[PORTAL_DNS_PACKET_CAPACITY];

    while (portal != NULL && !portal->dns_stop) {
        struct sockaddr_storage source;
        socklen_t source_length = sizeof(source);
        int received = recvfrom(portal->dns_socket, query, sizeof(query), 0,
                                (struct sockaddr *)&source, &source_length);
        if (received > 0) {
            size_t response_length = dns_response(query, (size_t)received,
                                                  response, sizeof(response));
            if (response_length > 0U) {
                (void)sendto(portal->dns_socket, response, response_length, 0,
                             (struct sockaddr *)&source, source_length);
            }
        }
    }
    secure_zero(query, sizeof(query));
    secure_zero(response, sizeof(response));
    if (portal != NULL) portal->dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns(provisioning_portal_context_t *portal)
{
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int reuse = 1;

    portal->dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (portal->dns_socket < 0) return ESP_FAIL;
    (void)setsockopt(portal->dns_socket, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    (void)setsockopt(portal->dns_socket, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PORTAL_DNS_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(portal->dns_socket, (struct sockaddr *)&address,
             sizeof(address)) != 0) {
        close(portal->dns_socket);
        portal->dns_socket = -1;
        return ESP_FAIL;
    }
    if (xTaskCreate(dns_worker, "setup_dns", PORTAL_DNS_STACK, portal,
                    PORTAL_TASK_PRIORITY, &portal->dns_task) != pdPASS) {
        close(portal->dns_socket);
        portal->dns_socket = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t start_http(provisioning_portal_context_t *portal)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET,
                        .handler = get_handler, .user_ctx = portal};
    httpd_uri_t generate = {.uri = "/generate_204", .method = HTTP_GET,
                            .handler = get_handler, .user_ctx = portal};
    httpd_uri_t hotspot = {.uri = "/hotspot-detect.html", .method = HTTP_GET,
                           .handler = get_handler, .user_ctx = portal};
    httpd_uri_t connect = {.uri = "/connecttest.txt", .method = HTTP_GET,
                           .handler = get_handler, .user_ctx = portal};
    httpd_uri_t configure = {.uri = "/configure", .method = HTTP_POST,
                             .handler = post_handler, .user_ctx = portal};
    esp_err_t error;

    config.max_uri_handlers = 6U;
    config.lru_purge_enable = true;
    error = httpd_start(&portal->http, &config);
    if (error != ESP_OK) return error;
    if ((error = httpd_register_uri_handler(portal->http, &root)) != ESP_OK ||
        (error = httpd_register_uri_handler(portal->http, &generate)) != ESP_OK ||
        (error = httpd_register_uri_handler(portal->http, &hotspot)) != ESP_OK ||
        (error = httpd_register_uri_handler(portal->http, &connect)) != ESP_OK ||
        (error = httpd_register_uri_handler(portal->http, &configure)) != ESP_OK) {
        (void)httpd_stop(portal->http);
        portal->http = NULL;
        return error;
    }
    return ESP_OK;
}

static void stop_services(provisioning_portal_context_t *portal,
                          bool stop_worker)
{
    if (portal->http != NULL) {
        (void)httpd_stop(portal->http);
        portal->http = NULL;
    }
    portal->dns_stop = true;
    if (portal->dns_socket >= 0) {
        shutdown(portal->dns_socket, SHUT_RDWR);
        close(portal->dns_socket);
        portal->dns_socket = -1;
    }
    if (portal->dns_task != NULL) {
        vTaskDelete(portal->dns_task);
        portal->dns_task = NULL;
    }
    if (stop_worker && portal->worker_task != NULL) {
        vTaskDelete(portal->worker_task);
        portal->worker_task = NULL;
    }
    if (portal->ip_handler_registered) {
        (void)esp_event_handler_instance_unregister(IP_EVENT,
                                                     IP_EVENT_STA_GOT_IP,
                                                     portal->ip_handler);
        portal->ip_handler_registered = false;
    }
    if (portal->wifi_handler_registered) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                     WIFI_EVENT_STA_DISCONNECTED,
                                                     portal->wifi_handler);
        portal->wifi_handler_registered = false;
    }
    if (portal->wifi_initialized) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        portal->wifi_initialized = false;
    }
    if (portal->ap_netif != NULL) {
        esp_netif_destroy_default_wifi(portal->ap_netif);
        portal->ap_netif = NULL;
    }
    if (portal->sta_netif != NULL) {
        esp_netif_destroy_default_wifi(portal->sta_netif);
        portal->sta_netif = NULL;
    }
}

static esp_err_t start_wifi(provisioning_portal_context_t *portal)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_config;
    esp_err_t error;

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    portal->ap_netif = esp_netif_create_default_wifi_ap();
    portal->sta_netif = esp_netif_create_default_wifi_sta();
    if (portal->ap_netif == NULL || portal->sta_netif == NULL) return ESP_ERR_NO_MEM;
    error = esp_wifi_init(&init);
    if (error != ESP_OK) return error;
    portal->wifi_initialized = true;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler,
        portal, &portal->wifi_handler);
    if (error != ESP_OK) return error;
    portal->wifi_handler_registered = true;
    error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
        portal, &portal->ip_handler);
    if (error != ESP_OK) return error;
    portal->ip_handler_registered = true;

    memset(&ap_config, 0, sizeof(ap_config));
    memcpy(ap_config.ap.ssid, portal->ap_ssid, strlen(portal->ap_ssid));
    memcpy(ap_config.ap.password, portal->ap_password,
           strlen(portal->ap_password));
    ap_config.ap.ssid_len = (uint8_t)strlen(portal->ap_ssid);
    ap_config.ap.channel = 1U;
    ap_config.ap.max_connection = 4U;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = true;
    error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (error == ESP_OK) error = esp_wifi_start();
    secure_zero(&ap_config, sizeof(ap_config));
    return error;
}

static bool connect_candidate(const runtime_config_t *candidate, void *context)
{
    provisioning_portal_context_t *portal = context;
    wifi_config_t station;
    EventBits_t bits;
    esp_err_t error;

    memset(&station, 0, sizeof(station));
    memcpy(station.sta.ssid, candidate->wifi_ssid,
           strlen(candidate->wifi_ssid));
    memcpy(station.sta.password, candidate->wifi_password,
           strlen(candidate->wifi_password));
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    xEventGroupClearBits(portal->events, PORTAL_GOT_IP_BIT);
    (void)esp_wifi_disconnect();
    error = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (error == ESP_OK) error = esp_wifi_connect();
    secure_zero(&station, sizeof(station));
    if (error != ESP_OK) return false;
    bits = xEventGroupWaitBits(
        portal->events, PORTAL_GOT_IP_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(PROVISIONING_PORTAL_CANDIDATE_TIMEOUT_MS));
    if ((bits & PORTAL_GOT_IP_BIT) == 0U) {
        wifi_config_t cleared;
        (void)esp_wifi_disconnect();
        memset(&cleared, 0, sizeof(cleared));
        (void)esp_wifi_set_config(WIFI_IF_STA, &cleared);
        secure_zero(&cleared, sizeof(cleared));
        return false;
    }
    return true;
}

static config_store_result_t save_candidate(const runtime_config_t *candidate,
                                             void *context)
{
    (void)context;
    return config_store_save(candidate);
}

static config_store_result_t reload_active(runtime_config_t *active_out,
                                           void *context)
{
    (void)context;
    return config_store_load(active_out);
}

static config_store_result_t clear_setup_request(void *context)
{
    (void)context;
    return config_store_setup_request_clear();
}

static void restart_device(void *context)
{
    (void)context;
    esp_restart();
}

static void portal_worker(void *argument)
{
    provisioning_portal_context_t *portal = argument;
    provisioning_session_ops_t session_ops = {
        .connect_candidate = connect_candidate,
        .save_candidate = save_candidate,
        .reload_active = reload_active,
        .clear_setup_request = clear_setup_request,
        .restart = restart_device,
        .context = portal,
    };
    TickType_t started_at = xTaskGetTickCount();
    TickType_t session_ticks = pdMS_TO_TICKS(portal->session_timeout_ms);

    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - started_at;
        TickType_t remaining;
        wifi_provisioning_submission_t submission;
        provisioning_session_result_t result;

        memset(&submission, 0, sizeof(submission));
        if (elapsed >= session_ticks) break;
        remaining = session_ticks - elapsed;
        if (ulTaskNotifyTake(pdTRUE, remaining) == 0U) break;
        if (!lock_portal(portal)) continue;
        if (!portal->pending_ready || portal->validating) {
            unlock_portal(portal);
            continue;
        }
        submission = portal->pending;
        secure_zero(&portal->pending, sizeof(portal->pending));
        portal->pending_ready = false;
        portal->validating = true;
        unlock_portal(portal);

        result = provisioning_session_apply(&portal->current, &submission,
                                            &session_ops);
        secure_zero(&submission, sizeof(submission));
        if (result == PROVISIONING_SESSION_RESULT_OK) {
            vTaskDelete(NULL);
        }
        ESP_LOGW(TAG, "candidate setup did not complete (code %d)", (int)result);
        if (lock_portal(portal)) {
            portal->validating = false;
            unlock_portal(portal);
        }
        (void)ui_task_publish_provisioning(
            portal->ui, portal->ap_ssid, sizeof(portal->ap_ssid),
            portal->ap_password, sizeof(portal->ap_password));
    }

    ESP_LOGI(TAG, "setup session expired");
    stop_services(portal, false);
    if (config_store_setup_request_clear() == CONFIG_STORE_RESULT_OK) {
        secure_zero(&portal->current, sizeof(portal->current));
        secure_zero(portal->ap_ssid, sizeof(portal->ap_ssid));
        secure_zero(portal->ap_password, sizeof(portal->ap_password));
        portal->started = false;
        esp_restart();
    }
    ESP_LOGE(TAG, "could not clear setup request after expiry");
    portal->worker_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t provisioning_portal_start(const runtime_config_t *current,
                                    ui_task_t *ui,
                                    uint32_t session_timeout_ms)
{
    provisioning_portal_context_t *portal = &s_portal;
    uint8_t mac[6];
    uint8_t random[WIFI_PROVISIONING_AP_PASSWORD_CAPACITY - 1U];
    esp_err_t error;

    memset(mac, 0, sizeof(mac));
    memset(random, 0, sizeof(random));
    if (current == NULL || ui == NULL || session_timeout_ms == 0U ||
        !runtime_config_validate(current, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (portal->started) return ESP_ERR_INVALID_STATE;

    memset(portal, 0, sizeof(*portal));
    portal->dns_socket = -1;
    portal->current = *current;
    portal->ui = ui;
    portal->session_timeout_ms = session_timeout_ms;
    portal->events = xEventGroupCreateStatic(&portal->events_storage);
    portal->lock = xSemaphoreCreateMutexStatic(&portal->lock_storage);
    if (portal->events == NULL || portal->lock == NULL) {
        error = ESP_ERR_NO_MEM;
        goto failure;
    }
    error = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (error != ESP_OK) goto failure;
    esp_fill_random(random, sizeof(random));
    if (!wifi_provisioning_ap_credentials(
            mac, sizeof(mac), random, sizeof(random),
            portal->ap_ssid, sizeof(portal->ap_ssid),
            portal->ap_password, sizeof(portal->ap_password))) {
        error = ESP_FAIL;
        goto failure;
    }
    secure_zero(random, sizeof(random));
    secure_zero(mac, sizeof(mac));

    error = start_wifi(portal);
    if (error != ESP_OK) goto failure;
    error = start_dns(portal);
    if (error != ESP_OK) goto failure;
    error = start_http(portal);
    if (error != ESP_OK) goto failure;
    if (!ui_task_publish_provisioning(
            ui, portal->ap_ssid, sizeof(portal->ap_ssid),
            portal->ap_password, sizeof(portal->ap_password))) {
        error = ESP_FAIL;
        goto failure;
    }
    portal->started = true;
    if (xTaskCreate(portal_worker, "setup_worker", PORTAL_WORKER_STACK,
                    portal, PORTAL_TASK_PRIORITY,
                    &portal->worker_task) != pdPASS) {
        portal->started = false;
        error = ESP_ERR_NO_MEM;
        goto failure;
    }
    ESP_LOGI(TAG, "temporary local Wi-Fi setup is active");
    return ESP_OK;

failure:
    secure_zero(random, sizeof(random));
    secure_zero(mac, sizeof(mac));
    stop_services(portal, true);
    secure_zero(&portal->current, sizeof(portal->current));
    secure_zero(&portal->pending, sizeof(portal->pending));
    secure_zero(portal->ap_ssid, sizeof(portal->ap_ssid));
    secure_zero(portal->ap_password, sizeof(portal->ap_password));
    portal->started = false;
    return error;
}
