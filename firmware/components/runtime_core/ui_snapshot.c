#include "runtime_core.h"

#include <stdio.h>
#include <string.h>

static bool bounded_string(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool valid_age(const char *value)
{
    size_t length = strlen(value);
    if (strcmp(value, "--") == 0) {
        return true;
    }
    if (length < 2U || length > 3U || value[length - 1U] != 'm') {
        return false;
    }
    {
        size_t index;
        for (index = 0U; index + 1U < length; index++) {
            if (value[index] < '0' || value[index] > '9') {
                return false;
            }
        }
    }
    return true;
}

static bool valid_clock(const char *value)
{
    unsigned int hour;
    unsigned int minute;
    if (value == NULL || strlen(value) != 5U || value[2] != ':' ||
        value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9') {
        return false;
    }
    hour = (unsigned int)(value[0] - '0') * 10U + (unsigned int)(value[1] - '0');
    minute = (unsigned int)(value[3] - '0') * 10U + (unsigned int)(value[4] - '0');
    return hour <= 23U && minute <= 59U;
}

static bool format_clock(const glucose_core_t *core,
                         char output[RUNTIME_CLOCK_TEXT_CAPACITY])
{
    uint64_t current_local_seconds;
    unsigned int hour;
    unsigned int minute;
    int written;

    if (core == NULL || output == NULL || !core->has_reading ||
        core->reading.libre_local_seconds >= 24U * 60U * 60U) {
        return false;
    }
    current_local_seconds = ((uint64_t)core->reading.libre_local_seconds +
                             (uint64_t)core->current_age_s) % (24ULL * 60ULL * 60ULL);
    hour = (unsigned int)(current_local_seconds / 3600ULL);
    minute = (unsigned int)((current_local_seconds % 3600ULL) / 60ULL);
    written = snprintf(output, RUNTIME_CLOCK_TEXT_CAPACITY, "%02u:%02u", hour, minute);
    return written == 5;
}

static bool valid_value(const char *value, const char *unit)
{
    size_t index;
    size_t length = strlen(value);
    bool dot = false;
    unsigned whole = 0U;
    unsigned fraction = 0U;
    if (length == 0U || length > 7U) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        if (value[index] == '.') {
            if (dot || index == 0U || index + 1U == length) {
                return false;
            }
            dot = true;
        } else if (value[index] < '0' || value[index] > '9') {
            return false;
        } else if (dot) {
            fraction = fraction * 10U + (unsigned)(value[index] - '0');
        } else {
            whole = whole * 10U + (unsigned)(value[index] - '0');
        }
    }
    if (strcmp(unit, "mg/dL") == 0) {
        return !dot && whole >= 20U && whole <= 500U;
    }
    if (strcmp(unit, "mmol/L") == 0) {
        return dot && length <= 4U && whole >= 1U &&
               (whole < 27U || (whole == 27U && fraction <= 8U));
    }
    return false;
}

static const char *status_for_state(glucose_display_state_t state)
{
    static const char *const statuses[] = {
        "starting", "Wi-Fi", "Time", "...", "fresh", "aging", "stale",
        "no data", "AUTH", "SRV", "no wifi"
    };
    return state < GLUCOSE_STATE_COUNT ? statuses[state] : NULL;
}

static ui_screen_kind_t screen_for_state(glucose_display_state_t state)
{
    switch (state) {
    case GLUCOSE_STATE_BOOT: return UI_SCREEN_BOOT;
    case GLUCOSE_STATE_WIFI_CONNECTING: return UI_SCREEN_WIFI_CONNECTING;
    case GLUCOSE_STATE_TIME_SYNCING: return UI_SCREEN_TIME_SYNCING;
    case GLUCOSE_STATE_FETCHING: return UI_SCREEN_FETCHING;
    case GLUCOSE_STATE_FRESH: return UI_SCREEN_FRESH;
    case GLUCOSE_STATE_AGING: return UI_SCREEN_AGING;
    case GLUCOSE_STATE_STALE: return UI_SCREEN_STALE;
    case GLUCOSE_STATE_NO_DATA: return UI_SCREEN_NO_DATA;
    case GLUCOSE_STATE_AUTH_ERROR: return UI_SCREEN_AUTH_ERROR;
    case GLUCOSE_STATE_SRV_ERROR: return UI_SCREEN_SERVER_ERROR;
    case GLUCOSE_STATE_NO_WIFI: return UI_SCREEN_NO_WIFI;
    default: return UI_SCREEN_COUNT;
    }
}

