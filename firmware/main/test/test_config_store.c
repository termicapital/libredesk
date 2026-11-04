#include "config_store.h"
#include "provisioning.h"
#include "config_store_host.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static runtime_config_t valid_config(void)
{
    runtime_config_t config;
    size_t index;

    memset(&config, 0, sizeof(config));
    strcpy(config.wifi_ssid, "Desk");
    strcpy(config.wifi_password, "desk-password");
    strcpy(config.bridge_origin, "https://bridge.example");
    for (index = 0U; index < 43U; index++) {
        config.device_token[index] = 'A';
    }
    config.device_token[43] = '\0';
    strcpy(config.tz, "UTC0");
    config.unit = GLUCOSE_UNIT_MG_DL;
    config.brightness_day_percent = 80U;
    config.brightness_night_percent = 30U;
    config.night_start_minute = 1200U;
    config.night_end_minute = 360U;
    config.freshness.fresh_max_s = 360U;
    config.freshness.aging_max_s = 900U;
    config.freshness.stale_max_s = 3600U;
    config.poll_s = 60U;
    config.ota_enabled = 1U;
    return config;
}

static runtime_config_t second_config(void)
{
    runtime_config_t config = valid_config();

    strcpy(config.wifi_ssid, "Desk-2");
    strcpy(config.wifi_password, "second-password");
    config.brightness_day_percent = 90U;
    config.brightness_night_percent = 40U;
    config.poll_s = 120U;
    return config;
}

static runtime_config_t third_config(void)
{
    runtime_config_t config = second_config();

    strcpy(config.wifi_ssid, "Desk-3");
    config.brightness_day_percent = 70U;
    config.brightness_night_percent = 20U;
    config.poll_s = 180U;
    return config;
}

static bool config_equal(const runtime_config_t *left,
                         const runtime_config_t *right)
{
    return strcmp(left->wifi_ssid, right->wifi_ssid) == 0 &&
           strcmp(left->wifi_password, right->wifi_password) == 0 &&
           strcmp(left->bridge_origin, right->bridge_origin) == 0 &&
           strcmp(left->device_token, right->device_token) == 0 &&
           strcmp(left->tz, right->tz) == 0 &&
           left->unit == right->unit &&
           left->brightness_day_percent == right->brightness_day_percent &&
           left->brightness_night_percent == right->brightness_night_percent &&
           left->night_start_minute == right->night_start_minute &&
           left->night_end_minute == right->night_end_minute &&
           left->freshness.fresh_max_s == right->freshness.fresh_max_s &&
           left->freshness.aging_max_s == right->freshness.aging_max_s &&
           left->freshness.stale_max_s == right->freshness.stale_max_s &&
           left->poll_s == right->poll_s &&
           left->ota_enabled == right->ota_enabled;
}

static unsigned report_save_before_after;
static unsigned report_first_save_operations;
static unsigned report_first_save_cases;
static unsigned report_ab_operations;
static unsigned report_ab_cases;
static unsigned report_ba_operations;
static unsigned report_ba_cases;
static unsigned report_load_before_after;
static unsigned report_invalid_recovery_representatives;
static unsigned report_invalid_recovery_cases;

static bool assert_production_trace_locked(void);

static bool output_is_zero(const runtime_config_t *config)
{
    const unsigned char *bytes = (const unsigned char *)config;
    size_t index;

    for (index = 0U; index < sizeof(*config); index++) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static uint32_t crc_update(uint32_t crc, uint8_t value)
{
    unsigned bit;

    crc ^= value;
    for (bit = 0U; bit < 8U; bit++) {
        crc = (crc & 1U) != 0U ? (crc >> 1U) ^ UINT32_C(0xedb88320) : crc >> 1U;
    }
    return crc;
}

static uint32_t crc_u32(uint32_t crc, uint32_t value)
{
    unsigned index;

    for (index = 0U; index < 4U; index++) {
        crc = crc_update(crc, (uint8_t)(value >> (index * 8U)));
    }
    return crc;
}

static uint32_t crc_u16(uint32_t crc, uint16_t value)
{
    crc = crc_update(crc, (uint8_t)value);
    return crc_update(crc, (uint8_t)(value >> 8U));
}

static uint32_t crc_string(uint32_t crc, const char *text)
{
    const size_t length = strlen(text);
    size_t index;

    crc = crc_u32(crc, (uint32_t)length);
    for (index = 0U; index < length; index++) {
        crc = crc_update(crc, (uint8_t)text[index]);
    }
    return crc;
}

static uint32_t expected_checksum(const runtime_config_t *config,
                                  uint32_t generation)
{
    uint32_t crc = UINT32_C(0xffffffff);

    crc = crc_u32(crc, 1U);
    crc = crc_u32(crc, generation);
    crc = crc_string(crc, config->wifi_ssid);
    crc = crc_string(crc, config->wifi_password);
    crc = crc_string(crc, config->bridge_origin);
    crc = crc_string(crc, config->device_token);
    crc = crc_update(crc, (uint8_t)config->unit);
    crc = crc_update(crc, config->brightness_day_percent);
    crc = crc_update(crc, config->brightness_night_percent);
    crc = crc_u16(crc, config->night_start_minute);
    crc = crc_u16(crc, config->night_end_minute);
    crc = crc_string(crc, config->tz);
    crc = crc_u32(crc, config->freshness.fresh_max_s);
    crc = crc_u32(crc, config->freshness.aging_max_s);
    crc = crc_u32(crc, config->freshness.stale_max_s);
    crc = crc_u32(crc, config->poll_s);
    crc = crc_update(crc, config->ota_enabled);
    return crc ^ UINT32_C(0xffffffff);
}

static bool trace_has_kind(task7_host_trace_kind_t kind)
{
    size_t index;

    for (index = 0U; index < task7_host_trace_count(); index++) {
        if (task7_host_trace_at(index)->kind == kind) {
            return true;
        }
    }
    return false;
}

static bool test_authentic_handle_transactions_and_selector_recovery(void)
{
    nvs_handle_t read_handle = 0U;
    nvs_handle_t write_handle = 0U;
    nvs_handle_t handles[4] = { 0U, 0U, 0U, 0U };
    nvs_handle_t sentinel = (nvs_handle_t)UINT32_C(0xdeadbeef);
    uint8_t value = 0U;
    size_t index;
    runtime_config_t replacement = second_config();
    runtime_config_t loaded;

    task7_host_reset();
    read_handle = sentinel;
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &read_handle) == ESP_FAIL);
    CHECK(read_handle == sentinel);
    task7_host_clear_failure();
    for (index = 0U; index < 4U; index++) {
        CHECK(nvs_open("libre_desk", NVS_READWRITE, &handles[index]) == ESP_OK);
        CHECK(handles[index] != 0U);
    }
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &write_handle) == ESP_ERR_NO_MEM);
    for (index = 0U; index < 4U; index++) {
        nvs_close(handles[index]);
    }
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_ERR_NVS_NOT_FOUND);

    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &write_handle) == ESP_OK);
    CHECK(nvs_set_u8(write_handle, "volatile", 7U) == ESP_OK);
    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_ERR_NVS_NOT_FOUND);
    nvs_close(write_handle);
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &write_handle) == ESP_OK);
    CHECK(nvs_set_u8(write_handle, "volatile", 7U) == ESP_OK);
    CHECK(nvs_commit(write_handle) == ESP_OK);
    nvs_close(write_handle);
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_OK);
    nvs_close(read_handle);
    CHECK(task7_host_read_u8("volatile", &value) && value == 7U);

    CHECK(config_store_save(&replacement) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_str("active", "wrong-type");
    CHECK(config_store_save(&replacement) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &replacement));
    return true;
}

