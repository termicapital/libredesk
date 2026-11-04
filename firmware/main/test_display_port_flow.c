#include "display_port_flow.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TRACE_CAPACITY 128U

typedef struct fake_executor {
    display_port_flow_operation_t trace[TRACE_CAPACITY];
    size_t trace_count;
    display_port_flow_operation_t fail_operation;
    int32_t fail_code;
    int fail_enabled;
    display_port_flow_operation_t cleanup_fail_operation;
    int32_t cleanup_fail_code;
    int cleanup_fail_enabled;
} fake_executor_t;

static int32_t fake_execute(void *context,
                            display_port_flow_operation_t operation)
{
    fake_executor_t *fake = context;
    assert(fake != NULL);
    assert(fake->trace_count < TRACE_CAPACITY);
    fake->trace[fake->trace_count++] = operation;
    if (fake->fail_enabled && operation == fake->fail_operation) {
        return fake->fail_code;
    }
    if (fake->cleanup_fail_enabled &&
        operation == fake->cleanup_fail_operation) {
        return fake->cleanup_fail_code;
    }
    return 0;
}

static void reset_fake(fake_executor_t *fake)
{
    *fake = (fake_executor_t){
        .fail_operation = DISPLAY_FLOW_OPERATION_COUNT,
    };
}

static void expect_trace(const fake_executor_t *fake,
                        const display_port_flow_operation_t *expected,
                        size_t expected_count)
{
    assert(fake->trace_count == expected_count);
    for (size_t index = 0U; index < expected_count; ++index) {
        assert(fake->trace[index] == expected[index]);
    }
}

static size_t success_trace(display_port_flow_operation_t *expected,
                            int acceptance)
{
    const display_port_flow_operation_t common[] = {
        DISPLAY_FLOW_BACKLIGHT_INIT_OFF,
        DISPLAY_FLOW_SHARED_SD_CS_HIGH,
        DISPLAY_FLOW_CREATE_SEMAPHORE,
        DISPLAY_FLOW_SPI_BUS_INIT,
        DISPLAY_FLOW_PANEL_IO_CREATE,
        DISPLAY_FLOW_ST7789_CREATE,
        DISPLAY_FLOW_PANEL_RESET,
        DISPLAY_FLOW_PANEL_INIT,
        DISPLAY_FLOW_PANEL_SET_GAP,
        DISPLAY_FLOW_PANEL_SWAP_XY,
        DISPLAY_FLOW_PANEL_MIRROR,
        DISPLAY_FLOW_PANEL_INVERT,
        DISPLAY_FLOW_ALLOCATE_FRAME,
        DISPLAY_FLOW_PRESENT_BLACK,
        DISPLAY_FLOW_PANEL_DISPLAY_ON,
        DISPLAY_FLOW_SET_INITIAL_BRIGHTNESS,
    };
    size_t count = sizeof(common) / sizeof(common[0]);
    for (size_t index = 0U; index < count; ++index) {
        expected[index] = common[index];
    }
    if (acceptance) {
        expected[count++] = DISPLAY_FLOW_PRESENT_RED;
        expected[count++] = DISPLAY_FLOW_PRESENT_GREEN;
        expected[count++] = DISPLAY_FLOW_PRESENT_BLUE;
        expected[count++] = DISPLAY_FLOW_PRESENT_WHITE;
    }
    const display_port_flow_operation_t tail[] = {
        DISPLAY_FLOW_FREE_FRAME,
        DISPLAY_FLOW_LVGL_INIT,
        DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF,
        DISPLAY_FLOW_LVGL_ADD_DISPLAY,
        DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE,
        DISPLAY_FLOW_LVGL_LOCK,
        DISPLAY_FLOW_LVGL_ROTATE_90,
        DISPLAY_FLOW_ACTIVE_SCREEN_BLACK,
    };
    const size_t tail_count = sizeof(tail) / sizeof(tail[0]);
    for (size_t index = 0U; index < tail_count; ++index) {
        expected[count++] = tail[index];
    }
    if (acceptance) {
        expected[count++] = DISPLAY_FLOW_ACCEPTANCE_CARD;
    }
    expected[count++] = DISPLAY_FLOW_LVGL_UNLOCK;
    expected[count++] = DISPLAY_FLOW_PUBLISH_DISPLAY;
    return count;
}

static void test_success_traces(void)
{
    for (int acceptance = 0; acceptance <= 1; ++acceptance) {
        fake_executor_t fake;
        display_port_flow_operation_t expected[TRACE_CAPACITY];
        reset_fake(&fake);
        const size_t expected_count = success_trace(expected, acceptance);
        assert(display_port_flow_run(acceptance != 0, fake_execute, &fake) == 0);
        expect_trace(&fake, expected, expected_count);
    }
}

static void test_publish_marker_is_non_failing(void)
{
    fake_executor_t fake;
    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_PUBLISH_DISPLAY;
    fake.fail_code = 4242;
    assert(display_port_flow_run(false, fake_execute, &fake) == 0);
    assert(fake.trace[fake.trace_count - 1U] == DISPLAY_FLOW_PUBLISH_DISPLAY);
}

