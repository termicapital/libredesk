#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

static unsigned assertions;
static unsigned failures;

#define TEST_PIXEL_COUNT ((size_t)UI_SCREEN_WIDTH * (size_t)UI_SCREEN_HEIGHT)

typedef union {
    uint32_t alignment;
    uint16_t pixels[TEST_PIXEL_COUNT];
} test_aligned_pixel_buffer_t;

static test_aligned_pixel_buffer_t test_draw_buffer;
static uint16_t test_framebuffer[TEST_PIXEL_COUNT];
static uint16_t baseline_framebuffer[TEST_PIXEL_COUNT];
static bool test_flush_failed;

#define EXPECT_TRUE(expression) do { \
    assertions++; \
    if (!(expression)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
    } \
} while (0)

#define EXPECT_FALSE(expression) EXPECT_TRUE(!(expression))

static void test_flush_callback(lv_display_t *display, const lv_area_t *area,
                                uint8_t *pixel_map)
{
    int32_t width;
    int32_t x;
    int32_t y;
    const uint16_t *source = (const uint16_t *)(const void *)pixel_map;
    if (area == NULL || pixel_map == NULL || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= UI_SCREEN_WIDTH || area->y2 >= UI_SCREEN_HEIGHT ||
        area->x2 < area->x1 || area->y2 < area->y1) {
        test_flush_failed = true;
        lv_display_flush_ready(display);
        return;
    }
    width = area->x2 - area->x1 + 1;
    for (y = area->y1; y <= area->y2; y++) {
        for (x = area->x1; x <= area->x2; x++) {
            size_t destination = (size_t)y * UI_SCREEN_WIDTH + (size_t)x;
            size_t source_index = (size_t)(y - area->y1) * (size_t)width +
                                  (size_t)(x - area->x1);
            test_framebuffer[destination] = source[source_index];
        }
    }
    lv_display_flush_ready(display);
}

static ui_screen_model_t valid_model(ui_screen_kind_t kind)
{
    ui_screen_model_t model;
    memset(&model, 0, sizeof(model));
    model.kind = kind;
    model.glucose.trend = GLUCOSE_TREND_STABLE;
    model.glucose.range = GLUCOSE_RANGE_IN_RANGE;

    switch (kind) {
        case UI_SCREEN_BOOT:
        case UI_SCREEN_OTA_UPDATING:
            model.glucose.state = GLUCOSE_STATE_BOOT;
            break;
        case UI_SCREEN_PROVISIONING:
            model.glucose.state = GLUCOSE_STATE_BOOT;
            (void)strcpy(model.ssid, "LibreDesk-123456");
            (void)strcpy(model.setup_password, "AAAAAAAAAAAA");
            break;
        case UI_SCREEN_WIFI_CONNECTING:
            model.glucose.state = GLUCOSE_STATE_WIFI_CONNECTING;
            (void)strcpy(model.ssid, "Office Wi-Fi");
            break;
        case UI_SCREEN_TIME_SYNCING:
            model.glucose.state = GLUCOSE_STATE_TIME_SYNCING;
            break;
        case UI_SCREEN_FETCHING:
            model.glucose.state = GLUCOSE_STATE_FETCHING;
            break;
        case UI_SCREEN_FRESH:
            model.glucose.state = GLUCOSE_STATE_FRESH;
            model.glucose.show_number = true;
            model.glucose.use_range_hue = true;
            (void)strcpy(model.glucose.value_text, "112");
            (void)strcpy(model.glucose.unit_text, "mg/dL");
            (void)strcpy(model.glucose.age_text, "1m");
            (void)strcpy(model.clock_text, "09:38");
            break;
        case UI_SCREEN_AGING:
            model.glucose.state = GLUCOSE_STATE_AGING;
            model.glucose.show_number = true;
            model.glucose.use_range_hue = true;
            (void)strcpy(model.glucose.value_text, "6.2");
            (void)strcpy(model.glucose.unit_text, "mmol/L");
            (void)strcpy(model.glucose.age_text, "12m");
            (void)strcpy(model.clock_text, "20:56");
            break;
        case UI_SCREEN_STALE:
            model.glucose.state = GLUCOSE_STATE_STALE;
            (void)strcpy(model.glucose.age_text, "22m");
            break;
        case UI_SCREEN_NO_DATA:
            model.glucose.state = GLUCOSE_STATE_NO_DATA;
            break;
        case UI_SCREEN_AUTH_ERROR:
        case UI_SCREEN_TERMS_REQUIRED:
            model.glucose.state = GLUCOSE_STATE_AUTH_ERROR;
            break;
        case UI_SCREEN_SERVER_ERROR:
            model.glucose.state = GLUCOSE_STATE_SRV_ERROR;
            break;
        case UI_SCREEN_NO_WIFI:
            model.glucose.state = GLUCOSE_STATE_NO_WIFI;
            break;
        default:
            model.glucose.state = GLUCOSE_STATE_BOOT;
            break;
    }
    return model;
}