static bool test_crc_vector_and_round_trip(void)
{
    static const char vector[] = "123456789";
    runtime_config_t config = valid_config();
    runtime_config_t loaded;
    uint32_t checksum;
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;

    for (index = 0U; index < sizeof(vector) - 1U; index++) {
        crc = crc_update(crc, (uint8_t)vector[index]);
    }
    CHECK((crc ^ UINT32_C(0xffffffff)) == UINT32_C(0xcbf43926));
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_read_u32("checksuma", &checksum));
    /* External IEEE CRC-32 vector (Python struct.pack/zlib.crc32): generation=1;
     * ssid=Desk; password=desk-password; bridge=https://bridge.example;
     * token=43xA; unit=0; day=80; night=30; start=1200; end=360;
     * tz=UTC0; fresh=360; aging=900; stale=3600; poll=60; ota=1;
     * each string is u32-LE length + bytes (no NUL); CRC=0x24e6c652. */
    CHECK(checksum == UINT32_C(0x24e6c652));
    CHECK(checksum == expected_checksum(&config, 1U));
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    return true;
}

static bool test_fake_fill_then_error_reads(void)
{
    nvs_handle_t write_handle = 0U;
    nvs_handle_t read_handle = 0U;
    uint8_t u8 = 0xa5U;
    uint16_t u16 = 0xa5a5U;
    uint32_t u32 = UINT32_C(0xa5a5a5a5);
    size_t length = 0U;
    char text[32];

    task7_host_reset();
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &write_handle) == ESP_OK);
    CHECK(nvs_set_u8(write_handle, "u8", 7U) == ESP_OK);
    CHECK(nvs_set_u16(write_handle, "u16", 700U) == ESP_OK);
    CHECK(nvs_set_u32(write_handle, "u32", UINT32_C(70000)) == ESP_OK);
    CHECK(nvs_set_str(write_handle, "text", "filled") == ESP_OK);
    CHECK(nvs_commit(write_handle) == ESP_OK);
    nvs_close(write_handle);
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READONLY, &read_handle) == ESP_OK);
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_get_u8(read_handle, "u8", &u8) == ESP_FAIL && u8 == 7U);
    task7_host_clear_failure();
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_get_u16(read_handle, "u16", &u16) == ESP_FAIL && u16 == 700U);
    task7_host_clear_failure();
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_get_u32(read_handle, "u32", &u32) == ESP_FAIL && u32 == UINT32_C(70000));
    task7_host_clear_failure();
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_get_str(read_handle, "text", NULL, &length) == ESP_FAIL && length == 7U);
    task7_host_clear_failure();
    memset(text, 0xa5, sizeof(text));
    length = sizeof(text);
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_get_str(read_handle, "text", text, &length) == ESP_FAIL && strcmp(text, "filled") == 0 && length == 7U);
    task7_host_clear_failure();
    u8 = 0xa5U;
    task7_host_clear_trace();
    task7_host_fail_at(1U, ESP_FAIL);
    CHECK(nvs_get_u8(read_handle, "u8", &u8) == ESP_FAIL && u8 == 0xa5U);
    task7_host_clear_failure();
    nvs_close(read_handle);
    return true;
}

static bool test_production_fill_then_error_is_transactional(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t output;

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    task7_host_fail_at_after(2U, ESP_FAIL);
    memset(&output, 0xa5, sizeof(output));
    CHECK(config_store_load(&output) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(output_is_zero(&output));
    task7_host_clear_failure();
    return true;
}

static bool test_validation_before_io_and_output_clear(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t output;

    task7_host_reset();
    CHECK(config_store_save(NULL) == CONFIG_STORE_RESULT_BAD_ARGUMENT);
    CHECK(task7_host_operation_count() == 0U);
    config.wifi_ssid[0] = '\0';
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_INVALID);
    CHECK(task7_host_operation_count() == 0U);
    CHECK(config_store_load(NULL) == CONFIG_STORE_RESULT_BAD_ARGUMENT);
    CHECK(task7_host_operation_count() == 0U);
    memset(&output, 0xa5, sizeof(output));
    CHECK(config_store_load(&output) == CONFIG_STORE_RESULT_ABSENT);
    CHECK(output_is_zero(&output));
    memset(&output, 0xa5, sizeof(output));
    task7_host_fail_next(ESP_FAIL);
    CHECK(config_store_load(&output) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(output_is_zero(&output));
    return true;
}

static bool test_exact_keys_types_and_publication_order(void)
{
    static const char *const keys[] = {
        "wifi_ssida", "wifi_passa", "bridge_urla", "device_tokena",
        "unita", "brightness_daya", "bright_nighta", "night_starta",
        "night_enda", "tza", "thresh_fresh_sa", "thresh_aging_sa",
        "thresh_stale_sa", "poll_sa", "ota_enableda", "schemaa",
        "generationa", "checksuma"
    };
    static const nvs_type_t types[] = {
        NVS_TYPE_STR, NVS_TYPE_STR, NVS_TYPE_STR, NVS_TYPE_STR,
        NVS_TYPE_U8, NVS_TYPE_U8, NVS_TYPE_U8, NVS_TYPE_U16,
        NVS_TYPE_U16, NVS_TYPE_STR, NVS_TYPE_U32, NVS_TYPE_U32,
        NVS_TYPE_U32, NVS_TYPE_U32, NVS_TYPE_U8, NVS_TYPE_U32,
        NVS_TYPE_U32, NVS_TYPE_U32
    };
    runtime_config_t config = valid_config();
    size_t index;
    size_t set_index = 0U;
    size_t payload_commit = 0U;
    size_t valid_set = 0U;
    size_t valid_commit = 0U;
    size_t active_set = 0U;
    size_t active_commit = 0U;
    bool saw_invalidation = false;

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    for (index = 0U; index < task7_host_trace_count(); index++) {
        const task7_host_trace_event_t *event = task7_host_trace_at(index);

        if (event->key[0] != '\0') {
            CHECK(strcmp(event->namespace_name, "libre_desk") == 0);
            CHECK(strlen(event->key) <= 15U);
        }
        if (event->kind == TASK7_TRACE_ERASE_KEY) {
            CHECK(strcmp(event->key, "valida") == 0);
            saw_invalidation = true;
        }
        if (event->kind == TASK7_TRACE_SET_STR ||
            event->kind == TASK7_TRACE_SET_U8 ||
            event->kind == TASK7_TRACE_SET_U16 ||
            event->kind == TASK7_TRACE_SET_U32) {
            if (strcmp(event->key, "valida") == 0) {
                valid_set = index;
                CHECK(event->type == NVS_TYPE_U8);
                CHECK(event->value == 0xa5U);
            } else if (strcmp(event->key, "active") == 0) {
                active_set = index;
                CHECK(event->type == NVS_TYPE_U8);
                CHECK(event->value == 0U);
            } else {
                CHECK(set_index < sizeof(keys) / sizeof(keys[0]));
                CHECK(strcmp(event->key, keys[set_index]) == 0);
                CHECK(event->type == types[set_index]);
                set_index++;
            }
        }
        if (event->kind == TASK7_TRACE_COMMIT && valid_set == 0U &&
            set_index == 0U) {
            saw_invalidation = true;
        }
        if (event->kind == TASK7_TRACE_COMMIT && valid_set == 0U &&
            set_index == sizeof(keys) / sizeof(keys[0])) {
            payload_commit = index;
        }
        if (event->kind == TASK7_TRACE_COMMIT && valid_set != 0U &&
            active_set == 0U && valid_commit == 0U) {
            valid_commit = index;
        }
        if (event->kind == TASK7_TRACE_COMMIT && active_set != 0U &&
            active_commit == 0U) {
            active_commit = index;
        }
    }
    CHECK(saw_invalidation);
    CHECK(set_index == sizeof(keys) / sizeof(keys[0]));
    CHECK(payload_commit > 0U);
    CHECK(valid_set > payload_commit);
    CHECK(valid_commit > valid_set);
    CHECK(active_set > valid_commit);
    CHECK(active_commit > active_set);
    CHECK(!trace_has_kind(TASK7_TRACE_FLASH_ERASE));
    return true;
}

static bool save_and_load(const runtime_config_t *config,
                          runtime_config_t *loaded)
{
    return config_store_save(config) == CONFIG_STORE_RESULT_OK &&
           config_store_load(loaded) == CONFIG_STORE_RESULT_OK;
}

static bool test_slot_rotation_and_no_fallback(void)
{
    runtime_config_t first = valid_config();
    runtime_config_t second = second_config();
    runtime_config_t loaded;
    uint8_t active;
    uint32_t generation;

    task7_host_reset();
    CHECK(save_and_load(&first, &loaded));
    CHECK(config_equal(&loaded, &first));
    CHECK(config_store_save(&second) == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_read_u8("active", &active) && active == 1U);
    CHECK(task7_host_read_u32("generationb", &generation) && generation == 2U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &second));
    task7_host_seed_u8("valida", 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &second));
    memset(&loaded, 0xa5, sizeof(loaded));
    task7_host_seed_u8("validb", 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_INVALID);
    CHECK(output_is_zero(&loaded));
    task7_host_seed_u8("active", 2U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_INVALID);
    CHECK(output_is_zero(&loaded));
    task7_host_seed_u16("active", 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_INVALID);
    CHECK(output_is_zero(&loaded));
    task7_host_remove("active");
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    CHECK(output_is_zero(&loaded));
    return true;
}

