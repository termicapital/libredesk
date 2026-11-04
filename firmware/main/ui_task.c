#include "ui_task.h"

#include <stdlib.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ui_screens.h"

struct ui_task {
    QueueHandle_t queue;
    lv_display_t *display;
    lv_obj_t *active;
};

static ui_screen_model_t screen_model(const runtime_ui_snapshot_t *snapshot)
{
    ui_screen_model_t model;
    memset(&model, 0, sizeof(model));
    model.kind = snapshot->screen;
    model.glucose = snapshot->model;
    (void)memcpy(model.clock_text, snapshot->clock_text, sizeof(model.clock_text));
    return model;
}

static ui_screen_model_t neutral_model(void)
{
    runtime_ui_snapshot_t snapshot;
    runtime_ui_snapshot_make_neutral(&snapshot);
    return screen_model(&snapshot);
}

static void secure_zero(void *address, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)address;
    while (size > 0U) {
        *bytes = 0U;
        bytes++;
        size--;
    }
}

static bool bounded_copy(char *destination, size_t destination_capacity,
                         const char *source, size_t source_capacity)
{
    const char *terminator;
    size_t length;
    if (destination == NULL || destination_capacity == 0U ||
        source == NULL || source_capacity == 0U) {
        return false;
    }
    terminator = memchr(source, '\0', source_capacity);
    if (terminator == NULL) {
        return false;
    }
    length = (size_t)(terminator - source);
    if (length >= destination_capacity) {
        return false;
    }
    (void)memcpy(destination, source, length + 1U);
    return true;
}

static bool object_belongs_to_display(const lv_obj_t *object, lv_display_t *display)
{
    return object != NULL && display != NULL && lv_obj_get_display(object) == display;
}

static void delete_on_display(lv_obj_t *object, lv_display_t *display)
{
    if (object_belongs_to_display(object, display)) {
        lv_obj_delete(object);
    }
}

static bool load_on_display(lv_obj_t *object, lv_display_t *display)
{
    lv_display_t *previous;
    bool loaded;
    if (!object_belongs_to_display(object, display)) {
        return false;
    }
    /* lv_screen_load() is default-display based in LVGL 9; scope the default
     * selection so a supplied non-default display is the actual target. */
    previous = lv_display_get_default();
    lv_display_set_default(display);
    lv_screen_load(object);
    loaded = lv_display_get_screen_active(display) == object;
    lv_display_set_default(previous);
    return loaded;
}
static lv_obj_t *neutral_screen(ui_task_t *task)
{
    ui_screen_model_t model = neutral_model();
    return ui_screen_create_on_display(&model, task->display);
}

static void blank_active(ui_task_t *task)
{
    if (task->active != NULL) {
        delete_on_display(task->active, task->display);
        task->active = NULL;
    }
    lv_obj_clean(lv_display_get_screen_active(task->display));
}

static void replace_screen(ui_task_t *task, const ui_screen_model_t *published)
{
    ui_screen_model_t model;
    lv_obj_t *candidate;

    if (published == NULL || !ui_screen_model_is_valid(published)) {
        model = neutral_model();
    } else {
        model = *published;
    }
    candidate = ui_screen_create_on_display(&model, task->display);
    if (candidate == NULL && model.kind != UI_SCREEN_NO_DATA) {
        model = neutral_model();
        candidate = ui_screen_create_on_display(&model, task->display);
    }
    if (candidate == NULL) {
        secure_zero(&model, sizeof(model));
        blank_active(task);
        return;
    }
    if (task->active != NULL) {
        delete_on_display(task->active, task->display);
    }
    task->active = candidate;
    if (!load_on_display(candidate, task->display)) {
        task->active = NULL;
        delete_on_display(candidate, task->display);
        blank_active(task);
    }
    secure_zero(&model, sizeof(model));
}

static void ui_task_worker(void *argument)
{
    ui_task_t *task = (ui_task_t *)argument;
    ui_screen_model_t model;

    for (;;) {
        if (xQueueReceive(task->queue, &model, portMAX_DELAY) == pdTRUE) {
            /* esp_lvgl_port takes milliseconds; zero means wait indefinitely. */
            if (lvgl_port_lock(0)) {
                replace_screen(task, &model);
                lvgl_port_unlock();
            }
            secure_zero(&model, sizeof(model));
        }
    }
}

esp_err_t ui_task_start(ui_task_t **task_out, lv_display_t *display)
{
    ui_task_t *task;
    BaseType_t created;

    if (task_out == NULL || display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *task_out = NULL;
    task = calloc(1U, sizeof(*task));
    if (task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task->display = display;
    task->queue = xQueueCreate(1U, sizeof(ui_screen_model_t));
    if (task->queue == NULL) {
        free(task);
        return ESP_ERR_NO_MEM;
    }
    if (lvgl_port_lock(0)) {
        task->active = neutral_screen(task);
        if (task->active != NULL) {
            (void)load_on_display(task->active, task->display);
        } else {
            lv_obj_clean(lv_display_get_screen_active(task->display));
        }
    } else {
        vQueueDelete(task->queue);
        free(task);
        return ESP_ERR_TIMEOUT;
    }
    created = xTaskCreate(ui_task_worker, "ui_task", 4096U, task, 5U, NULL);
    if (created != pdPASS) {
        if (task->active != NULL) {
            delete_on_display(task->active, task->display);
            task->active = NULL;
        }
        lvgl_port_unlock();
        vQueueDelete(task->queue);
        free(task);
        return ESP_ERR_NO_MEM;
    }
    lvgl_port_unlock();
    *task_out = task;
    return ESP_OK;
}

bool ui_task_publish(ui_task_t *task, const runtime_ui_snapshot_t *snapshot)
{
    ui_screen_model_t model;
    bool published;

    if (task == NULL || task->queue == NULL || snapshot == NULL) {
        return false;
    }
    model = runtime_ui_snapshot_is_valid(snapshot)
                ? screen_model(snapshot)
                : neutral_model();
    published = xQueueOverwrite(task->queue, &model) == pdPASS;
    secure_zero(&model, sizeof(model));
    return published;
}

bool ui_task_publish_provisioning(ui_task_t *task,
                                  const char *ssid, size_t ssid_capacity,
                                  const char *password, size_t password_capacity)
{
    ui_screen_model_t model;
    bool published = false;

    memset(&model, 0, sizeof(model));
    if (task != NULL && task->queue != NULL &&
        bounded_copy(model.ssid, sizeof(model.ssid), ssid, ssid_capacity) &&
        bounded_copy(model.setup_password, sizeof(model.setup_password),
                     password, password_capacity)) {
        model.kind = UI_SCREEN_PROVISIONING;
        model.glucose.state = GLUCOSE_STATE_BOOT;
        model.glucose.trend = GLUCOSE_TREND_UNKNOWN;
        model.glucose.range = GLUCOSE_RANGE_UNKNOWN;
        if (ui_screen_model_is_valid(&model)) {
            published = xQueueOverwrite(task->queue, &model) == pdPASS;
        }
    }
    secure_zero(&model, sizeof(model));
    return published;
}
