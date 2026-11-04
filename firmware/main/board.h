#pragma once

/*
 * Waveshare ESP32-C6-LCD-1.3, SKU 33643/33644.
 * Sources verified 2026-08-22:
 *   https://docs.waveshare.com/ESP32-C6-LCD-1.3
 *   docs/decisions/0003-waveshare-esp32-c6-lcd-1.3.md
 *   official ESP-IDF demo: main/LCD_Driver/ST7789.h
 *
 * Do not substitute the visually similar ESP32-C6-LCD-1.47 pin map.
 */

#include <stdbool.h>
#include "driver/spi_master.h"
#include "esp_lcd_types.h"

#define BOARD_LCD_HOST              SPI2_HOST
#define BOARD_LCD_WIDTH             240
#define BOARD_LCD_HEIGHT            240
#define BOARD_LCD_PIXEL_CLOCK_HZ     (12 * 1000 * 1000)
#define BOARD_LCD_SPI_MODE           0
#define BOARD_LCD_CMD_BITS           8
#define BOARD_LCD_PARAM_BITS         8

#define BOARD_LCD_PIN_SCLK           7
#define BOARD_LCD_PIN_MOSI           6
#define BOARD_LCD_PIN_CS             14
#define BOARD_LCD_PIN_DC             15
#define BOARD_LCD_PIN_RESET          21
#define BOARD_LCD_PIN_BACKLIGHT      22
#define BOARD_LCD_BACKLIGHT_ACTIVE_HIGH true

#define BOARD_LCD_GAP_X              0
#define BOARD_LCD_GAP_Y              0
#define BOARD_LCD_MIRROR_X           false
#define BOARD_LCD_MIRROR_Y           false
#define BOARD_LCD_SWAP_XY            false
#define BOARD_LCD_RGB_ELEMENT_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define BOARD_LCD_INVERT_COLOR       true
#define BOARD_LCD_INITIAL_BRIGHTNESS 25

#define BOARD_SD_PIN_CS              4
#define BOARD_SD_CS_PULLUP_OHMS      10000
#define BOARD_SHARED_SPI_PIN_MISO    5
#define BOARD_RGB_LED_PIN            8
#define BOARD_BOOT_BUTTON_PIN        9
/* BOOT is wired to ground on GPIO9; official schematic and ADR 0003: */
/* https://files.waveshare.com/wiki/ESP32-C6-LCD-1.3/ESP32C6-1.3.pdf */
#define BOARD_BOOT_BUTTON_ACTIVE_LOW true
