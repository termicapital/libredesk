#include "display_port_flow.h"

#include <stddef.h>

#define FLOW_ERR_INVALID_ARGUMENT (-1)

typedef struct flow_state {
    bool backlight_started;
    bool semaphore_owned;
    bool spi_owned;
    bool io_owned;
    bool panel_owned;
    bool panel_enabled;
    bool frame_owned;
    bool present_uncertain;
    bool lvgl_initialized;
    bool handoff_invoked;
    bool lock_held;
} flow_state_t;

static int32_t invoke(display_port_flow_execute_fn execute,
                      void *context,
                      display_port_flow_operation_t operation)
{
    return execute(context, operation);
}

static void quarantine(flow_state_t *state,
                       display_port_flow_execute_fn execute,
                       void *context)
{
    if (state->lock_held) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_LVGL_UNLOCK);
        state->lock_held = false;
    }
    (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT);
    (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT);
}

static void rollback(flow_state_t *state,
                     display_port_flow_execute_fn execute,
                     void *context)
{
    (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT);
    if (state->panel_enabled) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_PANEL_OFF_BEST_EFFORT);
        state->panel_enabled = false;
    }
    if (state->lock_held) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_LVGL_UNLOCK);
        state->lock_held = false;
    }
    if (state->lvgl_initialized && !state->handoff_invoked) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_LVGL_DEINIT);
        state->lvgl_initialized = false;
    }
    if (state->frame_owned && !state->present_uncertain) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_FREE_FRAME);
        state->frame_owned = false;
    }
    if (state->panel_owned) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_PANEL_DELETE);
        state->panel_owned = false;
    }
    if (state->io_owned) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_PANEL_IO_DELETE);
        state->io_owned = false;
    }
    if (state->spi_owned) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE);
        state->spi_owned = false;
    }
    if (state->semaphore_owned) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE);
        state->semaphore_owned = false;
    }
    if (state->backlight_started) {
        (void)invoke(execute, context, DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT);
        state->backlight_started = false;
    }
}

static int32_t normal_step(flow_state_t *state,
                           display_port_flow_execute_fn execute,
                           void *context,
                           display_port_flow_operation_t operation)
{
    const int32_t result = invoke(execute, context, operation);
    if (result != 0) {
        return result;
    }

    switch (operation) {
    case DISPLAY_FLOW_BACKLIGHT_INIT_OFF:
        state->backlight_started = true;
        break;
    case DISPLAY_FLOW_CREATE_SEMAPHORE:
        state->semaphore_owned = true;
        break;
    case DISPLAY_FLOW_SPI_BUS_INIT:
        state->spi_owned = true;
        break;
    case DISPLAY_FLOW_PANEL_IO_CREATE:
        state->io_owned = true;
        break;
    case DISPLAY_FLOW_ST7789_CREATE:
        state->panel_owned = true;
        break;
    case DISPLAY_FLOW_ALLOCATE_FRAME:
        state->frame_owned = true;
        break;
    case DISPLAY_FLOW_PANEL_DISPLAY_ON:
        state->panel_enabled = true;
        break;
    case DISPLAY_FLOW_FREE_FRAME:
        state->frame_owned = false;
        break;
    case DISPLAY_FLOW_LVGL_INIT:
        state->lvgl_initialized = true;
        break;
    case DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE:
        state->semaphore_owned = false;
        break;
    case DISPLAY_FLOW_LVGL_LOCK:
        state->lock_held = true;
        break;
    case DISPLAY_FLOW_LVGL_UNLOCK:
        state->lock_held = false;
        break;
    default:
        break;
    }
    return 0;
}

static int32_t run_normal(flow_state_t *state,
                          display_port_flow_execute_fn execute,
                          void *context,
                          display_port_flow_operation_t operation)
{
    const int32_t result = normal_step(state, execute, context, operation);
    if (result != 0) {
        rollback(state, execute, context);
    }
    return result;
}

int32_t display_port_flow_run(bool run_acceptance_sequence,
                              display_port_flow_execute_fn execute,
                              void *context)
{
    if (execute == NULL) {
        return FLOW_ERR_INVALID_ARGUMENT;
    }

    flow_state_t state = {0};
    const display_port_flow_operation_t setup[] = {
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

    for (size_t index = 0U; index < sizeof(setup) / sizeof(setup[0]); ++index) {
        const display_port_flow_operation_t operation = setup[index];
        if (operation == DISPLAY_FLOW_PRESENT_BLACK) {
            const int32_t result = invoke(execute, context, operation);
            if (result != 0) {
                state.present_uncertain = true;
                quarantine(&state, execute, context);
                return result;
            }
            continue;
        }
        const int32_t result = run_normal(&state, execute, context, operation);
        if (result != 0) {
            return result;
        }
    }

    if (run_acceptance_sequence) {
        const display_port_flow_operation_t solids[] = {
            DISPLAY_FLOW_PRESENT_RED,
            DISPLAY_FLOW_PRESENT_GREEN,
            DISPLAY_FLOW_PRESENT_BLUE,
            DISPLAY_FLOW_PRESENT_WHITE,
        };
        for (size_t index = 0U; index < sizeof(solids) / sizeof(solids[0]); ++index) {
            const int32_t result = invoke(execute, context, solids[index]);
            if (result != 0) {
                state.present_uncertain = true;
                quarantine(&state, execute, context);
                return result;
            }
        }
    }

    const int32_t free_result =
        run_normal(&state, execute, context, DISPLAY_FLOW_FREE_FRAME);
    if (free_result != 0) {
        return free_result;
    }
    const int32_t lvgl_init_result =
        run_normal(&state, execute, context, DISPLAY_FLOW_LVGL_INIT);
    if (lvgl_init_result != 0) {
        return lvgl_init_result;
    }
    const int32_t ready_result = run_normal(
        &state, execute, context, DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF);
    if (ready_result != 0) {
        return ready_result;
    }

    state.handoff_invoked = true;
    const int32_t add_result = invoke(execute, context, DISPLAY_FLOW_LVGL_ADD_DISPLAY);
    if (add_result != 0) {
        quarantine(&state, execute, context);
        return add_result;
    }

    const display_port_flow_operation_t post_add[] = {
        DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE,
        DISPLAY_FLOW_LVGL_LOCK,
        DISPLAY_FLOW_LVGL_ROTATE_90,
        DISPLAY_FLOW_ACTIVE_SCREEN_BLACK,
    };
    for (size_t index = 0U; index < sizeof(post_add) / sizeof(post_add[0]); ++index) {
        const int32_t result = invoke(execute, context, post_add[index]);
        if (result != 0) {
            quarantine(&state, execute, context);
            return result;
        }
        switch (post_add[index]) {
        case DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE:
            state.semaphore_owned = false;
            break;
        case DISPLAY_FLOW_LVGL_LOCK:
            state.lock_held = true;
            break;
        default:
            break;
        }
    }

    if (run_acceptance_sequence) {
        const int32_t result = invoke(execute, context, DISPLAY_FLOW_ACCEPTANCE_CARD);
        if (result != 0) {
            quarantine(&state, execute, context);
            return result;
        }
    }

    const int32_t unlock_result = invoke(execute, context, DISPLAY_FLOW_LVGL_UNLOCK);
    if (unlock_result != 0) {
        quarantine(&state, execute, context);
        return unlock_result;
    }
    state.lock_held = false;

    (void)invoke(execute, context, DISPLAY_FLOW_PUBLISH_DISPLAY);
    return 0;
}