static void expected_visible_bounds(const char *value, int32_t *left, int32_t *right)
{
    static const int32_t digit_left[10] = {0, 35, 0, 7, 0, 0, 0, 7, 0, 0};
    int32_t cursor = 0;
    size_t index;
    for (index = 0U; value[index] != '\0'; index++) {
        if (value[index] == '.') {
            if (index == 0U) {
                *left = cursor + 1;
            }
            *right = cursor + 7;
            cursor += 10;
        } else {
            unsigned digit = (unsigned)(value[index] - '0');
            if (index == 0U) {
                *left = cursor + digit_left[digit];
            }
            *right = cursor + 41;
            cursor += 42;
        }
        if (value[index + 1U] != '\0') {
            cursor += 5;
        }
    }
}

static void assert_visible_bounds(const char *value)
{
    int32_t actual_left = -1;
    int32_t actual_right = -1;
    int32_t expected_left = -1;
    int32_t expected_right = -1;
    int32_t origin;
    int32_t center_sum;
    expected_visible_bounds(value, &expected_left, &expected_right);
    EXPECT_TRUE(ui_screen_value_visible_bounds(
        value, GLUCOSE_VALUE_TEXT_CAPACITY, &actual_left, &actual_right));
    EXPECT_TRUE(actual_left == expected_left);
    EXPECT_TRUE(actual_right == expected_right);
    origin = (UI_SCREEN_WIDTH - (actual_right - actual_left + 1)) / 2 - actual_left;
    center_sum = (origin + actual_left) + (origin + actual_right);
    EXPECT_TRUE(center_sum == 238 || center_sum == 239);
}

static void test_every_accepted_value_has_independent_visible_bounds(void)
{
    char value_text[GLUCOSE_VALUE_TEXT_CAPACITY];
    int value;
    int tenths;
    int32_t left;
    int32_t right;
    ui_screen_model_t model;

    for (value = 20; value <= 500; value++) {
        (void)snprintf(value_text, sizeof(value_text), "%d", value);
        model = valid_model(UI_SCREEN_FRESH);
        (void)strcpy(model.glucose.value_text, value_text);
        EXPECT_TRUE(ui_screen_model_is_valid(&model));
        assert_visible_bounds(value_text);
    }
    for (tenths = 11; tenths <= 278; tenths++) {
        (void)snprintf(value_text, sizeof(value_text), "%d.%d", tenths / 10, tenths % 10);
        model = valid_model(UI_SCREEN_AGING);
        (void)strcpy(model.glucose.value_text, value_text);
        EXPECT_TRUE(ui_screen_model_is_valid(&model));
        assert_visible_bounds(value_text);
    }
    {
        const char unterminated[3] = {'2', '0', '0'};
        EXPECT_FALSE(ui_screen_value_visible_bounds(unterminated, sizeof(unterminated),
                                                    &left, &right));
    }
    EXPECT_FALSE(ui_screen_value_visible_bounds("", sizeof(""), &left, &right));
    EXPECT_FALSE(ui_screen_value_visible_bounds("-1", sizeof("-1"), &left, &right));
    EXPECT_FALSE(ui_screen_value_visible_bounds("1x", sizeof("1x"), &left, &right));
    EXPECT_FALSE(ui_screen_value_visible_bounds(NULL, 1U, &left, &right));
    EXPECT_FALSE(ui_screen_value_visible_bounds("20", sizeof("20"), NULL, &right));
    EXPECT_FALSE(ui_screen_value_visible_bounds("20", sizeof("20"), &left, NULL));
}

