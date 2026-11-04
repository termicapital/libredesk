#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "runtime_core.h"

#define UI_SCREEN_SSID_MAX_BYTES 32U
#define UI_SCREEN_SETUP_PASSWORD_BYTES 12U
#define UI_SCREEN_SETUP_PASSWORD_CAPACITY (UI_SCREEN_SETUP_PASSWORD_BYTES + 1U)

typedef struct {
    ui_screen_kind_t kind;
    glucose_display_model_t glucose;
    char ssid[UI_SCREEN_SSID_MAX_BYTES + 1U];
    char setup_password[UI_SCREEN_SETUP_PASSWORD_CAPACITY];
    char clock_text[RUNTIME_CLOCK_TEXT_CAPACITY];
    uint8_t progress_percent;
    int8_t jitter_x;
    int8_t jitter_y;
} ui_screen_model_t;

bool ui_screen_model_is_valid(const ui_screen_model_t *model);
lv_obj_t *ui_screen_create_on_display(const ui_screen_model_t *model,
                                       lv_display_t *display);

#endif
