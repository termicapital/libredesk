#include "ui_task_host.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; } } while (0)

static runtime_ui_snapshot_t snapshot_for(glucose_display_state_t state)
{
    glucose_core_t core;
    glucose_freshness_config_t freshness = {360U, 900U, 3600U};
    runtime_ui_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(glucose_core_init(&core, &freshness));
    core.state = state;
    if (state == GLUCOSE_STATE_FRESH || state == GLUCOSE_STATE_AGING) {
        core.has_reading = true;
        core.reading.value_mg_dl = 112.0;
        core.reading.unit = GLUCOSE_UNIT_MG_DL;
        core.reading.trend = GLUCOSE_TREND_STABLE;
        core.reading.range = GLUCOSE_RANGE_IN_RANGE;
        core.reading.libre_local_seconds = state == GLUCOSE_STATE_FRESH
            ? 9U * 3600U + 38U * 60U
            : 20U * 3600U + 56U * 60U;
        core.current_age_s = 10U;
    }
    CHECK(runtime_ui_snapshot_build(&core, &snapshot));
    return snapshot;
}

static ui_task_t *start(ui_host_stats_t *stats, ui_host_failure_t failure,
                        lv_display_t *display)
{
    ui_task_t *task = NULL;
    ui_host_reset(stats, failure);
    stats->display = display;
    CHECK(ui_task_start(&task, display) == ESP_OK);
    return task;
}

static bool step(ui_task_t *task)
{
    ui_host_step_begin();
    (void)task;
    return ui_host_step_end();
}

static void test_queue_contract_and_publish_ownership(void)
{
    ui_host_stats_t stats;
    lv_display_t *display = ui_host_display(0U);
    ui_task_t *task = start(&stats, UI_HOST_OK, display);
    runtime_ui_snapshot_t a = snapshot_for(GLUCOSE_STATE_FRESH);
    runtime_ui_snapshot_t b = snapshot_for(GLUCOSE_STATE_AGING);
    unsigned creates = stats.screen_creates;
    CHECK(task != NULL && stats.queue_length == 1U);
    CHECK(!ui_task_publish(NULL, &a));
    CHECK(!ui_task_publish(task, NULL));
    CHECK(ui_task_publish(task, &a));
    CHECK(ui_task_publish(task, &b));
    CHECK(stats.queue_overwrites == 1U);
    CHECK(stats.publish_wait_ticks == 0U);
    CHECK(stats.lock_held_during_publish == 0U);
    CHECK(stats.screen_creates == creates);
    CHECK(stats.queue_receives == 0U);
    CHECK(step(task));
    CHECK(stats.queue_receives == 2U);
    CHECK(stats.active_screen == UI_SCREEN_AGING);
    CHECK(strcmp(stats.last_clock_text, "20:56") == 0);
    CHECK(stats.lock_acquires == stats.lock_releases);

    CHECK(!ui_task_publish_provisioning(NULL, "LibreDesk-123456",
                                        sizeof("LibreDesk-123456"),
                                        "AAAAAAAAAAAA", sizeof("AAAAAAAAAAAA")));
    CHECK(!ui_task_publish_provisioning(task, NULL, 1U,
                                        "AAAAAAAAAAAA", sizeof("AAAAAAAAAAAA")));
    CHECK(!ui_task_publish_provisioning(task, "LibreDesk-123456",
                                        sizeof("LibreDesk-123456"), NULL, 1U));
    CHECK(ui_task_publish_provisioning(task, "LibreDesk-123456",
                                       sizeof("LibreDesk-123456"),
                                       "AAAAAAAAAAAA", sizeof("AAAAAAAAAAAA")));
    CHECK(stats.lock_held_during_publish == 0U);
    CHECK(step(task));
    CHECK(stats.active_screen == UI_SCREEN_PROVISIONING);
    CHECK(strcmp(stats.last_ssid, "LibreDesk-123456") == 0);
    CHECK(strcmp(stats.last_setup_password, "AAAAAAAAAAAA") == 0);
    CHECK(stats.last_clock_text[0] == '\0');
    CHECK(stats.lock_acquires == stats.lock_releases);
}

