#include "ui_screens.h"

#include <stddef.h>
#include <string.h>

#define UI_BG_HEX 0x05080AU
#define UI_NEUTRAL_HEX 0x7D8991U
#define UI_MUTED_HEX 0x54616AU
#define UI_WHITE_HEX 0xECF4F7U
#define UI_URGENT_HEX 0xE63746U
#define UI_LOW_HEX 0xFF6254U
#define UI_HIGH_HEX 0xF2A72EU

#define DIGIT_WIDTH 42
#define DIGIT_HEIGHT 86
#define DIGIT_THICKNESS 7
#define DIGIT_GAP 5
#define DECIMAL_WIDTH 10
#define VALUE_TOP 40

#define SEG_A (1U << 0U)
#define SEG_B (1U << 1U)
#define SEG_C (1U << 2U)
#define SEG_D (1U << 3U)
#define SEG_E (1U << 4U)
#define SEG_F (1U << 5U)
#define SEG_G (1U << 6U)

static const char *const kind_names[UI_SCREEN_COUNT] = {
    "boot",
    "provisioning",
    "wifi_connecting",
    "time_syncing",
    "fetching",
    "fresh",
    "aging",
    "stale",
    "no_data",
    "auth_error",
    "terms_required",
    "server_error",
    "no_wifi",
    "ota_updating"
};

static const uint8_t digit_segments[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G
};

/* LVGL itself is single-owner in this firmware; a failed child allocation
 * invalidates the entire screen so a partial digit can never be displayed. */
static bool render_failed;

static lv_color_t ui_color(uint32_t value)
{
    return lv_color_hex(value);
}

static lv_color_t dim_color(uint32_t value)
{
    uint8_t red = (uint8_t)(((value >> 16U) & 0xFFU) * 55U / 100U);
    uint8_t green = (uint8_t)(((value >> 8U) & 0xFFU) * 55U / 100U);
    uint8_t blue = (uint8_t)((value & 0xFFU) * 55U / 100U);
    return lv_color_make(red, green, blue);
}

static bool has_nul(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static bool value_matches_unit(const char *value, const char *unit)
{
    int whole = 0;
    int tenths;
    size_t length;
    size_t index;
    if (!has_nul(value, GLUCOSE_VALUE_TEXT_CAPACITY) || value[0] == '\0') {
        return false;
    }
    length = strlen(value);
    if (strcmp(unit, "mg/dL") == 0) {
        if (length < 2U || length > 3U || value[0] == '0') {
            return false;
        }
        for (index = 0U; index < length; index++) {
            if (value[index] < '0' || value[index] > '9') {
                return false;
            }
            whole = whole * 10 + (value[index] - '0');
        }
        return whole >= 20 && whole <= 500;
    }
    if (strcmp(unit, "mmol/L") != 0 || length < 3U || length > 4U ||
        value[length - 2U] != '.' || value[0] == '0') {
        return false;
    }
    for (index = 0U; index < length - 2U; index++) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        whole = whole * 10 + (value[index] - '0');
    }
    if (value[length - 1U] < '0' || value[length - 1U] > '9') {
        return false;
    }
    tenths = whole * 10 + (value[length - 1U] - '0');
    return tenths >= 11 && tenths <= 278;
}

static bool clock_is_valid(const char *value)
{
    unsigned int hour;
    unsigned int minute;
    if (!has_nul(value, UI_SCREEN_CLOCK_TEXT_CAPACITY) || strlen(value) != 5U ||
        value[2] != ':' || value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9') {
        return false;
    }
    hour = (unsigned int)(value[0] - '0') * 10U + (unsigned int)(value[1] - '0');
    minute = (unsigned int)(value[3] - '0') * 10U + (unsigned int)(value[4] - '0');
    return hour <= 23U && minute <= 59U;
}