static bool load_is_invalid_and_clear(void)
{
    runtime_config_t output;

    memset(&output, 0xa5, sizeof(output));
    CHECK(config_store_load(&output) == CONFIG_STORE_RESULT_INVALID);
    CHECK(output_is_zero(&output));
    return true;
}

static bool load_invalid_then_recover(const runtime_config_t *replacement)
{
    runtime_config_t loaded;

    CHECK(load_is_invalid_and_clear());
    CHECK(config_store_save(replacement) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, replacement));
    return true;
}

static bool test_corruption_and_bounded_reads(void)
{
    static const char *const field_keys[] = {
        "wifi_ssida", "wifi_passa", "bridge_urla", "device_tokena", "unita",
        "brightness_daya", "bright_nighta", "night_starta", "night_enda", "tza",
        "thresh_fresh_sa", "thresh_aging_sa", "thresh_stale_sa", "poll_sa",
        "ota_enableda", "schemaa", "generationa", "checksuma", "valida"
    };
    static const char *const string_keys[] = {
        "wifi_ssida", "wifi_passa", "bridge_urla", "device_tokena", "tza"
    };
    static const char *const numeric_keys[] = {
        "unita", "brightness_daya", "bright_nighta", "night_starta", "night_enda",
        "thresh_fresh_sa", "thresh_aging_sa", "thresh_stale_sa", "poll_sa",
        "ota_enableda", "schemaa", "generationa", "checksuma", "valida"
    };
    runtime_config_t config = valid_config();
    runtime_config_t loaded;
    size_t index;

    for (index = 0U; index < sizeof(field_keys) / sizeof(field_keys[0]); index++) {
        task7_host_reset();
        CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
        task7_host_remove(field_keys[index]);
        CHECK(load_invalid_then_recover(&config));
    }
    for (index = 0U; index < sizeof(string_keys) / sizeof(string_keys[0]); index++) {
        task7_host_reset();
        CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
        task7_host_seed_oversized_str(string_keys[index], 319U);
        CHECK(load_invalid_then_recover(&config));
        task7_host_reset();
        CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
        task7_host_seed_u32(string_keys[index], 4U);
        CHECK(load_invalid_then_recover(&config));
    }
    for (index = 0U; index < sizeof(numeric_keys) / sizeof(numeric_keys[0]); index++) {
        task7_host_reset();
        CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
        task7_host_seed_str(numeric_keys[index], "wrong-type");
        CHECK(load_invalid_then_recover(&config));
    }
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u32("schemaa", 2U);
    CHECK(load_invalid_then_recover(&config));
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u32("checksuma", 0U);
    CHECK(load_invalid_then_recover(&config));
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u32("generationa", 0U);
    CHECK(load_invalid_then_recover(&config));
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u8("valida", 0U);
    CHECK(load_invalid_then_recover(&config));
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    config.brightness_day_percent = 0U;
    task7_host_seed_u8("brightness_daya", 0U);
    task7_host_seed_u32("checksuma", expected_checksum(&config, 1U));
    CHECK(load_is_invalid_and_clear());
    config = valid_config();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    return true;
}

static bool measure_save_operations(const runtime_config_t *old_config,
                                    const runtime_config_t *new_config,
                                    size_t *operations_out)
{
    task7_host_reset();
    CHECK(config_store_save(old_config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_save(new_config) == CONFIG_STORE_RESULT_OK);
    *operations_out = task7_host_operation_count();
    CHECK(*operations_out > 0U);
    task7_host_reset();
    CHECK(config_store_save(old_config) == CONFIG_STORE_RESULT_OK);
    return true;
}

static bool assert_old_after_each_save_failure(const runtime_config_t *old_config,
                                               const runtime_config_t *new_config,
                                               size_t operations)
{
    runtime_config_t loaded;
    size_t failure;

    for (failure = 1U; failure <= operations; failure++) {
        task7_host_clear_trace();
        task7_host_fail_at(failure, ESP_FAIL);
        CHECK(config_store_save(new_config) == CONFIG_STORE_RESULT_IO_ERROR);
        task7_host_clear_failure();
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
        CHECK(config_equal(&loaded, old_config));
    }
    return true;
}

static bool test_interruption_preserves_previous_configuration(void)
{
    runtime_config_t old_config = valid_config();
    runtime_config_t new_config = second_config();
    runtime_config_t third = third_config();
    size_t operations;

    CHECK(measure_save_operations(&old_config, &new_config, &operations));
    CHECK(assert_old_after_each_save_failure(&old_config, &new_config, operations));
    task7_host_reset();
    CHECK(config_store_save(&old_config) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_save(&new_config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_save(&third) == CONFIG_STORE_RESULT_OK);
    operations = task7_host_operation_count();
    CHECK(operations > 0U);
    task7_host_reset();
    CHECK(config_store_save(&new_config) == CONFIG_STORE_RESULT_OK);
    CHECK(assert_old_after_each_save_failure(&new_config, &third, operations));
    return true;
}

static bool test_first_save_interruptions_remain_absent(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t loaded;
    size_t normal_operations;
    size_t failure;

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    normal_operations = task7_host_operation_count();
    CHECK(normal_operations > 0U);
    for (failure = 1U; failure <= normal_operations; failure++) {
        task7_host_reset();
        task7_host_fail_at(failure, ESP_FAIL);
        CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_IO_ERROR);
        task7_host_clear_failure();
        memset(&loaded, 0xa5, sizeof(loaded));
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
        CHECK(output_is_zero(&loaded));
    }
    return true;
}

static bool test_load_failure_matrix(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t loaded;
    size_t operations;
    size_t failure;

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    operations = task7_host_operation_count();
    CHECK(operations > 0U);
    for (failure = 1U; failure <= operations; failure++) {
        task7_host_clear_trace();
        task7_host_fail_at(failure, ESP_FAIL);
        memset(&loaded, 0xa5, sizeof(loaded));
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_IO_ERROR);
        CHECK(output_is_zero(&loaded));
        task7_host_clear_failure();
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
        CHECK(config_equal(&loaded, &config));
    }
    report_load_before_after = (unsigned)(operations * 2U);
    return true;
}

