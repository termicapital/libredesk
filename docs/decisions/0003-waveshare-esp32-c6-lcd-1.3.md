# ADR 0003: Waveshare ESP32-C6-LCD-1.3 board definition

- Status: accepted and physically verified for the display spike
- Date: 2026-08-23
- Confidence: high for electrical pin assignments and the accepted color/orientation calibration

## Exact product

Use Waveshare **ESP32-C6-LCD-1.3**, not the visually similar 1.47-inch board:

- SKU 33643: standard version
- SKU 33644: pre-soldered header
- MCU: ESP32-C6FH4, 4 MB flash, no advertised PSRAM
- panel: 1.3-inch IPS, 240 × 240, ST7789V2, 4-wire SPI

Official product documentation: <https://docs.waveshare.com/ESP32-C6-LCD-1.3>

Official resources: <https://docs.waveshare.com/ESP32-C6-LCD-1.3/Resources-And-Documents>

Official schematic: <https://files.waveshare.com/wiki/ESP32-C6-LCD-1.3/ESP32C6-1.3.pdf>

Official demo archive: <https://files.waveshare.com/wiki/ESP32-C6-LCD-1.3/ESP32-C6-LCD-1.3_Demo.zip>

The four URLs above returned HTTP 200 on 2026-08-22. Local research copies had SHA-256 `1eb6df3d9a01d044419c8f4dec644d4b1243384bd23a9094b8bcd7f87caa6c32` (schematic) and `fdc8f1eb3e89bd643a7b2921ce9364dc5d516d3c351c3fdef3b82c2de7f342f4` (demo archive).

The vendor recommends ESP-IDF 5.5.0 or newer; the downloaded demo was generated with 5.5.1. Libre Desk remains pinned to the PLAN.md 5.5.x baseline.

## Board pin map

The official demo's `ESP-IDF/ESP32-C6-LCD-1.3-Test/main/LCD_Driver/ST7789.h` and Arduino `Display_ST7789.h` agree:

| Function | GPIO | Notes |
|---|---:|---|
| LCD SPI SCLK | 7 | SPI2 host in ESP-IDF demo |
| LCD SPI MOSI | 6 | shared with microSD MOSI |
| LCD CS | 14 | active low |
| LCD D/C | 15 | command/data select |
| LCD reset | 21 | active low; schematic adds a 10 kΩ pull-up and 100 nF capacitor |
| LCD backlight | 22 | active-high PWM through a low-side SI2302 stage |
| SPI MISO | 5 | unused by LCD; shared bus uses it for microSD |
| microSD CS | 4 | separate active-low chip select with a 10 kΩ external pull-up |
| RGB LED data | 8 | unrelated to display |

Display geometry is 240 × 240 with zero X/Y gap in both vendor examples. The first-light firmware also drives microSD CS high before configuring the shared SCLK/MOSI pins; the external pull-up is retained as the reset-time default, not treated as the only deselection mechanism.

## Initial conservative first-light settings

The display spike began with the vendor ESP-IDF settings:

- `SPI2_HOST`
- SPI mode 0, MSB first
- 12 MHz pixel clock
- 8-bit commands and parameters
- RGB565, with the maintained ESP-IDF ST7789 driver programming `RAMCTRL` for little-endian pixel input
- BGR element order
- X mirror enabled, Y mirror disabled
- color inversion enabled (`INVON`, command `0x21`)
- no coordinate gap
- active-high 5 kHz PWM backlight, initially capped at 25% for first light

The Arduino example uses 80 MHz, but that is not the bring-up baseline. Frequency may only be raised after the conservative physical test is stable.

The vendor LVGL demo enables 16-bit byte swapping for its custom panel path. This spike intentionally does not copy that setting: ESP-IDF v5.5.5's maintained ST7789 driver sends the full two-byte `RAMCTRL` command and selects little-endian pixel input, so both the native RGB565 framebuffer and `esp_lvgl_port` use native little-endian words with `swap_bytes = false`. The physical red/blue sequence was used as the final check.

## Physically accepted calibration

The exact board was flashed and observed on 2026-08-23. The finished-product pose has the USB-C cable exiting from the **right**. The accepted configuration is:

- RGB element order;
- little-endian RGB565 with `swap_bytes = false`;
- no hardware X/Y swap and no hardware X/Y mirroring;
- color inversion enabled and zero coordinate gaps;
- `esp_lvgl_port` software rotation enabled with `LV_DISPLAY_ROTATION_90`;
- two 240 × 40 DMA partial draw buffers plus one partial-buffer-sized software-rotation allocation;
- SPI mode 0 at the conservative 12 MHz clock and 25% first-light brightness.

The physical calibration was evidence-driven:

1. The vendor-style BGR + X-mirror baseline produced a stable, unclipped card, but red and blue were exchanged and the asymmetric digit was transposed.
2. An RGB candidate using hardware `swap_xy` corrected the element order but corrupted partial LVGL flush geometry: the corner blocks clustered and the digit split. That configuration was rejected.
3. Firmware `0.1.2-display-calibration` kept ST7789 MV disabled and rotated each partial pixel buffer together with its flush area in software. With USB-C exiting right, the human observer confirmed exact red/green/blue/white corner placement and an intact, upright, non-mirrored yellow digit.
4. After removing power for five seconds, the full sequence and accepted final card returned unchanged on cold boot.

This accepts the color/orientation and first cold-boot gate. A brightness sweep, repeated-boot campaign, and extended stability soak remain later product-validation tasks; they are not implied by this first-light acceptance.

## Initialization quirks and inconsistencies

The official ESP-IDF archive carries a custom `Vernon_ST7789T` driver with panel-specific power, porch, VCOM, frame-rate, gamma, inversion, and display-on commands. Its final MADCTL state becomes MX + BGR after `esp_lcd_panel_mirror(panel, true, false)`. The Arduino demo also uses inversion, zero gaps, and mode 0, but it has a different power/gamma table and an 80 MHz clock.

One apparent vendor defect is present in the custom ESP-IDF driver: command `0xD0` provides two bytes but passes a length of one; the Arduino demo sends both bytes. Libre Desk will use Espressif's in-tree ST7789 driver first, then apply only source-grounded panel quirks if the physical color-bar test demonstrates they are necessary.

## Physical acceptance gate

Source inspection and a successful cross-build do **not** prove first light. Physical acceptance requires a human to confirm:

1. full-screen red, green, blue, white, and black render without offset or wrap;
2. red and blue are not swapped;
3. orientation matches the intended desk enclosure;
4. PWM brightness changes smoothly without flicker;
5. the panel remains stable through repeated cold boots.

Items 1–3 and one five-second cold boot at fixed 25% brightness were confirmed on 2026-08-23. PWM sweep behavior, repeated cold boots, and soak stability remain open for M7/M8. Any later color-order, rotation, mirror, inversion, or init-table adjustment must update this ADR and repeat the physical card test.
