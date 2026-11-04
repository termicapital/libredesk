#include "ui_screens.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIXEL_COUNT ((size_t)UI_SCREEN_WIDTH * (size_t)UI_SCREEN_HEIGHT)
#define PATH_CAPACITY 1024U

typedef union {
    uint32_t alignment;
    uint16_t pixels[PIXEL_COUNT];
} aligned_pixel_buffer_t;

typedef struct {
    const char *name;
    const char *title;
    ui_screen_kind_t kind;
    glucose_display_state_t state;
    const char *value;
    const char *unit;
    const char *age;
    const char *ssid;
    glucose_trend_t trend;
    glucose_range_t range;
    uint8_t progress_percent;
    uint8_t perceived_brightness_percent;
    int8_t jitter_x;
    int8_t jitter_y;
} scenario_t;

static aligned_pixel_buffer_t draw_buffer;
static uint16_t framebuffer[PIXEL_COUNT];
static bool flush_failed;

static const scenario_t scenarios[] = {
    {"boot", "Boot / starting", UI_SCREEN_BOOT, GLUCOSE_STATE_BOOT,
     "", "", "", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"provisioning", "Provisioning portal", UI_SCREEN_PROVISIONING, GLUCOSE_STATE_BOOT,
     "", "", "", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"wifi-connecting", "Wi-Fi connecting / long SSID", UI_SCREEN_WIFI_CONNECTING,
     GLUCOSE_STATE_WIFI_CONNECTING, "", "", "", "Office-WiFi-Long-Name-For-Clipping",
     GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"time-syncing", "Secure clock synchronization", UI_SCREEN_TIME_SYNCING,
     GLUCOSE_STATE_TIME_SYNCING, "", "", "", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"fetching", "First bridge fetch", UI_SCREEN_FETCHING, GLUCOSE_STATE_FETCHING,
     "", "", "", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"fresh-urgent-low", "Fresh / urgent-low hue", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "65", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING, GLUCOSE_RANGE_URGENT_LOW,
     0U, 100U, 0, 0},
    {"fresh-low", "Fresh / low hue", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "72", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING, GLUCOSE_RANGE_LOW,
     0U, 100U, 0, 0},
    {"fresh-in-range", "Fresh / stable trend", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "112", "mg/dL", "1m", "", GLUCOSE_TREND_STABLE, GLUCOSE_RANGE_IN_RANGE,
     0U, 100U, 0, 0},
    {"fresh-high", "Fresh / high hue", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "215", "mg/dL", "1m", "", GLUCOSE_TREND_RISING, GLUCOSE_RANGE_HIGH,
     0U, 100U, 0, 0},
    {"fresh-rising", "Fresh / rising trend", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "112", "mg/dL", "1m", "", GLUCOSE_TREND_RISING, GLUCOSE_RANGE_IN_RANGE,
     0U, 100U, 0, 0},
    {"fresh-rising-slightly", "Fresh / slightly rising trend", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_RISING_SLIGHTLY,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"fresh-falling", "Fresh / falling trend", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "112", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING, GLUCOSE_RANGE_IN_RANGE,
     0U, 100U, 0, 0},
    {"fresh-falling-slightly", "Fresh / slightly falling trend", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING_SLIGHTLY,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"fresh-rising-fast", "Fresh / fast-rising trend", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_RISING_FAST,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"fresh-falling-fast", "Fresh / fast-falling trend", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING_FAST,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"fresh-unknown-range", "Fresh / unknown trend and range", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"fresh-mmol", "Fresh / mmol/L", UI_SCREEN_FRESH, GLUCOSE_STATE_FRESH,
     "6.2", "mmol/L", "1m", "", GLUCOSE_TREND_STABLE, GLUCOSE_RANGE_IN_RANGE,
     0U, 100U, 0, 0},
    {"aging-low", "Aging / low and dimmed", UI_SCREEN_AGING, GLUCOSE_STATE_AGING,
     "75", "mg/dL", "12m", "", GLUCOSE_TREND_FALLING, GLUCOSE_RANGE_LOW,
     0U, 100U, 0, 0},
    {"aging-in-range", "Aging / in range and dimmed", UI_SCREEN_AGING,
     GLUCOSE_STATE_AGING, "108", "mg/dL", "12m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"stale", "Stale / number hidden", UI_SCREEN_STALE, GLUCOSE_STATE_STALE,
     "", "", "22m", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN,
     0U, 100U, 0, 0},
    {"no-data", "No data / number hidden", UI_SCREEN_NO_DATA, GLUCOSE_STATE_NO_DATA,
     "", "", "--", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN,
     0U, 100U, 0, 0},
    {"auth-error", "Authentication required", UI_SCREEN_AUTH_ERROR,
     GLUCOSE_STATE_AUTH_ERROR, "", "", "--", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"terms-required", "Terms acceptance required", UI_SCREEN_TERMS_REQUIRED,
     GLUCOSE_STATE_AUTH_ERROR, "", "", "--", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"server-error", "Bridge or payload error", UI_SCREEN_SERVER_ERROR,
     GLUCOSE_STATE_SRV_ERROR, "", "", "--", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_UNKNOWN, 0U, 100U, 0, 0},
    {"no-wifi", "Wi-Fi disconnected", UI_SCREEN_NO_WIFI, GLUCOSE_STATE_NO_WIFI,
     "", "", "--", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN,
     0U, 100U, 0, 0},
    {"ota-update", "OTA update in progress", UI_SCREEN_OTA_UPDATING, GLUCOSE_STATE_BOOT,
     "", "", "", "", GLUCOSE_TREND_UNKNOWN, GLUCOSE_RANGE_UNKNOWN,
     42U, 100U, 0, 0},
    {"fresh-night", "Fresh / scheduled night dimming", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 35U, 0, 0},
    {"fresh-jitter", "Fresh / +2,-2 pixel jitter", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "112", "mg/dL", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 2, -2},
    {"layout-number-with-arrow", "Layout fixture / centered number with arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "288", "mg/dL", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-number-no-arrow", "Layout fixture / centered number without arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "288", "mg/dL", "1m", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-two-digit-with-arrow", "Layout fixture / two digits with arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "88", "mg/dL", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-two-digit-no-arrow", "Layout fixture / two digits without arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "88", "mg/dL", "1m", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-asymmetric-with-arrow", "Layout fixture / asymmetric digits with arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "187", "mg/dL", "1m", "", GLUCOSE_TREND_FALLING,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-asymmetric-no-arrow", "Layout fixture / asymmetric digits without arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "187", "mg/dL", "1m", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-mmol-wide-with-arrow", "Layout fixture / widest decimal with arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "27.8", "mmol/L", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-mmol-wide-no-arrow", "Layout fixture / widest decimal without arrow", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "27.8", "mmol/L", "1m", "", GLUCOSE_TREND_UNKNOWN,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 0, 0},
    {"layout-mmol-wide-jitter", "Layout fixture / widest decimal +2,-2 jitter", UI_SCREEN_FRESH,
     GLUCOSE_STATE_FRESH, "27.8", "mmol/L", "1m", "", GLUCOSE_TREND_STABLE,
     GLUCOSE_RANGE_IN_RANGE, 0U, 100U, 2, -2}
};