static void test_copy_candidate_before_old_and_display_routing(void)
{
    ui_host_stats_t stats;
    lv_display_t *other = ui_host_display(0U);
    lv_display_t *supplied = ui_host_display(1U);
    ui_task_t *task;
    runtime_ui_snapshot_t fresh = snapshot_for(GLUCOSE_STATE_FRESH);
    lv_display_set_default(other);
    ui_host_reset(&stats, UI_HOST_OK);
    lv_display_set_default(other);
    stats.display = supplied;
    task = NULL;
    CHECK(ui_task_start(&task, supplied) == ESP_OK);
    CHECK(task != NULL);
    CHECK(stats.create_display == supplied);
    CHECK(stats.load_display == supplied);
    CHECK(stats.active_lookup_display == supplied);
    CHECK(lv_display_get_default() == other);
    CHECK(ui_task_publish(task, &fresh) && step(task));
    CHECK(stats.max_live_objects == 2U);
    CHECK(stats.load_display == supplied);
    CHECK(stats.display_mismatches == 0U);
    CHECK(lv_display_get_default() == other);
}

static void test_invalid_and_all_runtime_fallbacks(void)
{
    const ui_host_failure_t failures_to_test[] = {
        UI_HOST_FAIL_MODEL, UI_HOST_FAIL_ALLOCATION, UI_HOST_FAIL_RENDER,
        UI_HOST_FAIL_NEUTRAL, UI_HOST_FAIL_LOAD
    };
    size_t index;
    runtime_ui_snapshot_t fresh = snapshot_for(GLUCOSE_STATE_FRESH);
    for (index = 0U; index < sizeof(failures_to_test) / sizeof(failures_to_test[0]); index++) {
        ui_host_stats_t stats;
        ui_task_t *task = start(&stats, failures_to_test[index], ui_host_display(0U));
        CHECK(task != NULL);
        CHECK(ui_task_publish(task, &fresh) && step(task));
        CHECK(stats.queue_receives == 2U);
        CHECK(stats.lock_acquires == stats.lock_releases);
        CHECK(stats.live_objects == 0U || stats.active_screen == UI_SCREEN_NO_DATA);
        if (failures_to_test[index] == UI_HOST_FAIL_NEUTRAL ||
            failures_to_test[index] == UI_HOST_FAIL_LOAD) {
            CHECK(stats.cleans == 1U);
        } else {
            CHECK(stats.active_screen == UI_SCREEN_NO_DATA);
        }
    }

    {
        ui_host_stats_t stats;
        ui_task_t *task = start(&stats, UI_HOST_OK, ui_host_display(0U));
        runtime_ui_snapshot_t invalid = fresh;
        invalid.model.value_text[0] = 'x';
        CHECK(!runtime_ui_snapshot_is_valid(&invalid));
        CHECK(ui_task_publish(task, &invalid) && step(task));
        CHECK(stats.active_screen == UI_SCREEN_NO_DATA);
    }
}

static void test_start_failures_release_resources(void)
{
    ui_host_stats_t stats;
    ui_task_t *task = NULL;
    ui_host_reset(&stats, UI_HOST_FAIL_QUEUE);
    CHECK(ui_task_start(&task, ui_host_display(0U)) == ESP_ERR_NO_MEM);
    CHECK(stats.queue_creates == 1U && stats.queue_deletes == 0U);
    ui_host_reset(&stats, UI_HOST_FAIL_START_LOCK);
    CHECK(ui_task_start(&task, ui_host_display(0U)) == ESP_ERR_TIMEOUT);
    CHECK(stats.queue_deletes == 1U && stats.lock_acquires == 1U);
    ui_host_reset(&stats, UI_HOST_FAIL_START_NEUTRAL);
    CHECK(ui_task_start(&task, ui_host_display(0U)) == ESP_OK);
    /* Startup neutral failure is a fail-closed blank, not a start failure. */
    CHECK(stats.cleans == 1U);
    ui_host_reset(&stats, UI_HOST_FAIL_TASK);
    CHECK(ui_task_start(&task, ui_host_display(0U)) == ESP_ERR_NO_MEM);
    CHECK(stats.queue_deletes == 1U && stats.lock_acquires == stats.lock_releases);
    CHECK(ui_task_start(NULL, ui_host_display(0U)) == ESP_ERR_INVALID_ARG);
    CHECK(ui_task_start(&task, NULL) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_queue_contract_and_publish_ownership();
    test_copy_candidate_before_old_and_display_routing();
    test_invalid_and_all_runtime_fallbacks();
    test_start_failures_release_resources();
    if (failures != 0U) {
        fprintf(stderr, "%u failures\n", failures);
        return 1;
    }
    puts("authentic production ui_task host tests passed");
    return 0;
}