static bool assert_invalid_persisted(const char *key,
                                     const runtime_config_t *mutated,
                                     uint8_t number8,
                                     uint16_t number16,
                                     uint32_t number32,
                                     const char *text,
                                     unsigned kind)
{
    runtime_config_t base = valid_config();
    runtime_config_t loaded;

    task7_host_reset();
    CHECK(config_store_save(&base) == CONFIG_STORE_RESULT_OK);
    if (kind == 1U) {
        task7_host_seed_u8(key, number8);
    } else if (kind == 2U) {
        task7_host_seed_u16(key, number16);
    } else if (kind == 3U) {
        task7_host_seed_u32(key, number32);
    } else {
        task7_host_seed_str(key, text);
    }
    task7_host_seed_u32("checksuma", expected_checksum(mutated, 1U));
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_INVALID);
    CHECK(output_is_zero(&loaded));
    CHECK(config_store_save(&base) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &base));
    return true;
}

static bool test_semantic_validation_on_persisted_values(void)
{
    runtime_config_t mutated;

    mutated = valid_config();
    mutated.wifi_ssid[0] = '\0';
    CHECK(assert_invalid_persisted("wifi_ssida", &mutated, 0U, 0U, 0U, "", 4U));
    mutated = valid_config();
    strcpy(mutated.wifi_password, "short");
    CHECK(assert_invalid_persisted("wifi_passa", &mutated, 0U, 0U, 0U, "short", 4U));
    mutated = valid_config();
    strcpy(mutated.bridge_origin, "http://bridge.example");
    CHECK(assert_invalid_persisted("bridge_urla", &mutated, 0U, 0U, 0U,
                                   mutated.bridge_origin, 4U));
    mutated = valid_config();
    strcpy(mutated.device_token, "short");
    CHECK(assert_invalid_persisted("device_tokena", &mutated, 0U, 0U, 0U,
                                   mutated.device_token, 4U));
    mutated = valid_config();
    mutated.unit = GLUCOSE_UNIT_COUNT;
    CHECK(assert_invalid_persisted("unita", &mutated, (uint8_t)GLUCOSE_UNIT_COUNT,
                                   0U, 0U, NULL, 1U));
    mutated = valid_config();
    mutated.brightness_day_percent = 0U;
    CHECK(assert_invalid_persisted("brightness_daya", &mutated, 0U, 0U, 0U,
                                   NULL, 1U));
    mutated = valid_config();
    mutated.brightness_night_percent = 81U;
    CHECK(assert_invalid_persisted("bright_nighta", &mutated, 81U, 0U, 0U,
                                   NULL, 1U));
    mutated = valid_config();
    mutated.night_end_minute = mutated.night_start_minute;
    CHECK(assert_invalid_persisted("night_enda", &mutated, 0U,
                                   mutated.night_end_minute, 0U, NULL, 2U));
    mutated = valid_config();
    strcpy(mutated.tz, "UTC");
    CHECK(assert_invalid_persisted("tza", &mutated, 0U, 0U, 0U,
                                   mutated.tz, 4U));
    mutated = valid_config();
    mutated.freshness.fresh_max_s = 0U;
    CHECK(assert_invalid_persisted("thresh_fresh_sa", &mutated, 0U, 0U, 0U,
                                   NULL, 3U));
    mutated = valid_config();
    mutated.freshness.aging_max_s = 901U;
    CHECK(assert_invalid_persisted("thresh_aging_sa", &mutated, 0U, 0U, 901U,
                                   NULL, 3U));
    mutated = valid_config();
    mutated.freshness.stale_max_s = 900U;
    CHECK(assert_invalid_persisted("thresh_stale_sa", &mutated, 0U, 0U, 900U,
                                   NULL, 3U));
    mutated = valid_config();
    mutated.poll_s = 19U;
    CHECK(assert_invalid_persisted("poll_sa", &mutated, 0U, 0U, 19U,
                                   NULL, 3U));
    mutated = valid_config();
    mutated.ota_enabled = 2U;
    CHECK(assert_invalid_persisted("ota_enableda", &mutated, 2U, 0U, 0U,
                                   NULL, 1U));
    return true;
}

static bool test_generation_boundary(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t next = second_config();
    runtime_config_t loaded;
    uint8_t active;
    size_t trace_before;

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u32("generationa", UINT32_MAX);
    task7_host_seed_u32("checksuma", expected_checksum(&config, UINT32_MAX));
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    task7_host_clear_trace();
    CHECK(config_store_save(&next) == CONFIG_STORE_RESULT_INVALID);
    trace_before = task7_host_trace_count();
    CHECK(!trace_has_kind(TASK7_TRACE_ERASE_KEY));
    CHECK(!trace_has_kind(TASK7_TRACE_SET_STR));
    CHECK(!trace_has_kind(TASK7_TRACE_SET_U8));
    CHECK(!trace_has_kind(TASK7_TRACE_SET_U16));
    CHECK(!trace_has_kind(TASK7_TRACE_SET_U32));
    CHECK(trace_before > 0U);
    CHECK(task7_host_read_u8("active", &active) && active == 0U);
    task7_host_seed_u32("generationa", 0U);
    CHECK(load_invalid_then_recover(&config));
    return true;
}

