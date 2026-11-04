#ifndef LIBRE_DESK_CONFIG_STORE_HOST_H
#define LIBRE_DESK_CONFIG_STORE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

#define TASK7_HOST_MAX_TRACE 2048U
#define TASK7_HOST_MAX_KEY 16U
#define TASK7_HOST_MAX_STRING 320U

typedef enum task7_host_trace_kind {
    TASK7_TRACE_FLASH_INIT = 0,
    TASK7_TRACE_FLASH_ERASE,
    TASK7_TRACE_OPEN,
    TASK7_TRACE_CLOSE,
    TASK7_TRACE_GET_U8,
    TASK7_TRACE_GET_U16,
    TASK7_TRACE_GET_U32,
    TASK7_TRACE_GET_STR_QUERY,
    TASK7_TRACE_GET_STR,
    TASK7_TRACE_ERASE_KEY,
    TASK7_TRACE_SET_U8,
    TASK7_TRACE_SET_U16,
    TASK7_TRACE_SET_U32,
    TASK7_TRACE_SET_STR,
    TASK7_TRACE_COMMIT,
    TASK7_TRACE_ERASE_ALL,
    TASK7_TRACE_GPIO_CONFIG,
    TASK7_TRACE_TASK_CREATE,
    TASK7_TRACE_RESTART,
    TASK7_TRACE_CRITICAL_ENTER,
    TASK7_TRACE_CRITICAL_EXIT,
    TASK7_TRACE_DELAY,
    TASK7_TRACE_COUNT
} task7_host_trace_kind_t;

typedef struct task7_host_trace_event {
    task7_host_trace_kind_t kind;
    char namespace_name[TASK7_HOST_MAX_KEY];
    char key[TASK7_HOST_MAX_KEY];
    nvs_type_t type;
    uint32_t value;
    nvs_handle_t handle;
    nvs_open_mode_t mode;
    size_t mutex_depth;
    size_t critical_depth;
} task7_host_trace_event_t;

void task7_host_reset(void);
void task7_host_reset_uninitialized(void);
void task7_host_clear_trace(void);
size_t task7_host_trace_count(void);
const task7_host_trace_event_t *task7_host_trace_at(size_t index);
size_t task7_host_operation_count(void);
void task7_host_fail_at(size_t one_based_operation, esp_err_t error);
void task7_host_fail_at_after(size_t one_based_operation, esp_err_t error);
void task7_host_clear_failure(void);
void task7_host_fail_next(esp_err_t error);
void task7_host_power_cycle(void);
void task7_host_pause_after_operation(size_t one_based_operation);
bool task7_host_wait_until_paused(void);
void task7_host_release_pause(void);
void task7_host_set_mutex_failures(bool create_fail, bool take_fail, bool give_fail);
void task7_host_set_mutex_take_fail_once(void);
size_t task7_host_mutex_contention(void);
size_t task7_host_max_transaction_owners(void);

void task7_host_seed_u8(const char *key, uint8_t value);
void task7_host_seed_setup_u8(const char *key, uint8_t value);
void task7_host_seed_setup_u16(const char *key, uint16_t value);
bool task7_host_read_setup_u8(const char *key, uint8_t *value_out);
void task7_host_seed_u16(const char *key, uint16_t value);
void task7_host_seed_u32(const char *key, uint32_t value);
void task7_host_seed_str(const char *key, const char *value);
void task7_host_remove(const char *key);
bool task7_host_read_u8(const char *key, uint8_t *value);
bool task7_host_read_u16(const char *key, uint16_t *value);
bool task7_host_read_u32(const char *key, uint32_t *value);
bool task7_host_read_str(const char *key, char *value, size_t capacity);
void task7_host_seed_wrong_type_u16(const char *key, uint16_t value);
void task7_host_seed_wrong_type_u32(const char *key, uint32_t value);
void task7_host_seed_oversized_str(const char *key, size_t length);

void task7_host_set_init_results(esp_err_t first, esp_err_t recovery_reinit);
void task7_host_set_flash_erase_result(esp_err_t result);
size_t task7_host_global_erase_count(void);
size_t task7_host_namespace_erase_count(void);

void task7_host_set_gpio_config_result(esp_err_t result);
void task7_host_set_task_create_fail(bool fail);
void task7_host_set_gpio_samples(const int *levels, const int64_t *times, size_t count);
int task7_host_run_created_task(void);
size_t task7_host_restart_count(void);
size_t task7_host_delay_count(void);
size_t task7_host_critical_depth(void);
BaseType_t task7_host_created_task_priority(void);
uint32_t task7_host_created_task_stack_depth(void);
uint64_t task7_host_gpio_mask(void);
int task7_host_gpio_mode(void);
int task7_host_gpio_pull_up(void);
int task7_host_gpio_pull_down(void);
int task7_host_gpio_interrupt(void);

#endif