static bool setup_fields_are_valid(const ui_screen_model_t *model)
{
    static const char password_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    size_t index;

    if (strlen(model->ssid) != 16U || memcmp(model->ssid, "LibreDesk-", 10U) != 0 ||
        strlen(model->setup_password) != UI_SCREEN_SETUP_PASSWORD_BYTES) {
        return false;
    }
    for (index = 10U; index < 16U; index++) {
        const char value = model->ssid[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    for (index = 0U; index < UI_SCREEN_SETUP_PASSWORD_BYTES; index++) {
        if (strchr(password_alphabet, model->setup_password[index]) == NULL) {
            return false;
        }
    }
    return true;
}

const char *ui_screen_kind_name(ui_screen_kind_t kind)
{
    if (kind < UI_SCREEN_BOOT || kind >= UI_SCREEN_COUNT) {
        return "invalid";
    }
    return kind_names[kind];
}

bool ui_screen_model_is_valid(const ui_screen_model_t *model)
{
    bool value_screen;
    if (model == NULL || model->kind < UI_SCREEN_BOOT || model->kind >= UI_SCREEN_COUNT ||
        model->jitter_x < -UI_SCREEN_JITTER_LIMIT_PX ||
        model->jitter_x > UI_SCREEN_JITTER_LIMIT_PX ||
        model->jitter_y < -UI_SCREEN_JITTER_LIMIT_PX ||
        model->jitter_y > UI_SCREEN_JITTER_LIMIT_PX ||
        model->progress_percent > 100U ||
        (int)model->glucose.state < 0 || model->glucose.state >= GLUCOSE_STATE_COUNT ||
        (int)model->glucose.trend < 0 || model->glucose.trend >= GLUCOSE_TREND_COUNT ||
        (int)model->glucose.range < 0 || model->glucose.range >= GLUCOSE_RANGE_COUNT ||
        !has_nul(model->ssid, sizeof(model->ssid)) ||
        !has_nul(model->setup_password, sizeof(model->setup_password)) ||
        !has_nul(model->clock_text, sizeof(model->clock_text)) ||
        !has_nul(model->glucose.value_text, sizeof(model->glucose.value_text)) ||
        !has_nul(model->glucose.unit_text, sizeof(model->glucose.unit_text)) ||
        !has_nul(model->glucose.age_text, sizeof(model->glucose.age_text)) ||
        !has_nul(model->glucose.status_text, sizeof(model->glucose.status_text))) {
        return false;
    }

    value_screen = model->kind == UI_SCREEN_FRESH || model->kind == UI_SCREEN_AGING;
    if (value_screen) {
        glucose_display_state_t expected = model->kind == UI_SCREEN_FRESH ? GLUCOSE_STATE_FRESH : GLUCOSE_STATE_AGING;
        if (model->glucose.state != expected || !model->glucose.show_number ||
            !model->glucose.use_range_hue ||
            !value_matches_unit(model->glucose.value_text, model->glucose.unit_text) ||
            !clock_is_valid(model->clock_text)) {
            return false;
        }
    } else if (model->glucose.show_number || model->glucose.use_range_hue ||
               model->clock_text[0] != '\0') {
        return false;
    }
    if (model->kind == UI_SCREEN_PROVISIONING) {
        if (!setup_fields_are_valid(model)) {
            return false;
        }
    } else if (model->setup_password[0] != '\0') {
        return false;
    }

    switch (model->kind) {
        case UI_SCREEN_BOOT:
        case UI_SCREEN_PROVISIONING:
        case UI_SCREEN_OTA_UPDATING:
            return model->glucose.state == GLUCOSE_STATE_BOOT;
        case UI_SCREEN_WIFI_CONNECTING:
            return model->glucose.state == GLUCOSE_STATE_WIFI_CONNECTING;
        case UI_SCREEN_TIME_SYNCING:
            return model->glucose.state == GLUCOSE_STATE_TIME_SYNCING;
        case UI_SCREEN_FETCHING:
            return model->glucose.state == GLUCOSE_STATE_FETCHING;
        case UI_SCREEN_FRESH:
        case UI_SCREEN_AGING:
            return true;
        case UI_SCREEN_STALE:
            return model->glucose.state == GLUCOSE_STATE_STALE;
        case UI_SCREEN_NO_DATA:
            return model->glucose.state == GLUCOSE_STATE_NO_DATA;
        case UI_SCREEN_AUTH_ERROR:
            return model->glucose.state == GLUCOSE_STATE_AUTH_ERROR;
        case UI_SCREEN_TERMS_REQUIRED:
            return model->glucose.state == GLUCOSE_STATE_AUTH_ERROR;
        case UI_SCREEN_SERVER_ERROR:
            return model->glucose.state == GLUCOSE_STATE_SRV_ERROR;
        case UI_SCREEN_NO_WIFI:
            return model->glucose.state == GLUCOSE_STATE_NO_WIFI;
        default:
            return true;
    }
}

static lv_obj_t *plain_object(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height, lv_color_t color,
                              int32_t radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (object == NULL) {
        render_failed = true;
        return NULL;
    }
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_outline_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, int32_t x,
                            int32_t y, int32_t width, lv_color_t color,
                            lv_text_align_t alignment)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        render_failed = true;
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 20);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(label, alignment, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    return label;
}

static void draw_segment(lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t width, int32_t height, lv_color_t color)
{
    (void)plain_object(parent, x, y, width, height, color,
                       (width < height ? width : height) / 2);
}

static uint8_t digit_segment_mask(char digit)
{
    if (digit == '-') {
        return SEG_G;
    }
    if (digit >= '0' && digit <= '9') {
        return digit_segments[(unsigned)(digit - '0')];
    }
    return 0U;
}

static void draw_digit(lv_obj_t *parent, char digit, int32_t x, int32_t y,
                       lv_color_t color)
{
    uint8_t segments = digit_segment_mask(digit);
    int32_t vertical_height = (DIGIT_HEIGHT - (3 * DIGIT_THICKNESS)) / 2;
    int32_t middle_y = (DIGIT_HEIGHT - DIGIT_THICKNESS) / 2;
    int32_t lower_y = middle_y + DIGIT_THICKNESS;

    if (segments == 0U) {
        return;
    }

    if ((segments & SEG_A) != 0U) {
        draw_segment(parent, x + DIGIT_THICKNESS, y,
                     DIGIT_WIDTH - 2 * DIGIT_THICKNESS, DIGIT_THICKNESS, color);
    }
    if ((segments & SEG_G) != 0U) {
        draw_segment(parent, x + DIGIT_THICKNESS, y + middle_y,
                     DIGIT_WIDTH - 2 * DIGIT_THICKNESS, DIGIT_THICKNESS, color);
    }
    if ((segments & SEG_D) != 0U) {
        draw_segment(parent, x + DIGIT_THICKNESS, y + DIGIT_HEIGHT - DIGIT_THICKNESS,
                     DIGIT_WIDTH - 2 * DIGIT_THICKNESS, DIGIT_THICKNESS, color);
    }
    if ((segments & SEG_F) != 0U) {
        draw_segment(parent, x, y + DIGIT_THICKNESS,
                     DIGIT_THICKNESS, vertical_height, color);
    }
    if ((segments & SEG_B) != 0U) {
        draw_segment(parent, x + DIGIT_WIDTH - DIGIT_THICKNESS, y + DIGIT_THICKNESS,
                     DIGIT_THICKNESS, vertical_height, color);
    }
    if ((segments & SEG_E) != 0U) {
        draw_segment(parent, x, y + lower_y,
                     DIGIT_THICKNESS, vertical_height, color);
    }
    if ((segments & SEG_C) != 0U) {
        draw_segment(parent, x + DIGIT_WIDTH - DIGIT_THICKNESS, y + lower_y,
                     DIGIT_THICKNESS, vertical_height, color);
    }
}

static int32_t value_width(const char *value)
{
    int32_t width = 0;
    size_t index;
    for (index = 0U; value[index] != '\0'; index++) {
        width += value[index] == '.' ? DECIMAL_WIDTH : DIGIT_WIDTH;
        if (value[index + 1U] != '\0') {
            width += DIGIT_GAP;
        }
    }
    return width;
}

static int32_t digit_visible_left(uint8_t segments)
{
    if ((segments & (SEG_E | SEG_F)) != 0U) {
        return 0;
    }
    if ((segments & (SEG_A | SEG_D | SEG_G)) != 0U) {
        return DIGIT_THICKNESS;
    }
    return DIGIT_WIDTH - DIGIT_THICKNESS;
}

static int32_t digit_visible_right(uint8_t segments)
{
    if ((segments & (SEG_B | SEG_C)) != 0U) {
        return DIGIT_WIDTH - 1;
    }
    if ((segments & (SEG_A | SEG_D | SEG_G)) != 0U) {
        return DIGIT_WIDTH - DIGIT_THICKNESS - 1;
    }
    return DIGIT_THICKNESS - 1;
}

bool ui_screen_value_visible_bounds(const char *value, size_t value_capacity,
                                    int32_t *left, int32_t *right)
{
    const char *terminator;
    size_t length;
    bool initialized = false;
    int32_t cursor = 0;
    size_t index;

    if (value == NULL || value_capacity == 0U || left == NULL || right == NULL) {
        return false;
    }
    terminator = (const char *)memchr(value, '\0', value_capacity);
    if (terminator == NULL || terminator == value) {
        return false;
    }
    length = (size_t)(terminator - value);
    for (index = 0U; index < length; index++) {
        int32_t character_left;
        int32_t character_right;
        if (value[index] == '.') {
            character_left = cursor + 1;
            character_right = cursor + DIGIT_THICKNESS;
            cursor += DECIMAL_WIDTH;
        } else {
            uint8_t segments = digit_segment_mask(value[index]);
            if (value[index] < '0' || value[index] > '9' || segments == 0U) {
                return false;
            }
            character_left = cursor + digit_visible_left(segments);
            character_right = cursor + digit_visible_right(segments);
            cursor += DIGIT_WIDTH;
        }
        if (!initialized) {
            *left = character_left;
            initialized = true;
        }
        *right = character_right;
        if (index + 1U < length) {
            cursor += DIGIT_GAP;
        }
    }
    return initialized;
}

static void draw_value(lv_obj_t *parent, const char *value, int32_t x,
                       int32_t y, lv_color_t color)
{
    size_t index;
    int32_t cursor = x;
    for (index = 0U; value[index] != '\0'; index++) {
        if (value[index] == '.') {
            draw_segment(parent, cursor + 1, y + DIGIT_HEIGHT - DIGIT_THICKNESS,
                         DIGIT_THICKNESS, DIGIT_THICKNESS, color);
            cursor += DECIMAL_WIDTH;
        } else {
            draw_digit(parent, value[index], cursor, y, color);
            cursor += DIGIT_WIDTH;
        }
        if (value[index + 1U] != '\0') {
            cursor += DIGIT_GAP;
        }
    }
}

static void draw_trend(lv_obj_t *parent, glucose_trend_t trend,
                       int32_t x, int32_t y, lv_color_t color)
{
    static lv_point_precise_t flat_points[] = {
        {0, 17}, {31, 17}, {22, 8}, {31, 17}, {22, 26}
    };
    static lv_point_precise_t up_points[] = {
        {2, 30}, {30, 2}, {19, 2}, {30, 2}, {30, 13}
    };
    static lv_point_precise_t down_points[] = {
        {2, 2}, {30, 30}, {30, 19}, {30, 30}, {19, 30}
    };
    lv_point_precise_t *points = NULL;
    uint32_t point_count = 0U;
    lv_obj_t *line;

    if (trend == GLUCOSE_TREND_STABLE) {
        points = flat_points;
        point_count = (uint32_t)(sizeof(flat_points) / sizeof(flat_points[0]));
    } else if (trend == GLUCOSE_TREND_RISING ||
               trend == GLUCOSE_TREND_RISING_SLIGHTLY ||
               trend == GLUCOSE_TREND_RISING_FAST) {
        points = up_points;
        point_count = (uint32_t)(sizeof(up_points) / sizeof(up_points[0]));
    } else if (trend == GLUCOSE_TREND_FALLING ||
               trend == GLUCOSE_TREND_FALLING_SLIGHTLY ||
               trend == GLUCOSE_TREND_FALLING_FAST) {
        points = down_points;
        point_count = (uint32_t)(sizeof(down_points) / sizeof(down_points[0]));
    } else {
        return;
    }

    line = lv_line_create(parent);
    if (line == NULL) {
        render_failed = true;
        return;
    }
    lv_line_set_points(line, points, point_count);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, 4, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

static lv_color_t value_color(const ui_screen_model_t *model)
{
    uint32_t color;
    switch (model->glucose.range) {
        case GLUCOSE_RANGE_URGENT_LOW:
            color = UI_URGENT_HEX;
            break;
        case GLUCOSE_RANGE_LOW:
            color = UI_LOW_HEX;
            break;
        case GLUCOSE_RANGE_HIGH:
            color = UI_HIGH_HEX;
            break;
        case GLUCOSE_RANGE_IN_RANGE:
        case GLUCOSE_RANGE_UNKNOWN:
        default:
            color = UI_WHITE_HEX;
            break;
    }
    return model->kind == UI_SCREEN_AGING ? dim_color(color) : ui_color(color);
}

static void draw_wifi_bars(lv_obj_t *parent, bool disconnected)
{
    lv_color_t color = ui_color(UI_NEUTRAL_HEX);
    int32_t index;
    for (index = 0; index < 4; index++) {
        int32_t height = 4 + index * 4;
        draw_segment(parent, 197 + index * 7, 226 - height, 4, height, color);
    }
    if (disconnected) {
        static lv_point_precise_t slash[] = {{0, 0}, {31, 22}};
        lv_obj_t *line = lv_line_create(parent);
        if (line != NULL) {
            lv_line_set_points(line, slash, 2U);
            lv_obj_set_pos(line, 195, 202);
            lv_obj_set_style_line_color(line, ui_color(UI_WHITE_HEX), 0);
            lv_obj_set_style_line_width(line, 3, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        }
        else {
            render_failed = true;
        }
    }
}

static void sanitize_ssid(char *destination, size_t capacity, const char *source)
{
    size_t index;
    if (capacity == 0U) {
        return;
    }
    for (index = 0U; index + 1U < capacity && source[index] != '\0'; index++) {
        unsigned char byte = (unsigned char)source[index];
        destination[index] = byte >= 0x20U && byte <= 0x7EU ? (char)byte : '?';
    }
    destination[index] = '\0';
}

static void draw_footer(lv_obj_t *root, const ui_screen_model_t *model,
                        bool disconnected)
{
    const char *age = model->glucose.age_text[0] == '\0' ? "--" : model->glucose.age_text;
    lv_color_t age_color = model->kind == UI_SCREEN_AGING
                               ? ui_color(UI_WHITE_HEX)
                               : ui_color(UI_NEUTRAL_HEX);
    (void)text_label(root, age, 18, 207, 88, age_color, LV_TEXT_ALIGN_LEFT);
    draw_wifi_bars(root, disconnected);
}

static void draw_data_screen(lv_obj_t *root, const ui_screen_model_t *model)
{
    lv_color_t color = value_color(model);
    bool show_arrow = model->glucose.trend != GLUCOSE_TREND_UNKNOWN;
    int32_t digits_width = value_width(model->glucose.value_text);
    int32_t visible_left = 0;
    int32_t visible_right = digits_width - 1;
    int32_t visible_width;
    int32_t x;
    lv_obj_t *clock;

    if (!ui_screen_value_visible_bounds(model->glucose.value_text,
                                        sizeof(model->glucose.value_text),
                                        &visible_left, &visible_right)) {
        render_failed = true;
        return;
    }
    visible_width = visible_right - visible_left + 1;
    x = (UI_SCREEN_WIDTH - visible_width) / 2 - visible_left;

    clock = text_label(root, model->clock_text, 40, 3, 160,
                       model->kind == UI_SCREEN_AGING
                           ? dim_color(UI_WHITE_HEX)
                           : ui_color(UI_WHITE_HEX),
                       LV_TEXT_ALIGN_CENTER);
    if (clock != NULL) {
        lv_obj_set_size(clock, 160, 32);
        lv_obj_set_style_text_font(clock, &lv_font_montserrat_24, 0);
    }
    draw_value(root, model->glucose.value_text, x, VALUE_TOP, color);
    if (show_arrow) {
        draw_trend(root, model->glucose.trend, x + digits_width + 10,
                   VALUE_TOP + 25, color);
    }
    if (model->kind == UI_SCREEN_AGING) {
        (void)text_label(root, "OLDER READING", 55, 174, 130,
                         ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
    }
    draw_footer(root, model, false);
}

static void draw_no_number(lv_obj_t *root, const ui_screen_model_t *model,
                           const char *status, bool disconnected)
{
    int32_t width = value_width("--");
    draw_value(root, "--", (UI_SCREEN_WIDTH - width) / 2, VALUE_TOP,
               ui_color(UI_NEUTRAL_HEX));
    (void)text_label(root, status, 40, 145, 160,
                     ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
    draw_footer(root, model, disconnected);
}

static void draw_boot(lv_obj_t *root)
{
    lv_obj_t *ring = plain_object(root, 91, 49, 58, 58,
                                  ui_color(UI_BG_HEX), LV_RADIUS_CIRCLE);
    if (ring != NULL) {
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, ui_color(UI_WHITE_HEX), 0);
    }
    draw_segment(root, 106, 62, 8, 31, ui_color(UI_WHITE_HEX));
    draw_segment(root, 126, 62, 8, 31, ui_color(UI_NEUTRAL_HEX));
    (void)text_label(root, "LIBRE DESK", 50, 127, 140,
                     ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
    (void)text_label(root, "starting", 70, 157, 100,
                     ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
}

static void draw_progress(lv_obj_t *root, uint8_t percent)
{
    int32_t width = (int32_t)percent * 160 / 100;
    (void)plain_object(root, 40, 145, 160, 6, ui_color(UI_MUTED_HEX), 3);
    if (width > 0) {
        (void)plain_object(root, 40, 145, width, 6, ui_color(UI_WHITE_HEX), 3);
    }
}

static void draw_transition(lv_obj_t *root, const ui_screen_model_t *model)
{
    switch (model->kind) {
        case UI_SCREEN_PROVISIONING:
            (void)text_label(root, "SETUP", 40, 52, 160,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, model->ssid, 16, 88, 208,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, "Password", 40, 119, 160,
                             ui_color(UI_MUTED_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, model->setup_password, 24, 141, 192,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, "Open 192.168.4.1", 24, 176, 192,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            break;
        case UI_SCREEN_WIFI_CONNECTING: {
            char safe_ssid[UI_SCREEN_SSID_MAX_BYTES + 1U];
            sanitize_ssid(safe_ssid, sizeof(safe_ssid), model->ssid);
            (void)text_label(root, "Wi-Fi...", 40, 82, 160,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, safe_ssid[0] == '\0' ? "hidden network" : safe_ssid,
                             28, 121, 184,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            draw_wifi_bars(root, false);
            break;
        }
        case UI_SCREEN_TIME_SYNCING:
            (void)text_label(root, "TIME...", 40, 96, 160,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, "secure clock", 55, 130, 130,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            break;
        case UI_SCREEN_FETCHING:
            (void)text_label(root, "...", 40, 96, 160,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            (void)text_label(root, "first reading", 55, 130, 130,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            break;
        case UI_SCREEN_OTA_UPDATING:
            (void)text_label(root, "UPDATE", 40, 92, 160,
                             ui_color(UI_WHITE_HEX), LV_TEXT_ALIGN_CENTER);
            draw_progress(root, model->progress_percent);
            (void)text_label(root, "do not unplug", 55, 167, 130,
                             ui_color(UI_NEUTRAL_HEX), LV_TEXT_ALIGN_CENTER);
            break;
        default:
            break;
    }
}

lv_obj_t *ui_screen_create(const ui_screen_model_t *model)
{
    lv_obj_t *screen;
    lv_obj_t *root;
    if (!ui_screen_model_is_valid(model)) {
        return NULL;
    }

    render_failed = false;
    screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return NULL;
    }
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, ui_color(UI_BG_HEX), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    root = plain_object(screen, model->jitter_x, model->jitter_y,
                        UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT,
                        ui_color(UI_BG_HEX), 0);
    if (root == NULL) {
        lv_obj_delete(screen);
        return NULL;
    }

    switch (model->kind) {
        case UI_SCREEN_BOOT:
            draw_boot(root);
            break;
        case UI_SCREEN_PROVISIONING:
        case UI_SCREEN_WIFI_CONNECTING:
        case UI_SCREEN_TIME_SYNCING:
        case UI_SCREEN_FETCHING:
        case UI_SCREEN_OTA_UPDATING:
            draw_transition(root, model);
            break;
        case UI_SCREEN_FRESH:
        case UI_SCREEN_AGING:
            draw_data_screen(root, model);
            break;
        case UI_SCREEN_STALE:
            draw_no_number(root, model, "STALE", false);
            break;
        case UI_SCREEN_NO_DATA:
            draw_no_number(root, model, "NO DATA", false);
            break;
        case UI_SCREEN_AUTH_ERROR:
            draw_no_number(root, model, "AUTH", false);
            break;
        case UI_SCREEN_TERMS_REQUIRED:
            draw_no_number(root, model, "TERMS", false);
            break;
        case UI_SCREEN_SERVER_ERROR:
            draw_no_number(root, model, "SRV", false);
            break;
        case UI_SCREEN_NO_WIFI:
            draw_no_number(root, model, "NO WIFI", true);
            break;
        default:
            lv_obj_delete(screen);
            return NULL;
    }
    if (render_failed) {
        lv_obj_delete(screen);
        return NULL;
    }
    return screen;
}

lv_obj_t *ui_screen_create_on_display(const ui_screen_model_t *model,
                                       lv_display_t *display)
{
    lv_display_t *previous;
    lv_obj_t *screen;
    if (display == NULL) {
        return NULL;
    }
    previous = lv_display_get_default();
    lv_display_set_default(display);
    screen = ui_screen_create(model);
    if (screen != NULL && lv_obj_get_display(screen) != display) {
        lv_obj_delete(screen);
        screen = NULL;
    }
    lv_display_set_default(previous);
    return screen;
}