static bool test_init_recovery_and_namespace_erase(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t loaded;

    task7_host_reset();
    task7_host_set_init_results(ESP_ERR_NVS_NO_FREE_PAGES, ESP_OK);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_global_erase_count() == 1U);
    CHECK(task7_host_trace_count() == 3U);
    CHECK(task7_host_trace_at(0U)->kind == TASK7_TRACE_FLASH_INIT);
    CHECK(task7_host_trace_at(1U)->kind == TASK7_TRACE_FLASH_ERASE);
    CHECK(task7_host_trace_at(2U)->kind == TASK7_TRACE_FLASH_INIT);
    task7_host_reset();
    task7_host_set_init_results(ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_OK);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_global_erase_count() == 1U);
    task7_host_reset();
    task7_host_set_init_results(ESP_FAIL, ESP_OK);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(task7_host_global_erase_count() == 0U);
    task7_host_reset();
    task7_host_set_init_results(ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_FAIL);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(task7_host_global_erase_count() == 1U);
    CHECK(task7_host_trace_count() == 3U);
    task7_host_reset();
    task7_host_set_flash_erase_result(ESP_FAIL);
    task7_host_set_init_results(ESP_ERR_NVS_NO_FREE_PAGES, ESP_OK);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(task7_host_global_erase_count() == 1U);

    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_erase_namespace() == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_namespace_erase_count() == 1U);
    CHECK(task7_host_global_erase_count() == 0U);
    CHECK(task7_host_trace_count() == 4U);
    CHECK(task7_host_trace_at(0U)->kind == TASK7_TRACE_OPEN);
    CHECK(task7_host_trace_at(1U)->kind == TASK7_TRACE_ERASE_ALL);
    CHECK(task7_host_trace_at(2U)->kind == TASK7_TRACE_COMMIT);
    CHECK(task7_host_trace_at(3U)->kind == TASK7_TRACE_CLOSE);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    task7_host_fail_at(2U, ESP_FAIL);
    CHECK(config_store_erase_namespace() == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_clear_failure();
    CHECK(task7_host_global_erase_count() == 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    task7_host_clear_trace();
    task7_host_fail_at(3U, ESP_FAIL);
    CHECK(config_store_erase_namespace() == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_clear_failure();
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    return true;
}

static bool test_namespace_erase_is_per_handle_and_eagerly_consumed(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t loaded;
    nvs_handle_t h1;
    nvs_handle_t h2;

    /* A: close and power-cycle discard h1's uncommitted namespace erase. */
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h1) == ESP_OK);
    CHECK(nvs_erase_all(h1) == ESP_OK);
    nvs_close(h1);
    task7_host_power_cycle();
    CHECK(task7_host_namespace_erase_count() == 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h2) == ESP_OK);
    CHECK(nvs_commit(h2) == ESP_OK);
    nvs_close(h2);
    CHECK(task7_host_namespace_erase_count() == 0U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));

    /* B: a simultaneous unrelated commit cannot consume h1's marker. */
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h1) == ESP_OK);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h2) == ESP_OK);
    CHECK(nvs_erase_all(h1) == ESP_OK);
    CHECK(nvs_commit(h2) == ESP_OK);
    CHECK(task7_host_namespace_erase_count() == 0U);
    nvs_close(h1);
    nvs_close(h2);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));

    /* C: AFTER-commit persists and consumes the erase exactly once. */
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h1) == ESP_OK);
    CHECK(nvs_erase_all(h1) == ESP_OK);
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_commit(h1) == ESP_FAIL);
    task7_host_clear_failure();
    CHECK(task7_host_namespace_erase_count() == 1U);
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h2) == ESP_OK);
    CHECK(nvs_commit(h2) == ESP_OK);
    nvs_close(h2);
    CHECK(task7_host_namespace_erase_count() == 1U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);

    /* D: AFTER-erase_all persists and consumes the erase exactly once. */
    task7_host_reset();
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h1) == ESP_OK);
    task7_host_clear_trace();
    task7_host_fail_at_after(1U, ESP_FAIL);
    CHECK(nvs_erase_all(h1) == ESP_FAIL);
    task7_host_clear_failure();
    CHECK(task7_host_namespace_erase_count() == 1U);
    nvs_close(h1);
    task7_host_power_cycle();
    CHECK(nvs_open("libre_desk", NVS_READWRITE, &h2) == ESP_OK);
    CHECK(nvs_commit(h2) == ESP_OK);
    nvs_close(h2);
    CHECK(task7_host_namespace_erase_count() == 1U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    return true;
}

static bool test_setup_request_marker_contract(void)
{
    bool requested = true;
    uint8_t value = 0U;

    task7_host_reset_uninitialized();
    CHECK(config_store_setup_request_get(NULL) == CONFIG_STORE_RESULT_BAD_ARGUMENT);
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(!requested);
    CHECK(config_store_setup_request_set() == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_IO_ERROR);

    task7_host_reset();
    requested = true;
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(!requested);
    CHECK(config_store_setup_request_set() == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_read_setup_u8("setup_req", &value));
    CHECK(value == UINT8_C(0xa5));
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(requested);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_OK);
    CHECK(!task7_host_read_setup_u8("setup_req", &value));
    requested = true;
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(!requested);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_OK);

    task7_host_seed_setup_u16("setup_req", UINT16_C(0xa5));
    requested = true;
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_INVALID);
    CHECK(!requested);
    task7_host_seed_setup_u8("setup_req", UINT8_C(0x5a));
    requested = true;
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_INVALID);
    CHECK(!requested);
    CHECK(config_store_setup_request_set() == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(requested);

    task7_host_clear_trace();
    task7_host_fail_next(ESP_FAIL);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_clear_failure();
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(requested);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_global_erase_count() == 0U);
    CHECK(assert_production_trace_locked());
    return true;
}

static bool test_factory_reset_request_interruption_safety(void)
{
    runtime_config_t old_config = valid_config();
    runtime_config_t loaded;
    bool requested;
    size_t operation_count;
    size_t operation;
    unsigned after;

    task7_host_reset();
    CHECK(config_store_save(&old_config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_factory_reset_request() == CONFIG_STORE_RESULT_OK);
    operation_count = task7_host_operation_count();
    CHECK(operation_count == 6U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    requested = false;
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(requested);
    CHECK(task7_host_global_erase_count() == 0U);

    for (operation = 1U; operation <= operation_count; ++operation) {
        for (after = 0U; after < 2U; ++after) {
            config_store_result_t load_result;
            task7_host_reset();
            CHECK(config_store_save(&old_config) == CONFIG_STORE_RESULT_OK);
            task7_host_clear_trace();
            if (after == 0U) task7_host_fail_at(operation, ESP_FAIL);
            else task7_host_fail_at_after(operation, ESP_FAIL);
            (void)config_store_factory_reset_request();
            task7_host_clear_failure();
            task7_host_power_cycle();

            requested = false;
            CHECK(config_store_setup_request_get(&requested) ==
                  CONFIG_STORE_RESULT_OK);
            load_result = config_store_load(&loaded);
            CHECK(load_result == CONFIG_STORE_RESULT_ABSENT ||
                  load_result == CONFIG_STORE_RESULT_OK);
            if (load_result == CONFIG_STORE_RESULT_OK) {
                CHECK(config_equal(&loaded, &old_config));
            } else {
                CHECK(requested);
            }
            CHECK(task7_host_global_erase_count() == 0U);
        }
    }
    return true;
}

static bool test_reset_monitor_and_hold_boundaries(void)
{
    static const int below_levels[] = {0, 0};
    static const int64_t below_times[] = {0LL, 4999999LL};
    static const int exact_setup_levels[] = {0, 0};
    static const int64_t exact_setup_times[] = {0LL, 5000000LL};
    static const int bounce_levels[] = {0, 1, 0, 1};
    static const int64_t bounce_times[] = {0LL, 1000000LL, 2000000LL, 6999999LL};
    static const int regression_levels[] = {0, 0, 1};
    static const int64_t regression_times[] = {3000000LL, 2000000LL, 8000000LL};
    static const int invalid_levels[] = {0, -1, 0, 1};
    static const int64_t invalid_times[] = {0LL, 1000000LL, 2000000LL, 6000000LL};
    static const int negative_time_levels[] = {0, 0, 1};
    static const int64_t negative_time_times[] = {0LL, -1LL, 6000000LL};
    bool requested = false;

    task7_host_reset();
    task7_host_set_gpio_config_result(ESP_FAIL);
    CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_IO_ERROR);
    CHECK(task7_host_critical_depth() == 0U);
    task7_host_set_gpio_config_result(ESP_OK);
    task7_host_set_task_create_fail(true);
    CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_IO_ERROR);
    CHECK(task7_host_critical_depth() == 0U);
    task7_host_set_task_create_fail(false);
    CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_OK);
    CHECK(task7_host_critical_depth() == 0U);
    CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_ALREADY_STARTED);
    CHECK(task7_host_gpio_mask() == (UINT64_C(1) << 9U));
    CHECK(task7_host_gpio_mode() == GPIO_MODE_INPUT);
    CHECK(task7_host_gpio_pull_up() == GPIO_PULLUP_ENABLE);
    CHECK(task7_host_gpio_pull_down() == GPIO_PULLDOWN_DISABLE);
    CHECK(task7_host_gpio_interrupt() == GPIO_INTR_DISABLE);
    CHECK(task7_host_created_task_priority() == tskIDLE_PRIORITY + 1U);
    CHECK(task7_host_created_task_stack_depth() == 1024U);

    task7_host_set_gpio_samples(below_levels, below_times,
                                sizeof(below_levels) / sizeof(below_levels[0]));
    CHECK(task7_host_run_created_task() == 2);
    CHECK(task7_host_restart_count() == 0U);
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(!requested);

    task7_host_set_gpio_samples(exact_setup_levels, exact_setup_times,
                                sizeof(exact_setup_levels) / sizeof(exact_setup_levels[0]));
    CHECK(task7_host_run_created_task() == 1);
    CHECK(task7_host_restart_count() == 1U);
    CHECK(task7_host_namespace_erase_count() == 0U);
    CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
    CHECK(requested);
    CHECK(config_store_setup_request_clear() == CONFIG_STORE_RESULT_OK);


    task7_host_set_gpio_samples(bounce_levels, bounce_times,
                                sizeof(bounce_levels) / sizeof(bounce_levels[0]));
    CHECK(task7_host_run_created_task() == 2);
    task7_host_set_gpio_samples(regression_levels, regression_times,
                                sizeof(regression_levels) / sizeof(regression_levels[0]));
    CHECK(task7_host_run_created_task() == 2);
    task7_host_set_gpio_samples(invalid_levels, invalid_times,
                                sizeof(invalid_levels) / sizeof(invalid_levels[0]));
    CHECK(task7_host_run_created_task() == 2);
    task7_host_set_gpio_samples(negative_time_levels, negative_time_times,
                                sizeof(negative_time_levels) / sizeof(negative_time_levels[0]));
    CHECK(task7_host_run_created_task() == 2);
    CHECK(task7_host_restart_count() == 1U);

    task7_host_set_gpio_samples(exact_setup_levels, exact_setup_times,
                                sizeof(exact_setup_levels) / sizeof(exact_setup_levels[0]));
    task7_host_fail_next(ESP_FAIL);
    CHECK(task7_host_run_created_task() == 2);
    CHECK(task7_host_restart_count() == 1U);
    task7_host_clear_failure();
    task7_host_set_gpio_samples(exact_setup_levels, exact_setup_times,
                                sizeof(exact_setup_levels) / sizeof(exact_setup_levels[0]));
    CHECK(task7_host_run_created_task() == 1);
    CHECK(task7_host_restart_count() == 2U);
    CHECK(task7_host_critical_depth() == 0U);
    return true;
}

