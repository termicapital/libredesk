#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

typedef struct display_port {
    lv_display_t *display;
} display_port_t;

esp_err_t display_port_start(display_port_t *port,
                             bool run_acceptance_sequence);
esp_err_t display_port_set_brightness(uint8_t percent);
