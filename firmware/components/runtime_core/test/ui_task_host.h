#ifndef UI_TASK_HOST_H
#define UI_TASK_HOST_H

#include <stdbool.h>
#include "runtime_core.h"
#include "ui_screens.h"
#include "ui_task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_host_failure {
    UI_HOST_OK = 0,
    UI_HOST_FAIL_QUEUE,
    UI_HOST_FAIL_TASK,
    UI_HOST_FAIL_START_LOCK,
    UI_HOST_FAIL_START_NEUTRAL,
    UI_HOST_FAIL_MODEL,
    UI_HOST_FAIL_ALLOCATION,
    UI_HOST_FAIL_RENDER,
    UI_HOST_FAIL_NEUTRAL,
    UI_HOST_FAIL_LOAD
} ui_host_failure_t;

typedef struct ui_host_stats {
    unsigned queue_creates;
    unsigned queue_deletes;
    unsigned queue_length;
    unsigned queue_overwrites;
    unsigned publish_wait_ticks;
    unsigned queue_receives;
    unsigned task_creates;
    unsigned lock_acquires;
    unsigned lock_releases;
    unsigned lock_held_during_publish;
    unsigned screen_creates;
    unsigned screen_create_failures;
    unsigned screen_loads;
    unsigned screen_load_failures;
    unsigned deletes;
    unsigned cleans;
    unsigned live_objects;
    unsigned max_live_objects;
    unsigned display_mismatches;
    const void *display;
    const void *create_display;
    const void *load_display;
    const void *active_lookup_display;
    ui_screen_kind_t active_screen;
    char last_ssid[UI_SCREEN_SSID_MAX_BYTES + 1U];
    char last_setup_password[UI_SCREEN_SETUP_PASSWORD_CAPACITY];
    char last_clock_text[RUNTIME_CLOCK_TEXT_CAPACITY];
} ui_host_stats_t;

void ui_host_reset(ui_host_stats_t *stats, ui_host_failure_t failure);
lv_display_t *ui_host_display(unsigned id);
ui_host_failure_t ui_host_failure(void);
void ui_host_step_begin(void);
bool ui_host_step_end(void);
void ui_host_note_publish_lock_violation(void);

#ifdef __cplusplus
}
#endif

#endif