static bool test_amended_contract_regressions(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t replacement = second_config();
    runtime_config_t loaded;

    task7_host_reset_uninitialized();
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(output_is_zero(&loaded));
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(config_store_erase_namespace() == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_OK);
    task7_host_reset();
    CHECK(config_store_init() == CONFIG_STORE_RESULT_OK);
    memset(&loaded, 0xa5, sizeof(loaded));
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    CHECK(output_is_zero(&loaded));

    task7_host_reset();
    CHECK(config_store_init() == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_OK);
    task7_host_seed_u8("active", 0U);
    task7_host_seed_u8("valida", 0U);
    task7_host_clear_trace();
    CHECK(config_store_save(&replacement) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &replacement));
    CHECK(task7_host_read_u8("active", &((uint8_t){0})));
    return true;
}

static bool is_nvs_event(task7_host_trace_kind_t kind)
{
    return kind <= TASK7_TRACE_ERASE_ALL;
}

static bool assert_production_trace_locked(void)
{
    size_t index;
    for (index = 0U; index < task7_host_trace_count(); index++) {
        const task7_host_trace_event_t *event = task7_host_trace_at(index);
        if (is_nvs_event(event->kind)) {
            CHECK(event->mutex_depth == 1U);
            CHECK(event->critical_depth == 0U);
        }
        if (event->kind == TASK7_TRACE_GPIO_CONFIG ||
            event->kind == TASK7_TRACE_TASK_CREATE) {
            CHECK(event->critical_depth == 0U);
        }
    }
    return true;
}

typedef struct behavior_thread_arg {
    const runtime_config_t *config;
    config_store_result_t result;
} behavior_thread_arg_t;

static void *save_thread(void *opaque)
{
    behavior_thread_arg_t *arg = (behavior_thread_arg_t *)opaque;
    arg->result = config_store_save(arg->config);
    return NULL;
}

static bool wait_for_contention(void)
{
    struct timespec pause = {0L, 1000000L};
    size_t index;
    for (index = 0U; index < 1000U; index++) {
        if (task7_host_mutex_contention() > 0U) return true;
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

static bool test_real_pthread_save_interleaving(void)
{
    runtime_config_t first = valid_config();
    runtime_config_t second = second_config();
    runtime_config_t loaded;
    behavior_thread_arg_t a = {&first, CONFIG_STORE_RESULT_IO_ERROR};
    behavior_thread_arg_t b = {&second, CONFIG_STORE_RESULT_IO_ERROR};
    pthread_t first_thread;
    pthread_t second_thread;

    task7_host_reset();
    task7_host_pause_after_operation(1U);
    CHECK(pthread_create(&first_thread, NULL, save_thread, &a) == 0);
    CHECK(task7_host_wait_until_paused());
    CHECK(pthread_create(&second_thread, NULL, save_thread, &b) == 0);
    CHECK(wait_for_contention());
    task7_host_release_pause();
    CHECK(pthread_join(first_thread, NULL) == 0);
    CHECK(pthread_join(second_thread, NULL) == 0);
    CHECK(a.result == CONFIG_STORE_RESULT_OK);
    CHECK(b.result == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_max_transaction_owners() == 1U);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &second));
    CHECK(assert_production_trace_locked());
    return true;
}

static void *load_thread(void *opaque)
{
    behavior_thread_arg_t *arg = (behavior_thread_arg_t *)opaque;
    runtime_config_t loaded;
    arg->result = config_store_load(&loaded);
    if (arg->result == CONFIG_STORE_RESULT_OK && !config_equal(&loaded, arg->config))
        arg->result = CONFIG_STORE_RESULT_INVALID;
    return NULL;
}

static void *erase_thread(void *opaque)
{
    behavior_thread_arg_t *arg = (behavior_thread_arg_t *)opaque;
    arg->result = config_store_erase_namespace();
    return NULL;
}

