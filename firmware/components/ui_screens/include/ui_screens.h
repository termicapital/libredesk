#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glucose_core.h"
#include "ui_contract.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SCREEN_WIDTH 240
#define UI_SCREEN_HEIGHT 240
#define UI_SCREEN_JITTER_LIMIT_PX 2
#define UI_SCREEN_SSID_MAX_BYTES 32U
#define UI_SCREEN_SETUP_PASSWORD_BYTES 12U
#define UI_SCREEN_SETUP_PASSWORD_CAPACITY (UI_SCREEN_SETUP_PASSWORD_BYTES + 1U)
#define UI_SCREEN_CLOCK_TEXT_CAPACITY 6U

typedef struct {
    ui_screen_kind_t kind;
    glucose_display_model_t glucose;
    char ssid[UI_SCREEN_SSID_MAX_BYTES + 1U];
    char setup_password[UI_SCREEN_SETUP_PASSWORD_CAPACITY];
    char clock_text[UI_SCREEN_CLOCK_TEXT_CAPACITY];
    uint8_t progress_percent;
    int8_t jitter_x;
    int8_t jitter_y;
} ui_screen_model_t;

const char *ui_screen_kind_name(ui_screen_kind_t kind);
bool ui_screen_model_is_valid(const ui_screen_model_t *model);
/** Return inclusive visible-pixel offsets without reading beyond value_capacity. */
bool ui_screen_value_visible_bounds(const char *value, size_t value_capacity,
                                    int32_t *left, int32_t *right);

/**
 * Create a complete 240x240 LVGL screen. The caller owns the returned object
 * and must hold the LVGL lock; like LVGL object creation itself, this function
 * is not reentrant. Returns NULL when the model violates an input invariant or
 * when any tracked child-object allocation fails. On NULL, the caller must
 * replace any previously active glucose screen with a known neutral fallback
 * or blank it; retaining an older numeric screen would violate fail-closed use.
 */
lv_obj_t *ui_screen_create(const ui_screen_model_t *model);
lv_obj_t *ui_screen_create_on_display(const ui_screen_model_t *model,
                                       lv_display_t *display);

#ifdef __cplusplus
}
#endif

#endif
