#include "ui_task_host.h"
#include "esp_lvgl_port.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ui_screens.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

struct lv_display_t { unsigned id; lv_obj_t *active; };
struct lv_obj_t { lv_display_t *display; bool deleted; ui_screen_kind_t kind; };
struct fake_queue { unsigned length; unsigned item_size; bool has_item; unsigned char item[sizeof(ui_screen_model_t)]; };

static ui_host_stats_t *g_stats;
static ui_host_failure_t g_failure;
static bool g_lock_held;
static lv_display_t *g_default_display;
static TaskFunction_t g_worker;
static void *g_worker_argument;
static jmp_buf g_step_jump;
static bool g_step_active;
static lv_display_t g_displays[2];

lv_display_t *ui_host_display(unsigned id)
{
    return &g_displays[id % 2U];
}

void ui_host_reset(ui_host_stats_t *stats, ui_host_failure_t failure)
{
    memset(stats, 0, sizeof(*stats));
    memset(g_displays, 0, sizeof(g_displays));
    g_stats = stats;
    g_failure = failure;
    g_lock_held = false;
    g_default_display = NULL;
    g_worker = NULL;
    g_worker_argument = NULL;
    g_step_active = false;
    stats->display = NULL;
}

ui_host_failure_t ui_host_failure(void) { return g_failure; }
void ui_host_step_begin(void) { g_step_active = true; }
void ui_host_note_publish_lock_violation(void)
{
    if (g_stats != NULL) g_stats->lock_held_during_publish++;
}

bool ui_host_step_end(void)
{
    int jumped;
    if (g_worker == NULL) return false;
    jumped = setjmp(g_step_jump);
    if (jumped == 0) {
        g_worker(g_worker_argument);
    }
    g_step_active = false;
    return jumped != 0;
}

bool lvgl_port_lock(int timeout_ms)
{
    (void)timeout_ms;
    if (g_stats != NULL) g_stats->lock_acquires++;
    if (g_failure == UI_HOST_FAIL_START_LOCK && g_stats != NULL &&
        g_stats->lock_acquires == 1U) return false;
    g_lock_held = true;
    return true;
}

void lvgl_port_unlock(void)
{
    if (g_stats != NULL) g_stats->lock_releases++;
    g_lock_held = false;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct fake_queue *queue;
    if (g_stats != NULL) {
        g_stats->queue_creates++;
        g_stats->queue_length = length;
    }
    if (g_failure == UI_HOST_FAIL_QUEUE) return NULL;
    queue = calloc(1U, sizeof(*queue));
    if (queue != NULL) { queue->length = length; queue->item_size = item_size; }
    return queue;
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    if (g_lock_held) {
        ui_host_note_publish_lock_violation();
        return pdFALSE;
    }
    if (queue == NULL || item == NULL || queue->length != 1U ||
        queue->item_size != sizeof(ui_screen_model_t)) return pdFALSE;
    if (queue->has_item && g_stats != NULL) g_stats->queue_overwrites++;
    memcpy(queue->item, item, queue->item_size);
    queue->has_item = true;
    if (g_stats != NULL) g_stats->publish_wait_ticks = 0U;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    if (g_stats != NULL) g_stats->queue_receives++;
    if (queue == NULL || item == NULL || wait != portMAX_DELAY) return pdFALSE;
    if (queue->has_item) {
        memcpy(item, queue->item, queue->item_size);
        queue->has_item = false;
        return pdTRUE;
    }
    if (g_step_active) longjmp(g_step_jump, 1);
    return pdFALSE;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (g_stats != NULL) g_stats->queue_deletes++;
    free(queue);
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                       void *argument, UBaseType_t priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    if (g_stats != NULL) g_stats->task_creates++;
    if (g_failure == UI_HOST_FAIL_TASK) return 0;
    g_worker = task;
    g_worker_argument = argument;
    return pdPASS;
}