static bool copy_exact(char *destination, size_t capacity, const char *source)
{
    size_t length;
    if (destination == NULL || capacity == 0U || source == NULL) {
        return false;
    }
    length = strlen(source);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static void copy_truncated(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool build_model(const scenario_t *scenario, ui_screen_model_t *model)
{
    bool value_screen;
    if (scenario == NULL || model == NULL) {
        return false;
    }
    memset(model, 0, sizeof(*model));
    model->kind = scenario->kind;
    model->progress_percent = scenario->progress_percent;
    model->jitter_x = scenario->jitter_x;
    model->jitter_y = scenario->jitter_y;
    copy_truncated(model->ssid, sizeof(model->ssid), scenario->ssid);
    if (scenario->kind == UI_SCREEN_PROVISIONING) {
        if (!copy_exact(model->ssid, sizeof(model->ssid), "LibreDesk-123456") ||
            !copy_exact(model->setup_password, sizeof(model->setup_password),
                        "AAAAAAAAAAAA")) {
            return false;
        }
    }

    model->glucose.state = scenario->state;
    model->glucose.trend = scenario->trend;
    model->glucose.range = scenario->range;
    value_screen = scenario->kind == UI_SCREEN_FRESH || scenario->kind == UI_SCREEN_AGING;
    model->glucose.show_number = value_screen;
    model->glucose.use_range_hue = value_screen;

    if (!copy_exact(model->glucose.value_text, sizeof(model->glucose.value_text), scenario->value) ||
        !copy_exact(model->glucose.unit_text, sizeof(model->glucose.unit_text), scenario->unit) ||
        !copy_exact(model->glucose.age_text, sizeof(model->glucose.age_text), scenario->age) ||
        !copy_exact(model->glucose.status_text, sizeof(model->glucose.status_text), "") ||
        !copy_exact(model->clock_text, sizeof(model->clock_text), value_screen ? "20:56" : "")) {
        return false;
    }
    return ui_screen_model_is_valid(model);
}

static void flush_callback(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    int32_t width;
    int32_t x;
    int32_t y;
    const uint16_t *source = (const uint16_t *)(const void *)pixel_map;
    if (area == NULL || pixel_map == NULL || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= UI_SCREEN_WIDTH || area->y2 >= UI_SCREEN_HEIGHT ||
        area->x2 < area->x1 || area->y2 < area->y1) {
        flush_failed = true;
        lv_display_flush_ready(display);
        return;
    }

    width = area->x2 - area->x1 + 1;
    for (y = area->y1; y <= area->y2; y++) {
        for (x = area->x1; x <= area->x2; x++) {
            size_t destination_index = (size_t)y * UI_SCREEN_WIDTH + (size_t)x;
            size_t source_index = (size_t)(y - area->y1) * (size_t)width +
                                  (size_t)(x - area->x1);
            framebuffer[destination_index] = source[source_index];
        }
    }
    lv_display_flush_ready(display);
}

static bool make_path(char *destination, size_t capacity,
                      const char *directory, const char *name, const char *extension)
{
    int length = snprintf(destination, capacity, "%s/%s%s", directory, name, extension);
    return length >= 0 && (size_t)length < capacity;
}

static uint8_t expand_5_to_8(uint16_t value)
{
    return (uint8_t)((value * 255U + 15U) / 31U);
}

static uint8_t expand_6_to_8(uint16_t value)
{
    return (uint8_t)((value * 255U + 31U) / 63U);
}

static uint8_t apply_brightness(uint8_t value, uint8_t percent)
{
    return (uint8_t)(((unsigned)value * (unsigned)percent + 50U) / 100U);
}

static bool write_ppm(const char *path, uint8_t brightness_percent)
{
    FILE *output = fopen(path, "wb");
    size_t index;
    if (output == NULL) {
        return false;
    }
    if (fprintf(output, "P6\n%d %d\n255\n", UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT) < 0) {
        fclose(output);
        return false;
    }
    for (index = 0U; index < PIXEL_COUNT; index++) {
        uint16_t pixel = framebuffer[index];
        uint8_t rgb[3];
        rgb[0] = apply_brightness(expand_5_to_8((uint16_t)((pixel >> 11U) & 0x1FU)),
                                  brightness_percent);
        rgb[1] = apply_brightness(expand_6_to_8((uint16_t)((pixel >> 5U) & 0x3FU)),
                                  brightness_percent);
        rgb[2] = apply_brightness(expand_5_to_8((uint16_t)(pixel & 0x1FU)),
                                  brightness_percent);
        if (fwrite(rgb, sizeof(rgb), 1U, output) != 1U) {
            fclose(output);
            return false;
        }
    }
    return fclose(output) == 0;
}

static bool write_index_row(FILE *output, const scenario_t *scenario)
{
    return fprintf(output, "%s\t%s\t%s\t%s\t%u\t%d\t%d\n",
                   scenario->name,
                   scenario->title,
                   ui_screen_kind_name(scenario->kind),
                   (scenario->kind == UI_SCREEN_FRESH || scenario->kind == UI_SCREEN_AGING)
                       ? "true" : "false",
                   (unsigned)scenario->perceived_brightness_percent,
                   (int)scenario->jitter_x,
                   (int)scenario->jitter_y) > 0;
}

static bool host_rgb565_byte_order_is_supported(void)
{
    const uint16_t marker = 1U;
    return *(const uint8_t *)(const void *)&marker == 1U;
}

int main(int argc, char **argv)
{
    lv_display_t *display;
    lv_obj_t *old_screen;
    FILE *index_file;
    char path[PATH_CAPACITY];
    size_t scenario_index;

    if (argc != 2) {
        fprintf(stderr, "usage: uisim OUTPUT_DIRECTORY\n");
        return EXIT_FAILURE;
    }
    if (!host_rgb565_byte_order_is_supported()) {
        fprintf(stderr, "uisim requires a little-endian host for RGB565 capture\n");
        return EXIT_FAILURE;
    }

    lv_init();
    display = lv_display_create(UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    if (display == NULL) {
        fprintf(stderr, "failed to create LVGL display\n");
        return EXIT_FAILURE;
    }
    lv_display_set_default(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer.pixels, NULL,
                           (uint32_t)sizeof(draw_buffer.pixels),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_callback);

    if (!make_path(path, sizeof(path), argv[1], "manifest", ".tsv")) {
        fprintf(stderr, "output path is too long\n");
        return EXIT_FAILURE;
    }
    index_file = fopen(path, "wb");
    if (index_file == NULL) {
        fprintf(stderr, "cannot open manifest output: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    old_screen = lv_display_get_screen_active(display);
    for (scenario_index = 0U;
         scenario_index < sizeof(scenarios) / sizeof(scenarios[0]);
         scenario_index++) {
        const scenario_t *scenario = &scenarios[scenario_index];
        ui_screen_model_t model;
        lv_obj_t *screen;

        if (!build_model(scenario, &model)) {
            fprintf(stderr, "invalid UI scenario: %s\n", scenario->name);
            fclose(index_file);
            return EXIT_FAILURE;
        }
        screen = ui_screen_create(&model);
        if (screen == NULL) {
            fprintf(stderr, "LVGL screen creation failed: %s\n", scenario->name);
            fclose(index_file);
            return EXIT_FAILURE;
        }

        memset(framebuffer, 0, sizeof(framebuffer));
        flush_failed = false;
        lv_screen_load(screen);
        if (old_screen != NULL && old_screen != screen) {
            lv_obj_delete(old_screen);
        }
        old_screen = screen;
        lv_tick_inc(40U);
        lv_refr_now(display);
        if (flush_failed) {
            fprintf(stderr, "invalid LVGL flush area: %s\n", scenario->name);
            fclose(index_file);
            return EXIT_FAILURE;
        }

        if (!make_path(path, sizeof(path), argv[1], scenario->name, ".ppm") ||
            !write_ppm(path, scenario->perceived_brightness_percent) ||
            !write_index_row(index_file, scenario)) {
            fprintf(stderr, "failed to write UI artifact: %s\n", scenario->name);
            fclose(index_file);
            return EXIT_FAILURE;
        }
        printf("rendered %s\n", scenario->name);
    }

    if (fclose(index_file) != 0) {
        fprintf(stderr, "failed to close scenario manifest\n");
        return EXIT_FAILURE;
    }
    lv_display_delete(display);
    lv_deinit();
    return EXIT_SUCCESS;
}