void runtime_ui_snapshot_make_neutral(runtime_ui_snapshot_t *snapshot_out)
{
    if (snapshot_out == NULL) {
        return;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->screen = UI_SCREEN_NO_DATA;
    snapshot_out->model.state = GLUCOSE_STATE_NO_DATA;
    snapshot_out->model.trend = GLUCOSE_TREND_UNKNOWN;
    snapshot_out->model.range = GLUCOSE_RANGE_UNKNOWN;
    (void)strcpy(snapshot_out->model.value_text, "--");
    (void)strcpy(snapshot_out->model.unit_text, "--");
    (void)strcpy(snapshot_out->model.age_text, "--");
    (void)strcpy(snapshot_out->model.status_text, "no data");
}

bool runtime_ui_snapshot_is_valid(const runtime_ui_snapshot_t *snapshot)
{
    const glucose_display_model_t *model;
    ui_screen_kind_t expected;
    bool value_screen;

    if (snapshot == NULL || snapshot->screen < UI_SCREEN_BOOT ||
        snapshot->screen >= UI_SCREEN_COUNT) {
        return false;
    }
    model = &snapshot->model;
    if (model->state < GLUCOSE_STATE_BOOT || model->state >= GLUCOSE_STATE_COUNT ||
        model->trend < GLUCOSE_TREND_RISING || model->trend >= GLUCOSE_TREND_COUNT ||
        model->range < GLUCOSE_RANGE_URGENT_LOW || model->range >= GLUCOSE_RANGE_COUNT ||
        !bounded_string(model->value_text, sizeof(model->value_text)) ||
        !bounded_string(model->unit_text, sizeof(model->unit_text)) ||
        !bounded_string(model->age_text, sizeof(model->age_text)) ||
        !bounded_string(model->status_text, sizeof(model->status_text)) ||
        !bounded_string(snapshot->clock_text, sizeof(snapshot->clock_text))) {
        return false;
    }
    expected = screen_for_state(model->state);
    if (expected == UI_SCREEN_COUNT || snapshot->screen != expected) {
        return false;
    }
    value_screen = snapshot->screen == UI_SCREEN_FRESH || snapshot->screen == UI_SCREEN_AGING;
    if (value_screen != model->show_number || value_screen != model->use_range_hue) {
        return false;
    }
    if (value_screen ? !valid_clock(snapshot->clock_text) : snapshot->clock_text[0] != '\0') {
        return false;
    }
    if (strcmp(model->status_text, status_for_state(model->state)) != 0 ||
        (value_screen ? !valid_age(model->age_text) :
                         (strcmp(model->age_text, "") != 0 &&
                          !valid_age(model->age_text)))) {
        return false;
    }
    if (value_screen) {
        if (!valid_value(model->value_text, model->unit_text) ||
            (strcmp(model->unit_text, "mg/dL") != 0 &&
             strcmp(model->unit_text, "mmol/L") != 0)) {
            return false;
        }
    } else if (strcmp(model->value_text, "--") != 0 ||
               (strcmp(model->unit_text, "") != 0 &&
                strcmp(model->unit_text, "--") != 0)) {
        return false;
    }
    return true;
}

bool runtime_ui_snapshot_equal(const runtime_ui_snapshot_t *left,
                               const runtime_ui_snapshot_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    return left->screen == right->screen &&
           left->model.state == right->model.state &&
           left->model.show_number == right->model.show_number &&
           left->model.use_range_hue == right->model.use_range_hue &&
           left->model.trend == right->model.trend &&
           left->model.range == right->model.range &&
           strcmp(left->model.value_text, right->model.value_text) == 0 &&
           strcmp(left->model.unit_text, right->model.unit_text) == 0 &&
           strcmp(left->model.age_text, right->model.age_text) == 0 &&
           strcmp(left->model.status_text, right->model.status_text) == 0 &&
           strcmp(left->clock_text, right->clock_text) == 0;
}

bool runtime_ui_snapshot_build(const glucose_core_t *core,
                               runtime_ui_snapshot_t *snapshot_out)
{
    runtime_ui_snapshot_t candidate;

    if (snapshot_out == NULL) {
        return false;
    }
    runtime_ui_snapshot_make_neutral(snapshot_out);
    runtime_ui_snapshot_make_neutral(&candidate);
    if (core == NULL || !glucose_core_build_display(core, &candidate.model)) {
        return false;
    }
    candidate.screen = screen_for_state(candidate.model.state);
    if ((candidate.screen == UI_SCREEN_FRESH || candidate.screen == UI_SCREEN_AGING) &&
        !format_clock(core, candidate.clock_text)) {
        return false;
    }
    if (!runtime_ui_snapshot_is_valid(&candidate)) {
        return false;
    }
    *snapshot_out = candidate;
    return true;
}
