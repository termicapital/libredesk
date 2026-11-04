#include "glucose_core.h"

#include <math.h>
#include <stdio.h>

static bool glucose_value_is_valid(double glucose_mg_dl, glucose_unit_t unit)
{
    if (!isfinite(glucose_mg_dl)) {
        return false;
    }
    if (unit == GLUCOSE_UNIT_MG_DL) {
        return glucose_mg_dl >= 20.0 && glucose_mg_dl <= 500.0;
    }
    if (unit == GLUCOSE_UNIT_MMOL_L) {
        /* The frozen wire contract permits 1.0..28.0 mmol/L. */
        return glucose_mg_dl >= 18.0 && glucose_mg_dl <= 504.0;
    }
    return false;
}

bool glucose_convert_from_mg_dl(double glucose_mg_dl,
                                glucose_unit_t unit,
                                double *converted_out)
{
    if (converted_out == NULL) {
        return false;
    }
    *converted_out = 0.0;
    if (!glucose_value_is_valid(glucose_mg_dl, unit)) {
        return false;
    }
    if (unit == GLUCOSE_UNIT_MG_DL) {
        *converted_out = glucose_mg_dl;
        return true;
    }
    if (unit == GLUCOSE_UNIT_MMOL_L) {
        *converted_out = glucose_mg_dl / 18.0;
        return true;
    }
    return false;
}

bool glucose_format_value(double glucose_mg_dl,
                          glucose_unit_t unit,
                          char *buffer,
                          size_t buffer_size)
{
    double converted;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    if (!glucose_convert_from_mg_dl(glucose_mg_dl, unit, &converted)) {
        return false;
    }
    if (unit == GLUCOSE_UNIT_MG_DL) {
        written = snprintf(buffer, buffer_size, "%.0f", converted);
    } else {
        written = snprintf(buffer, buffer_size, "%.1f", converted);
    }
    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return false;
    }
    return true;
}

bool glucose_format_age(uint32_t age_s, char *buffer, size_t buffer_size)
{
    uint64_t minutes;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    minutes = ((uint64_t)age_s + 59U) / 60U;
    if (minutes > 9999999U) {
        minutes = 9999999U;
    }
    written = snprintf(buffer, buffer_size, "%llum", (unsigned long long)minutes);
    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return false;
    }
    return true;
}
