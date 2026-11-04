#include "glucose_core.h"

#include <limits.h>

bool glucose_backoff_config_is_valid(const glucose_backoff_config_t *config)
{
    return config != NULL && config->minimum_s > 0U &&
           config->minimum_s <= config->base_s &&
           config->base_s <= config->maximum_s &&
           config->jitter_s <= config->maximum_s;
}

bool glucose_backoff_delay_s(const glucose_backoff_config_t *config,
                             uint32_t consecutive_failures,
                             uint32_t jitter_sample,
                             uint32_t *delay_out)
{
    uint32_t exponent;
    uint64_t delay;
    int64_t jitter;
    int64_t adjusted;

    if (delay_out == NULL) {
        return false;
    }
    *delay_out = 0U;
    if (!glucose_backoff_config_is_valid(config) ||
        jitter_sample > GLUCOSE_JITTER_SAMPLE_MAX) {
        return false;
    }

    exponent = consecutive_failures > 0U ? consecutive_failures - 1U : 0U;
    delay = config->base_s;
    while (exponent > 0U && delay < config->maximum_s) {
        if (delay > (uint64_t)config->maximum_s / 2U) {
            delay = config->maximum_s;
            break;
        }
        delay *= 2U;
        exponent--;
    }
    if (delay > config->maximum_s) {
        delay = config->maximum_s;
    }

    jitter = ((int64_t)(2U * (uint64_t)config->jitter_s * jitter_sample) /
              GLUCOSE_JITTER_SAMPLE_MAX) - (int64_t)config->jitter_s;
    adjusted = (int64_t)delay + jitter;
    if (adjusted < (int64_t)config->minimum_s) {
        adjusted = config->minimum_s;
    } else if (adjusted > (int64_t)config->maximum_s) {
        adjusted = config->maximum_s;
    }
    *delay_out = (uint32_t)adjusted;
    return true;
}