static bool test_real_pthread_save_load_and_erase_interleavings(void)
{
    runtime_config_t old_config = valid_config();
    runtime_config_t new_config = second_config();
    behavior_thread_arg_t save = {&new_config, CONFIG_STORE_RESULT_IO_ERROR};
    behavior_thread_arg_t load = {&new_config, CONFIG_STORE_RESULT_IO_ERROR};
    behavior_thread_arg_t erase = {&new_config, CONFIG_STORE_RESULT_IO_ERROR};
    pthread_t owner, contender;
    runtime_config_t loaded;

    task7_host_reset();
    CHECK(config_store_save(&old_config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    task7_host_pause_after_operation(1U);
    CHECK(pthread_create(&owner, NULL, save_thread, &save) == 0);
    CHECK(task7_host_wait_until_paused());
    CHECK(pthread_create(&contender, NULL, load_thread, &load) == 0);
    CHECK(wait_for_contention());
    task7_host_release_pause();
    CHECK(pthread_join(owner, NULL) == 0);
    CHECK(pthread_join(contender, NULL) == 0);
    CHECK(save.result == CONFIG_STORE_RESULT_OK);
    CHECK(load.result == CONFIG_STORE_RESULT_OK);
    CHECK(task7_host_max_transaction_owners() == 1U);
    CHECK(assert_production_trace_locked());

    task7_host_reset();
    CHECK(config_store_save(&old_config) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    task7_host_pause_after_operation(1U);
    CHECK(pthread_create(&owner, NULL, save_thread, &save) == 0);
    CHECK(task7_host_wait_until_paused());
    CHECK(pthread_create(&contender, NULL, erase_thread, &erase) == 0);
    CHECK(wait_for_contention());
    task7_host_release_pause();
    CHECK(pthread_join(owner, NULL) == 0);
    CHECK(pthread_join(contender, NULL) == 0);
    CHECK(save.result == CONFIG_STORE_RESULT_OK);
    CHECK(erase.result == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
    CHECK(task7_host_max_transaction_owners() == 1U);
    CHECK(assert_production_trace_locked());
    return true;
}

typedef struct reset_start_arg {
    provisioning_result_t result;
} reset_start_arg_t;

static void *reset_start_thread(void *opaque)
{
    reset_start_arg_t *arg = (reset_start_arg_t *)opaque;
    arg->result = provisioning_reset_monitor_start();
    return NULL;
}

static bool test_reset_start_race_and_failure_recovery(void)
{
    static const int levels[] = {0, 0, 1, 0, 0};
    static const int64_t times[] = {
        0LL, 5000000LL, 6000000LL, 7000000LL, 12000000LL
    };
    reset_start_arg_t first;
    pthread_t thread;
    unsigned failure;

    task7_host_reset();
    task7_host_pause_after_operation(1U);
    first.result = PROVISIONING_RESULT_IO_ERROR;
    CHECK(pthread_create(&thread, NULL, reset_start_thread, &first) == 0);
    CHECK(task7_host_wait_until_paused());
    CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_ALREADY_STARTED);
    task7_host_release_pause();
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(first.result == PROVISIONING_RESULT_OK);
    CHECK(assert_production_trace_locked());

    for (failure = 0U; failure < 4U; failure++) {
        bool requested = false;
        task7_host_reset();
        CHECK(provisioning_reset_monitor_start() == PROVISIONING_RESULT_OK);
        task7_host_set_gpio_samples(levels, times, sizeof(levels) / sizeof(levels[0]));
        task7_host_clear_trace();
        if (failure == 0U) task7_host_set_mutex_take_fail_once();
        else task7_host_fail_at(failure, ESP_FAIL);
        CHECK(task7_host_run_created_task() == 1);
        task7_host_clear_failure();
        CHECK(task7_host_restart_count() == 1U);
        CHECK(task7_host_namespace_erase_count() == 0U);
        CHECK(config_store_setup_request_get(&requested) == CONFIG_STORE_RESULT_OK);
        CHECK(requested);
        CHECK(assert_production_trace_locked());
    }
    return true;
}

static bool test_mutex_failure_matrix(void)
{
    runtime_config_t config = valid_config();
    runtime_config_t loaded;

    task7_host_reset_uninitialized();
    task7_host_set_mutex_failures(true, false, false);
    CHECK(config_store_init() == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_reset();
    task7_host_set_mutex_failures(false, true, false);
    memset(&loaded, 0xa5, sizeof(loaded));
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_IO_ERROR);
    CHECK(output_is_zero(&loaded));
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_set_mutex_failures(false, false, true);
    CHECK(config_store_save(&config) == CONFIG_STORE_RESULT_IO_ERROR);
    task7_host_clear_failure();
    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
    CHECK(config_equal(&loaded, &config));
    return true;
}

static bool trace_faultable(task7_host_trace_kind_t kind)
{
    return kind <= TASK7_TRACE_COMMIT && kind != TASK7_TRACE_CLOSE;
}

static size_t trace_operation_number(size_t trace_index)
{
    size_t index;
    size_t number = 0U;
    for (index = 0U; index <= trace_index; index++) {
        if (trace_faultable(task7_host_trace_at(index)->kind)) number++;
    }
    return number;
}

static bool locate_publication(size_t *active_set, size_t *active_commit)
{
    size_t index;
    *active_set = 0U;
    *active_commit = 0U;
    for (index = 0U; index < task7_host_trace_count(); index++) {
        const task7_host_trace_event_t *event = task7_host_trace_at(index);
        if (event->kind == TASK7_TRACE_SET_U8 && strcmp(event->key, "active") == 0) {
            *active_set = trace_operation_number(index);
            if (index + 1U < task7_host_trace_count() &&
                task7_host_trace_at(index + 1U)->kind == TASK7_TRACE_COMMIT) {
                *active_commit = trace_operation_number(index + 1U);
            }
            break;
        }
    }
    CHECK(*active_set > 0U && *active_commit == *active_set + 1U);
    return true;
}

static bool assert_save_matrix_case(const runtime_config_t *old_config,
                                    const runtime_config_t *new_config,
                                    size_t operation, bool after,
                                    size_t active_set, size_t active_commit,
                                    bool initially_absent)
{
    runtime_config_t loaded;
    config_store_result_t expected = CONFIG_STORE_RESULT_IO_ERROR;
    bool new_is_durable = after ? operation >= active_set : operation > active_commit;
    task7_host_fail_at_after(operation, ESP_FAIL);
    if (!after) task7_host_fail_at(operation, ESP_FAIL);
    {
        config_store_result_t result = config_store_save(new_config);
        expected = CONFIG_STORE_RESULT_IO_ERROR;
        if (result != expected) {
            fprintf(stderr, "matrix save result=%d op=%zu after=%d active_set=%zu active_commit=%zu actual=%zu\\n",
                    (int)result, operation, after, active_set, active_commit,
                    task7_host_operation_count());
        }
        CHECK(result == expected);
    }
    task7_host_power_cycle();
    task7_host_clear_failure();
    if (initially_absent && !new_is_durable) {
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_ABSENT);
        CHECK(output_is_zero(&loaded));
    } else {
        CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
        CHECK(config_equal(&loaded, new_is_durable ? new_config : old_config));
    }
    return true;
}

static bool run_save_phase_matrix(const runtime_config_t *old_config,
                                  const runtime_config_t *new_config,
                                  bool initially_absent, bool seed_prior,
                                  unsigned *operations_out,
                                  unsigned *cases_out)
{
    size_t operations;
    size_t active_set;
    size_t active_commit;
    size_t operation;
    unsigned cases = 0U;
    unsigned phase;
    runtime_config_t prior = valid_config();

    task7_host_reset();
    if (!initially_absent) {
        if (seed_prior) CHECK(config_store_save(&prior) == CONFIG_STORE_RESULT_OK);
        CHECK(config_store_save(old_config) == CONFIG_STORE_RESULT_OK);
    }
    task7_host_clear_trace();
    CHECK(config_store_save(new_config) == CONFIG_STORE_RESULT_OK);
    operations = task7_host_operation_count();
    CHECK(operations > 0U);
    CHECK(locate_publication(&active_set, &active_commit));
    for (phase = 0U; phase < 2U; phase++) {
        for (operation = 1U; operation <= operations; operation++) {
            task7_host_reset();
            if (!initially_absent) {
                if (seed_prior) CHECK(config_store_save(&prior) == CONFIG_STORE_RESULT_OK);
                CHECK(config_store_save(old_config) == CONFIG_STORE_RESULT_OK);
            }
            task7_host_clear_trace();
            CHECK(assert_save_matrix_case(old_config, new_config, operation, phase != 0U,
                                          active_set, active_commit, initially_absent));
            cases++;
        }
    }
    *operations_out = (unsigned)operations;
    *cases_out = cases;
    return true;
}

typedef enum invalid_representative {
    INVALID_ACTIVE_TYPE, INVALID_ACTIVE_RANGE, INVALID_MISSING_FIELD,
    INVALID_FIELD_TYPE, INVALID_OVERSIZED_STRING, INVALID_SCHEMA,
    INVALID_CHECKSUM, INVALID_VALID, INVALID_RUNTIME_VALUE
} invalid_representative_t;

static bool seed_invalid_representative(invalid_representative_t representative,
                                         const runtime_config_t *base)
{
    task7_host_reset();
    CHECK(config_store_save(base) == CONFIG_STORE_RESULT_OK);
    switch (representative) {
    case INVALID_ACTIVE_TYPE: task7_host_seed_u16("active", 0U); break;
    case INVALID_ACTIVE_RANGE: task7_host_seed_u8("active", 2U); break;
    case INVALID_MISSING_FIELD: task7_host_remove("wifi_ssida"); break;
    case INVALID_FIELD_TYPE: task7_host_seed_u32("wifi_ssida", 4U); break;
    case INVALID_OVERSIZED_STRING: task7_host_seed_oversized_str("wifi_ssida", 319U); break;
    case INVALID_SCHEMA: task7_host_seed_u32("schemaa", 2U); break;
    case INVALID_CHECKSUM: task7_host_seed_u32("checksuma", 0U); break;
    case INVALID_VALID: task7_host_seed_u8("valida", 0U); break;
    case INVALID_RUNTIME_VALUE:
        task7_host_seed_u8("brightness_daya", 0U);
        task7_host_seed_u32("checksuma", expected_checksum(base, 1U));
        break;
    }
    if (representative == INVALID_FIELD_TYPE || representative == INVALID_OVERSIZED_STRING) {
        return true;
    }
    if (representative == INVALID_RUNTIME_VALUE) return true;
    return true;
}

static bool test_invalid_recovery_before_after_matrix(void)
{
    static const invalid_representative_t representatives[] = {
        INVALID_ACTIVE_TYPE, INVALID_ACTIVE_RANGE, INVALID_MISSING_FIELD,
        INVALID_FIELD_TYPE, INVALID_OVERSIZED_STRING, INVALID_SCHEMA,
        INVALID_CHECKSUM, INVALID_VALID, INVALID_RUNTIME_VALUE
    };
    runtime_config_t base = valid_config();
    runtime_config_t replacement = third_config();
    runtime_config_t loaded;
    size_t active_set;
    size_t active_commit;
    size_t operations;
    size_t operation;
    size_t index;
    unsigned cases = 0U;

    for (index = 0U; index < sizeof(representatives) / sizeof(representatives[0]); index++) {
        CHECK(seed_invalid_representative(representatives[index], &base));
        CHECK(load_is_invalid_and_clear());
        task7_host_clear_trace();
        CHECK(config_store_save(&replacement) == CONFIG_STORE_RESULT_OK);
        operations = task7_host_operation_count();
        CHECK(operations > 0U);
        CHECK(locate_publication(&active_set, &active_commit));
        report_invalid_recovery_representatives++;
        for (operation = 1U; operation <= operations; operation++) {
            unsigned phase;
            for (phase = 0U; phase < 2U; phase++) {
                config_store_result_t recovery_result;
                CHECK(seed_invalid_representative(representatives[index], &base));
                CHECK(load_is_invalid_and_clear());
                task7_host_clear_trace();
                if (phase == 0U) task7_host_fail_at(operation, ESP_FAIL);
                else task7_host_fail_at_after(operation, ESP_FAIL);
                recovery_result = config_store_save(&replacement);
                CHECK(recovery_result == CONFIG_STORE_RESULT_IO_ERROR);
                task7_host_power_cycle();
                task7_host_clear_failure();
                if ((phase == 0U && operation > active_commit) ||
                    (phase != 0U && operation >= active_set)) {
                    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_OK);
                    CHECK(config_equal(&loaded, &replacement));
                } else {
                    CHECK(config_store_load(&loaded) == CONFIG_STORE_RESULT_INVALID);
                    CHECK(output_is_zero(&loaded));
                }
                cases++;
            }
        }
    }
    report_invalid_recovery_cases = cases;
    CHECK(report_invalid_recovery_representatives == 9U);
    CHECK(report_invalid_recovery_cases > 0U);
    return true;
}

static bool test_before_after_power_cycle_matrix(void)
{
    runtime_config_t first = valid_config();
    runtime_config_t second = second_config();
    runtime_config_t third = third_config();

    CHECK(run_save_phase_matrix(NULL, &first, true, false,
                                &report_first_save_operations, &report_first_save_cases));
    CHECK(run_save_phase_matrix(&first, &second, false, false,
                                &report_ab_operations, &report_ab_cases));
    task7_host_reset();
    CHECK(config_store_save(&first) == CONFIG_STORE_RESULT_OK);
    CHECK(config_store_save(&second) == CONFIG_STORE_RESULT_OK);
    task7_host_clear_trace();
    CHECK(config_store_save(&third) == CONFIG_STORE_RESULT_OK);
    report_ba_operations = (unsigned)task7_host_operation_count();
    CHECK(report_ba_operations > 0U);
    CHECK(run_save_phase_matrix(&second, &third, false, true,
                                &report_ba_operations, &report_ba_cases));
    report_save_before_after = report_first_save_cases + report_ab_cases + report_ba_cases;
    return true;
}

static bool run_all_tests(void)
{
    CHECK(test_authentic_handle_transactions_and_selector_recovery());
    CHECK(test_amended_contract_regressions());
    CHECK(test_crc_vector_and_round_trip());
    CHECK(test_fake_fill_then_error_reads());
    CHECK(test_production_fill_then_error_is_transactional());
    CHECK(test_validation_before_io_and_output_clear());
    CHECK(test_exact_keys_types_and_publication_order());
    CHECK(test_slot_rotation_and_no_fallback());
    CHECK(test_corruption_and_bounded_reads());
    CHECK(test_interruption_preserves_previous_configuration());
    CHECK(test_first_save_interruptions_remain_absent());
    CHECK(test_load_failure_matrix());
    CHECK(test_semantic_validation_on_persisted_values());
    CHECK(test_generation_boundary());
    CHECK(test_init_recovery_and_namespace_erase());
    CHECK(test_namespace_erase_is_per_handle_and_eagerly_consumed());
    CHECK(test_setup_request_marker_contract());
    CHECK(test_factory_reset_request_interruption_safety());
    CHECK(test_reset_monitor_and_hold_boundaries());
    CHECK(test_mutex_failure_matrix());
    CHECK(test_before_after_power_cycle_matrix());
    CHECK(test_invalid_recovery_before_after_matrix());
    CHECK(test_real_pthread_save_interleaving());
    CHECK(test_real_pthread_save_load_and_erase_interleavings());
    CHECK(test_reset_start_race_and_failure_recovery());
    return true;
}

int main(int argc, char **argv)
{
    bool report = false;
    if (argc > 1 && strcmp(argv[1], "--report") == 0) {
        report = true;
    } else if (argc > 1 && strcmp(argv[1], "--silence") != 0) {
        fprintf(stderr, "unknown argument\n");
        return 2;
    }
    if (!run_all_tests()) return 1;
    if (report) {
        printf("save first-save operations=%u cases=%u\n",
               report_first_save_operations, report_first_save_cases);
        printf("save A->B operations=%u cases=%u\n",
               report_ab_operations, report_ab_cases);
        printf("save B->A operations=%u cases=%u\n",
               report_ba_operations, report_ba_cases);
        printf("save before/after cases=%u\n", report_save_before_after);
        printf("load before/after cases=%u\n", report_load_before_after);
        printf("invalid recovery representatives=%u cases=%u\n",
               report_invalid_recovery_representatives, report_invalid_recovery_cases);
    }
    return 0;
}