static int32_t independent_nominal_width(const char *value)
{
    int32_t width = 0;
    size_t index;
    for (index = 0U; value[index] != '\0'; index++) {
        width += value[index] == '.' ? 10 : 42;
        if (value[index + 1U] != '\0') {
            width += 5;
        }
    }
    return width;
}

static bool render_model(ui_screen_model_t *model, lv_obj_t **old_screen)
{
    lv_obj_t *screen = ui_screen_create(model);
    if (screen == NULL) {
        return false;
    }
    memset(test_framebuffer, 0, sizeof(test_framebuffer));
    test_flush_failed = false;
    lv_screen_load(screen);
    if (*old_screen != NULL && *old_screen != screen) {
        lv_obj_delete(*old_screen);
    }
    *old_screen = screen;
    lv_tick_inc(1U);
    lv_refr_now(NULL);
    return !test_flush_failed;
}

static void assert_rendered_value(ui_screen_model_t *model, lv_obj_t **old_screen)
{
    int32_t visible_left;
    int32_t visible_right;
    int32_t origin;
    int32_t expected_x1;
    int32_t expected_x2;
    int32_t nominal_x2;
    int32_t actual_x1 = UI_SCREEN_WIDTH;
    int32_t actual_x2 = -1;
    int32_t arrow_x1 = UI_SCREEN_WIDTH;
    int32_t arrow_x2 = -1;
    uint16_t background;
    bool number_rgb_equal = true;
    bool arrow_found = false;
    int32_t x;
    int32_t y;

    expected_visible_bounds(model->glucose.value_text, &visible_left, &visible_right);
    origin = (UI_SCREEN_WIDTH - (visible_right - visible_left + 1)) / 2 - visible_left;
    expected_x1 = origin + visible_left;
    expected_x2 = origin + visible_right;
    nominal_x2 = origin + independent_nominal_width(model->glucose.value_text) - 1;

    model->glucose.trend = GLUCOSE_TREND_UNKNOWN;
    if (!render_model(model, old_screen)) {
        EXPECT_TRUE(false);
        return;
    }
    background = test_framebuffer[(size_t)80 * UI_SCREEN_WIDTH];
    for (y = 35; y < 132; y++) {
        for (x = 0; x < UI_SCREEN_WIDTH; x++) {
            size_t index = (size_t)y * UI_SCREEN_WIDTH + (size_t)x;
            if (test_framebuffer[index] != background) {
                if (x < actual_x1) {
                    actual_x1 = x;
                }
                if (x > actual_x2) {
                    actual_x2 = x;
                }
            }
        }
    }
    EXPECT_TRUE(actual_x1 == expected_x1);
    EXPECT_TRUE(actual_x2 == expected_x2);
    memcpy(baseline_framebuffer, test_framebuffer, sizeof(baseline_framebuffer));

    model->glucose.trend = GLUCOSE_TREND_STABLE;
    if (!render_model(model, old_screen)) {
        EXPECT_TRUE(false);
        return;
    }
    for (y = 35; y < 132; y++) {
        for (x = 0; x < UI_SCREEN_WIDTH; x++) {
            size_t index = (size_t)y * UI_SCREEN_WIDTH + (size_t)x;
            bool changed = test_framebuffer[index] != baseline_framebuffer[index];
            if (x >= origin && x <= nominal_x2 && changed) {
                number_rgb_equal = false;
            }
            if ((x < origin || x > nominal_x2) && changed) {
                arrow_found = true;
                if (x < arrow_x1) {
                    arrow_x1 = x;
                }
                if (x > arrow_x2) {
                    arrow_x2 = x;
                }
            }
        }
    }
    EXPECT_TRUE(number_rgb_equal);
    EXPECT_TRUE(arrow_found);
    EXPECT_TRUE(arrow_x1 > nominal_x2);
    EXPECT_TRUE(arrow_x2 < UI_SCREEN_WIDTH);
}