static void test_null_executor_is_rejected(void)
{
    assert(display_port_flow_run(false, NULL, NULL) == -1);
}

static void assert_failure_has_no_publish(const fake_executor_t *fake)
{
    for (size_t index = 0U; index < fake->trace_count; ++index) {
        assert(fake->trace[index] != DISPLAY_FLOW_PUBLISH_DISPLAY);
    }
}

static void test_every_fallible_operation_preserves_error(void)
{
    const display_port_flow_operation_t setup_failures[] = {
        DISPLAY_FLOW_BACKLIGHT_INIT_OFF, DISPLAY_FLOW_SHARED_SD_CS_HIGH,
        DISPLAY_FLOW_CREATE_SEMAPHORE, DISPLAY_FLOW_SPI_BUS_INIT,
        DISPLAY_FLOW_PANEL_IO_CREATE, DISPLAY_FLOW_ST7789_CREATE,
        DISPLAY_FLOW_PANEL_RESET, DISPLAY_FLOW_PANEL_INIT,
        DISPLAY_FLOW_PANEL_SET_GAP, DISPLAY_FLOW_PANEL_SWAP_XY,
        DISPLAY_FLOW_PANEL_MIRROR, DISPLAY_FLOW_PANEL_INVERT,
        DISPLAY_FLOW_ALLOCATE_FRAME, DISPLAY_FLOW_PANEL_DISPLAY_ON,
        DISPLAY_FLOW_SET_INITIAL_BRIGHTNESS, DISPLAY_FLOW_FREE_FRAME,
        DISPLAY_FLOW_LVGL_INIT, DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF,
    };
    for (size_t index = 0U; index < sizeof(setup_failures) / sizeof(setup_failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = setup_failures[index];
        fake.fail_code = 12000 + (int32_t)index;
        assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
        assert_failure_has_no_publish(&fake);
    }

    const display_port_flow_operation_t acceptance_failures[] = {
        DISPLAY_FLOW_PRESENT_RED, DISPLAY_FLOW_PRESENT_GREEN,
        DISPLAY_FLOW_PRESENT_BLUE, DISPLAY_FLOW_PRESENT_WHITE,
        DISPLAY_FLOW_ACCEPTANCE_CARD,
    };
    for (size_t index = 0U; index < sizeof(acceptance_failures) / sizeof(acceptance_failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = acceptance_failures[index];
        fake.fail_code = 12100 + (int32_t)index;
        assert(display_port_flow_run(true, fake_execute, &fake) == fake.fail_code);
        assert_failure_has_no_publish(&fake);
    }

    const display_port_flow_operation_t post_handoff_failures[] = {
        DISPLAY_FLOW_LVGL_ADD_DISPLAY, DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE,
        DISPLAY_FLOW_LVGL_LOCK, DISPLAY_FLOW_LVGL_ROTATE_90,
        DISPLAY_FLOW_ACTIVE_SCREEN_BLACK, DISPLAY_FLOW_LVGL_UNLOCK,
    };
    for (size_t index = 0U; index < sizeof(post_handoff_failures) / sizeof(post_handoff_failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = post_handoff_failures[index];
        fake.fail_code = 12200 + (int32_t)index;
        assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
        assert_failure_has_no_publish(&fake);
    }
}

static void test_cleanup_errors_are_ignored_without_masking_first_error(void)
{
    const display_port_flow_operation_t cleanup_failures[] = {
        DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT,
        DISPLAY_FLOW_CLEANUP_PANEL_DELETE, DISPLAY_FLOW_CLEANUP_PANEL_IO_DELETE,
        DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE, DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE,
        DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT,
    };
    for (size_t index = 0U; index < sizeof(cleanup_failures) / sizeof(cleanup_failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = DISPLAY_FLOW_PANEL_INIT;
        fake.fail_code = 12300;
        fake.cleanup_fail_enabled = 1;
        fake.cleanup_fail_operation = cleanup_failures[index];
        fake.cleanup_fail_code = 12400 + (int32_t)index;
        assert(display_port_flow_run(false, fake_execute, &fake) == 12300);
    }

    fake_executor_t fake;
    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_SET_INITIAL_BRIGHTNESS;
    fake.fail_code = 12301;
    fake.cleanup_fail_enabled = 1;
    fake.cleanup_fail_operation = DISPLAY_FLOW_CLEANUP_PANEL_OFF_BEST_EFFORT;
    fake.cleanup_fail_code = 12499;
    assert(display_port_flow_run(false, fake_execute, &fake) == 12301);

    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF;
    fake.fail_code = 12302;
    fake.cleanup_fail_enabled = 1;
    fake.cleanup_fail_operation = DISPLAY_FLOW_CLEANUP_LVGL_DEINIT;
    fake.cleanup_fail_code = 12500;
    assert(display_port_flow_run(false, fake_execute, &fake) == 12302);

    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_PANEL_DISPLAY_ON;
    fake.fail_code = 12303;
    fake.cleanup_fail_enabled = 1;
    fake.cleanup_fail_operation = DISPLAY_FLOW_CLEANUP_FREE_FRAME;
    fake.cleanup_fail_code = 12501;
    assert(display_port_flow_run(false, fake_execute, &fake) == 12303);

    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_LVGL_ROTATE_90;
    fake.fail_code = 12304;
    fake.cleanup_fail_enabled = 1;
    fake.cleanup_fail_operation = DISPLAY_FLOW_CLEANUP_LVGL_UNLOCK;
    fake.cleanup_fail_code = 12502;
    assert(display_port_flow_run(false, fake_execute, &fake) == 12304);
}

static void test_direct_present_failure_quarantines(void)
{
    const display_port_flow_operation_t present_ops[] = {
        DISPLAY_FLOW_PRESENT_BLACK,
        DISPLAY_FLOW_PRESENT_RED,
        DISPLAY_FLOW_PRESENT_GREEN,
        DISPLAY_FLOW_PRESENT_BLUE,
        DISPLAY_FLOW_PRESENT_WHITE,
    };
    for (size_t index = 0U; index < sizeof(present_ops) / sizeof(present_ops[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = present_ops[index];
        fake.fail_code = 7000 + (int32_t)index;
        assert(display_port_flow_run(index != 0U, fake_execute, &fake) == fake.fail_code);
        assert(fake.trace[fake.trace_count - 2U] ==
               DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT);
        assert(fake.trace[fake.trace_count - 1U] ==
               DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT);
        for (size_t trace_index = 0U; trace_index < fake.trace_count; ++trace_index) {
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_PANEL_DELETE);
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_FREE_FRAME);
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE);
        }
    }
}

static void test_add_display_failure_quarantines(void)
{
    fake_executor_t fake;
    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_LVGL_ADD_DISPLAY;
    fake.fail_code = 8123;
    assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
    assert(fake.trace[fake.trace_count - 2U] ==
           DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT);
    assert(fake.trace[fake.trace_count - 1U] ==
           DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT);
    for (size_t index = 0U; index < fake.trace_count; ++index) {
        assert(fake.trace[index] != DISPLAY_FLOW_CLEANUP_PANEL_DELETE);
        assert(fake.trace[index] != DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE);
    }
}

static void test_pre_handoff_failure_preserves_first_error_and_reverses(void)
{
    fake_executor_t fake;
    reset_fake(&fake);
    fake.fail_enabled = 1;
    fake.fail_operation = DISPLAY_FLOW_PANEL_INIT;
    fake.fail_code = 9001;
    assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
    const display_port_flow_operation_t expected[] = {
        DISPLAY_FLOW_BACKLIGHT_INIT_OFF,
        DISPLAY_FLOW_SHARED_SD_CS_HIGH,
        DISPLAY_FLOW_CREATE_SEMAPHORE,
        DISPLAY_FLOW_SPI_BUS_INIT,
        DISPLAY_FLOW_PANEL_IO_CREATE,
        DISPLAY_FLOW_ST7789_CREATE,
        DISPLAY_FLOW_PANEL_RESET,
        DISPLAY_FLOW_PANEL_INIT,
        DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT,
        DISPLAY_FLOW_CLEANUP_PANEL_DELETE,
        DISPLAY_FLOW_CLEANUP_PANEL_IO_DELETE,
        DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE,
        DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE,
        DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT,
    };
    expect_trace(&fake, expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_first_error_is_preserved_before_handoff(void)
{
    const display_port_flow_operation_t failures[] = {
        DISPLAY_FLOW_FREE_FRAME,
        DISPLAY_FLOW_LVGL_INIT,
        DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF,
    };
    for (size_t index = 0U; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = failures[index];
        fake.fail_code = 11000 + (int32_t)index;
        assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
    }
}

static void test_post_handoff_failure_unlocks_only(void)
{
    const display_port_flow_operation_t failures[] = {
        DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE,
        DISPLAY_FLOW_LVGL_LOCK,
        DISPLAY_FLOW_LVGL_ROTATE_90,
        DISPLAY_FLOW_ACTIVE_SCREEN_BLACK,
        DISPLAY_FLOW_LVGL_UNLOCK,
    };
    for (size_t index = 0U; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        fake_executor_t fake;
        reset_fake(&fake);
        fake.fail_enabled = 1;
        fake.fail_operation = failures[index];
        fake.fail_code = 9500 + (int32_t)index;
        assert(display_port_flow_run(false, fake_execute, &fake) == fake.fail_code);
        for (size_t trace_index = 0U; trace_index < fake.trace_count; ++trace_index) {
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_PANEL_DELETE);
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_PANEL_IO_DELETE);
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE);
            assert(fake.trace[trace_index] != DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE);
        }
    }
}

int main(void)
{
    test_success_traces();
    test_publish_marker_is_non_failing();
    test_null_executor_is_rejected();
    test_every_fallible_operation_preserves_error();
    test_cleanup_errors_are_ignored_without_masking_first_error();
    test_direct_present_failure_quarantines();
    test_add_display_failure_quarantines();
    test_pre_handoff_failure_preserves_first_error_and_reverses();
    test_first_error_is_preserved_before_handoff();
    test_post_handoff_failure_unlocks_only();
    puts("display_port_flow tests passed");
    return 0;
}