void vTaskDelete(void *task) { (void)task; }

lv_display_t *lv_display_get_default(void) { return g_default_display; }
void lv_display_set_default(lv_display_t *display) { g_default_display = display; }
lv_display_t *lv_obj_get_display(const lv_obj_t *object)
{ return object == NULL || object->deleted ? NULL : object->display; }

lv_obj_t *lv_display_get_screen_active(lv_display_t *display)
{
    if (g_stats != NULL) g_stats->active_lookup_display = display;
    return display == NULL ? NULL : display->active;
}

void lv_screen_load(lv_obj_t *object)
{
    if (g_stats != NULL) {
        g_stats->screen_loads++;
        g_stats->load_display = g_default_display;
        if (object == NULL || object->display != g_default_display) g_stats->display_mismatches++;
    }
    if (g_failure == UI_HOST_FAIL_LOAD) return;
    if (object != NULL && object->display == g_default_display) object->display->active = object;
}

void lv_obj_delete(lv_obj_t *object)
{
    if (object == NULL || object->deleted) return;
    object->deleted = true;
    if (g_stats != NULL) {
        g_stats->deletes++;
        if (g_stats->live_objects > 0U) g_stats->live_objects--;
    }
    free(object);
}

void lv_obj_clean(lv_obj_t *object)
{
    (void)object;
    if (g_stats != NULL) g_stats->cleans++;
}

bool ui_screen_model_is_valid(const ui_screen_model_t *model)
{
    if (model == NULL || model->kind < UI_SCREEN_BOOT || model->kind >= UI_SCREEN_COUNT ||
        memchr(model->ssid, '\0', sizeof(model->ssid)) == NULL ||
        memchr(model->setup_password, '\0', sizeof(model->setup_password)) == NULL ||
        memchr(model->clock_text, '\0', sizeof(model->clock_text)) == NULL) {
        return false;
    }
    if (model->kind == UI_SCREEN_PROVISIONING) {
        return strcmp(model->ssid, "LibreDesk-123456") == 0 &&
               strcmp(model->setup_password, "AAAAAAAAAAAA") == 0;
    }
    return model->setup_password[0] == '\0';
}

lv_obj_t *ui_screen_create_on_display(const ui_screen_model_t *model, lv_display_t *display)
{
    lv_obj_t *object;
    lv_display_t *previous = lv_display_get_default();
    bool neutral = model != NULL && model->kind == UI_SCREEN_NO_DATA;
    if (g_stats != NULL) {
        g_stats->screen_creates++;
        g_stats->create_display = display;
    }
    lv_display_set_default(display);
    if (display == NULL || model == NULL ||
        (g_failure == UI_HOST_FAIL_START_NEUTRAL && neutral && g_stats->screen_creates == 1U) ||
        (g_failure == UI_HOST_FAIL_NEUTRAL &&
         ((neutral && g_stats->screen_creates > 1U) || !neutral)) ||
        (!neutral && (g_failure == UI_HOST_FAIL_MODEL ||
                      g_failure == UI_HOST_FAIL_ALLOCATION || g_failure == UI_HOST_FAIL_RENDER))) {
        lv_display_set_default(previous);
        if (g_stats != NULL) g_stats->screen_create_failures++;
        return NULL;
    }
    object = calloc(1U, sizeof(*object));
    lv_display_set_default(previous);
    if (object == NULL) return NULL;
    object->display = display;
    object->kind = model->kind;
    if (g_stats != NULL) {
        g_stats->live_objects++;
        if (g_stats->live_objects > g_stats->max_live_objects)
            g_stats->max_live_objects = g_stats->live_objects;
        g_stats->active_screen = model->kind;
        (void)strcpy(g_stats->last_ssid, model->ssid);
        (void)strcpy(g_stats->last_setup_password, model->setup_password);
        (void)strcpy(g_stats->last_clock_text, model->clock_text);
    }
    return object;
}
