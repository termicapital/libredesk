#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "wifi_setup_sequence.h"

#define TEST_TOLERATED_START_ERROR 0x7001
#define TEST_STAGE_ERROR 0x7002

typedef struct fake_setup {
    wifi_setup_operation_t fail_at;
    esp_err_t start_result;
    size_t calls;
    size_t failures;
} fake_setup_t;

static void fake_failure(void *context)
{
    fake_setup_t *fake = (fake_setup_t *)context;
    assert(fake != NULL);
    fake->failures++;
}

static esp_err_t fake_call(wifi_setup_operation_t operation, void *context)
{
    fake_setup_t *fake = (fake_setup_t *)context;
    assert(fake != NULL);
    assert((size_t)operation == fake->calls);
    fake->calls++;
    if (operation == fake->fail_at) {
        return TEST_STAGE_ERROR;
    }
    if (operation == WIFI_SETUP_OPERATION_START) {
        return fake->start_result;
    }
    return ESP_OK;
}

static void test_every_setup_operation_fails_closed_and_stops(void)
{
    for (size_t index = 0U; index < (size_t)WIFI_SETUP_OPERATION_COUNT; index++) {
        fake_setup_t fake = {
            .fail_at = (wifi_setup_operation_t)index,
            .start_result = ESP_OK,
            .calls = 0U,
            .failures = 0U,
        };
        assert(wifi_setup_sequence_run(fake_call, &fake,
                                       TEST_TOLERATED_START_ERROR,
                                       fake_failure, &fake) ==
               TEST_STAGE_ERROR);
        assert(fake.calls == index + 1U);
        assert(fake.failures == 1U);
    }
}

static void test_tolerated_start_result_completes_setup(void)
{
    fake_setup_t fake = {
        .fail_at = WIFI_SETUP_OPERATION_COUNT,
        .start_result = TEST_TOLERATED_START_ERROR,
        .calls = 0U,
        .failures = 0U,
    };
    assert(wifi_setup_sequence_run(fake_call, &fake,
                                   TEST_TOLERATED_START_ERROR,
                                   fake_failure, &fake) == ESP_OK);
    assert(fake.calls == (size_t)WIFI_SETUP_OPERATION_COUNT);
    assert(fake.failures == 0U);
}

static void test_invalid_callback_is_rejected_without_calls(void)
{
    fake_setup_t fake = { 0 };
    assert(wifi_setup_sequence_run(NULL, NULL, TEST_TOLERATED_START_ERROR,
                                   fake_failure, &fake) ==
           ESP_ERR_INVALID_ARG);
    assert(fake.failures == 1U);
}

int main(void)
{
    test_every_setup_operation_fails_closed_and_stops();
    test_tolerated_start_result_completes_setup();
    test_invalid_callback_is_rejected_without_calls();
    puts("wifi setup sequence tests: PASS");
    return 0;
}
