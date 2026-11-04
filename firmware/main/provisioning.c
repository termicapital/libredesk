#include "provisioning.h"

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESET_MONITOR_STACK_DEPTH 1024U
#define RESET_MONITOR_SAMPLE_MS 20U
#define RESET_MONITOR_SETUP_HOLD_US INT64_C(5000000)

#if !BOARD_BOOT_BUTTON_ACTIVE_LOW
#error "Task 7 reset gesture is frozen to active-low BOOT"
#endif

static StaticTask_t s_reset_monitor_task_buffer;
static StackType_t s_reset_monitor_stack[RESET_MONITOR_STACK_DEPTH];
typedef enum reset_monitor_state {
    RESET_MONITOR_STOPPED = 0,
    RESET_MONITOR_STARTING,
    RESET_MONITOR_STARTED
} reset_monitor_state_t;

static reset_monitor_state_t s_reset_monitor_state;
static portMUX_TYPE s_reset_monitor_lock = portMUX_INITIALIZER_UNLOCKED;

#ifdef TASK7_HOST_TEST
void provisioning_test_reset(void)
{
    s_reset_monitor_state = RESET_MONITOR_STOPPED;
}
#endif

static void reset_monitor_task(void *argument)
{
    bool holding = false;
    bool require_release = false;
    bool have_previous_time = false;
    int64_t hold_start = 0;
    int64_t previous_time = 0;

    (void)argument;
    for (;;) {
        const int level = gpio_get_level(BOARD_BOOT_BUTTON_PIN);
        const int64_t now = esp_timer_get_time();

        if (now < 0LL || (have_previous_time && now < previous_time)) {
            holding = false;
            have_previous_time = false;
            vTaskDelay(pdMS_TO_TICKS(RESET_MONITOR_SAMPLE_MS));
            continue;
        }
        previous_time = now;
        have_previous_time = true;
        if (level != 0 && level != 1) {
            holding = false;
            vTaskDelay(pdMS_TO_TICKS(RESET_MONITOR_SAMPLE_MS));
            continue;
        }
        if (level == 1) {
            holding = false;
            require_release = false;
            vTaskDelay(pdMS_TO_TICKS(RESET_MONITOR_SAMPLE_MS));
            continue;
        }
        if (require_release) {
            vTaskDelay(pdMS_TO_TICKS(RESET_MONITOR_SAMPLE_MS));
            continue;
        }
        if (!holding) {
            holding = true;
            hold_start = now;
        } else if (now - hold_start >= RESET_MONITOR_SETUP_HOLD_US) {
            holding = false;
            if (config_store_setup_request_set() == CONFIG_STORE_RESULT_OK) {
                esp_restart();
                return;
            }
            require_release = true;
        }
        vTaskDelay(pdMS_TO_TICKS(RESET_MONITOR_SAMPLE_MS));
    }
}

provisioning_result_t provisioning_reset_monitor_start(void)
{
    const gpio_config_t gpio = {
        .pin_bit_mask = UINT64_C(1) << BOARD_BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    TaskHandle_t task_handle;
    esp_err_t error;

    taskENTER_CRITICAL(&s_reset_monitor_lock);
    if (s_reset_monitor_state != RESET_MONITOR_STOPPED) {
        taskEXIT_CRITICAL(&s_reset_monitor_lock);
        return PROVISIONING_RESULT_ALREADY_STARTED;
    }
    s_reset_monitor_state = RESET_MONITOR_STARTING;
    taskEXIT_CRITICAL(&s_reset_monitor_lock);

    error = gpio_config(&gpio);
    if (error == ESP_OK) {
        task_handle = xTaskCreateStatic(
            reset_monitor_task,
            "reset_monitor",
            RESET_MONITOR_STACK_DEPTH,
            NULL,
            tskIDLE_PRIORITY + 1U,
            s_reset_monitor_stack,
            &s_reset_monitor_task_buffer);
    } else {
        task_handle = NULL;
    }

    taskENTER_CRITICAL(&s_reset_monitor_lock);
    if (task_handle == NULL) {
        s_reset_monitor_state = RESET_MONITOR_STOPPED;
        taskEXIT_CRITICAL(&s_reset_monitor_lock);
        return PROVISIONING_RESULT_IO_ERROR;
    }
    s_reset_monitor_state = RESET_MONITOR_STARTED;
    taskEXIT_CRITICAL(&s_reset_monitor_lock);
    return PROVISIONING_RESULT_OK;
}