static void test_every_accepted_value_renders_with_independent_geometry(void)
{
    lv_display_t *display;
    lv_obj_t *old_screen;
    ui_screen_model_t model;
    char value_text[GLUCOSE_VALUE_TEXT_CAPACITY];
    int value;
    int tenths;

    lv_init();
    display = lv_display_create(UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    EXPECT_TRUE(display != NULL);
    if (display == NULL) {
        lv_deinit();
        return;
    }
    lv_display_set_default(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, test_draw_buffer.pixels, NULL,
                           (uint32_t)sizeof(test_draw_buffer.pixels),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, test_flush_callback);
    old_screen = lv_display_get_screen_active(display);

    for (value = 20; value <= 500; value++) {
        (void)snprintf(value_text, sizeof(value_text), "%d", value);
        model = valid_model(UI_SCREEN_FRESH);
        (void)strcpy(model.glucose.value_text, value_text);
        assert_rendered_value(&model, &old_screen);
    }
    for (tenths = 11; tenths <= 278; tenths++) {
        (void)snprintf(value_text, sizeof(value_text), "%d.%d", tenths / 10, tenths % 10);
        model = valid_model(UI_SCREEN_AGING);
        (void)strcpy(model.glucose.value_text, value_text);
        assert_rendered_value(&model, &old_screen);
    }

    lv_display_delete(display);
    lv_deinit();
}

static void test_every_kind_has_a_valid_model(void)
{
    int kind;
    for (kind = UI_SCREEN_BOOT; kind < UI_SCREEN_COUNT; kind++) {
        ui_screen_model_t model = valid_model((ui_screen_kind_t)kind);
        EXPECT_TRUE(ui_screen_model_is_valid(&model));
        EXPECT_TRUE(strcmp(ui_screen_kind_name((ui_screen_kind_t)kind), "invalid") != 0);
    }
    EXPECT_TRUE(strcmp(ui_screen_kind_name((ui_screen_kind_t)-1), "invalid") == 0);
    EXPECT_TRUE(strcmp(ui_screen_kind_name(UI_SCREEN_COUNT), "invalid") == 0);
}

static void test_no_number_and_hue_gate(void)
{
    ui_screen_model_t model = valid_model(UI_SCREEN_STALE);
    model.glucose.show_number = true;
    (void)strcpy(model.glucose.value_text, "112");
    (void)strcpy(model.glucose.unit_text, "mg/dL");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
    EXPECT_TRUE(ui_screen_create(&model) == NULL);

    model = valid_model(UI_SCREEN_AUTH_ERROR);
    model.glucose.use_range_hue = true;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    model.glucose.show_number = false;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

static void test_value_and_unit_validation(void)
{
    ui_screen_model_t model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "11x");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "1.1.2");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.unit_text, "MG/DL");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_AGING);
    model.glucose.state = GLUCOSE_STATE_FRESH;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "19");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "20");
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "500");
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.glucose.value_text, "501");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_AGING);
    (void)strcpy(model.glucose.value_text, "1.0");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_AGING);
    (void)strcpy(model.glucose.value_text, "1.1");
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_AGING);
    (void)strcpy(model.glucose.value_text, "27.8");
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_AGING);
    (void)strcpy(model.glucose.value_text, "27.9");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

static void test_clock_validation(void)
{
    ui_screen_model_t model = valid_model(UI_SCREEN_FRESH);
    model.clock_text[0] = '\0';
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.clock_text, "9:38");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.clock_text, "24:00");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    (void)strcpy(model.clock_text, "23:59");
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_NO_DATA);
    (void)strcpy(model.clock_text, "09:38");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

static void test_bounds_and_enum_validation(void)
{
    static const int8_t jitter_corners[][2] = {
        {-UI_SCREEN_JITTER_LIMIT_PX, -UI_SCREEN_JITTER_LIMIT_PX},
        {-UI_SCREEN_JITTER_LIMIT_PX, UI_SCREEN_JITTER_LIMIT_PX},
        {UI_SCREEN_JITTER_LIMIT_PX, -UI_SCREEN_JITTER_LIMIT_PX},
        {UI_SCREEN_JITTER_LIMIT_PX, UI_SCREEN_JITTER_LIMIT_PX}
    };
    ui_screen_model_t model;
    size_t index;

    for (index = 0U; index < sizeof(jitter_corners) / sizeof(jitter_corners[0]); index++) {
        model = valid_model(UI_SCREEN_FRESH);
        model.jitter_x = jitter_corners[index][0];
        model.jitter_y = jitter_corners[index][1];
        EXPECT_TRUE(ui_screen_model_is_valid(&model));
    }

    model = valid_model(UI_SCREEN_FRESH);
    model.jitter_x = 3;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    model.jitter_x = -3;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    model.jitter_y = 3;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    model.jitter_y = -3;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_OTA_UPDATING);
    model.progress_percent = 101U;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_FRESH);
    model.glucose.trend = GLUCOSE_TREND_COUNT;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    for (index = 0U; index < (size_t)GLUCOSE_TREND_COUNT; index++) {
        model = valid_model(UI_SCREEN_FRESH);
        model.glucose.trend = (glucose_trend_t)index;
        EXPECT_TRUE(ui_screen_model_is_valid(&model));
    }

    model = valid_model(UI_SCREEN_FRESH);
    model.glucose.range = GLUCOSE_RANGE_COUNT;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_WIFI_CONNECTING);
    model.glucose.state = GLUCOSE_STATE_BOOT;
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

static void test_provisioning_fields_are_bounded_and_screen_specific(void)
{
    ui_screen_model_t model = valid_model(UI_SCREEN_PROVISIONING);
    EXPECT_TRUE(ui_screen_model_is_valid(&model));

    model.ssid[0] = '\0';
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
    model = valid_model(UI_SCREEN_PROVISIONING);
    model.setup_password[0] = '\0';
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
    model = valid_model(UI_SCREEN_PROVISIONING);
    model.setup_password[0] = 'I';
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
    model = valid_model(UI_SCREEN_PROVISIONING);
    memset(model.setup_password, 'A', sizeof(model.setup_password));
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
    model = valid_model(UI_SCREEN_PROVISIONING);
    memset(model.ssid, 'A', sizeof(model.ssid));
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_BOOT);
    (void)strcpy(model.setup_password, "AAAAAAAAAAAA");
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

static void test_unterminated_input_is_rejected(void)
{
    ui_screen_model_t model = valid_model(UI_SCREEN_FRESH);
    memset(model.glucose.value_text, '1', sizeof(model.glucose.value_text));
    EXPECT_FALSE(ui_screen_model_is_valid(&model));

    model = valid_model(UI_SCREEN_WIFI_CONNECTING);
    memset(model.ssid, 'A', sizeof(model.ssid));
    EXPECT_FALSE(ui_screen_model_is_valid(&model));
}

int main(void)
{
    test_every_kind_has_a_valid_model();
    test_no_number_and_hue_gate();
    test_value_and_unit_validation();
    test_clock_validation();
    test_bounds_and_enum_validation();
    test_provisioning_fields_are_bounded_and_screen_specific();
    test_unterminated_input_is_rejected();
    test_every_accepted_value_has_independent_visible_bounds();
    test_every_accepted_value_renders_with_independent_geometry();

    printf("ui-model-tests assertions=%u failures=%u\n", assertions, failures);
    return failures == 0U ? 0 : 1;
}
