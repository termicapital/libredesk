#include "config_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CONFIG_STORE_NAMESPACE "libre_desk"
#define CONFIG_STORE_SETUP_NAMESPACE "libre_setup"
#define CONFIG_STORE_SCHEMA UINT32_C(1)
#define CONFIG_STORE_VALID_MAGIC UINT8_C(0xa5)
#define CONFIG_STORE_KEY_CAPACITY 16U

static const char *const CONFIG_STORE_FIELD_WIFI_SSID = "wifi_ssid";
static const char *const CONFIG_STORE_FIELD_WIFI_PASSWORD = "wifi_pass";
static const char *const CONFIG_STORE_FIELD_BRIDGE_ORIGIN = "bridge_url";
static const char *const CONFIG_STORE_FIELD_DEVICE_TOKEN = "device_token";
static const char *const CONFIG_STORE_FIELD_UNIT = "unit";
static const char *const CONFIG_STORE_FIELD_BRIGHTNESS_DAY = "brightness_day";
static const char *const CONFIG_STORE_FIELD_BRIGHTNESS_NIGHT = "bright_night";
static const char *const CONFIG_STORE_FIELD_NIGHT_START = "night_start";
static const char *const CONFIG_STORE_FIELD_NIGHT_END = "night_end";
static const char *const CONFIG_STORE_FIELD_TZ = "tz";
static const char *const CONFIG_STORE_FIELD_THRESH_FRESH = "thresh_fresh_s";
static const char *const CONFIG_STORE_FIELD_THRESH_AGING = "thresh_aging_s";
static const char *const CONFIG_STORE_FIELD_THRESH_STALE = "thresh_stale_s";
static const char *const CONFIG_STORE_FIELD_POLL = "poll_s";
static const char *const CONFIG_STORE_FIELD_OTA = "ota_enabled";

static const char *const CONFIG_STORE_META_SCHEMA = "schema";
static const char *const CONFIG_STORE_META_GENERATION = "generation";
static const char *const CONFIG_STORE_META_CHECKSUM = "checksum";
static const char *const CONFIG_STORE_META_VALID = "valid";
static const char *const CONFIG_STORE_ACTIVE = "active";
static const char *const CONFIG_STORE_SETUP_REQUEST = "setup_req";
#define CONFIG_STORE_SETUP_REQUEST_MAGIC UINT8_C(0xa5)

typedef struct config_store_slot_data {
    runtime_config_t config;
    uint32_t generation;
} config_store_slot_data_t;

static StaticSemaphore_t s_transaction_mutex_storage;
static SemaphoreHandle_t s_transaction_mutex;
static bool s_store_initialized;

static bool store_lock(void)
{
    return s_transaction_mutex != NULL &&
           xSemaphoreTake(s_transaction_mutex, portMAX_DELAY) == pdTRUE;
}

static bool store_unlock(void)
{
    return s_transaction_mutex != NULL &&
           xSemaphoreGive(s_transaction_mutex) == pdTRUE;
}

#ifdef TASK7_HOST_TEST
/* Host-only reset hook; not part of the public API. */
void config_store_test_reset(void)
{
    s_transaction_mutex = NULL;
    s_store_initialized = false;
    memset(&s_transaction_mutex_storage, 0, sizeof(s_transaction_mutex_storage));
}
#endif
static void secure_zero(void *address, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)address;

    while (size > 0U) {
        *bytes = 0U;
        bytes++;
        size--;
    }
}

static void slot_key(char *key, size_t capacity, const char *stem, uint8_t slot)
{
    size_t length = strlen(stem);

    if (capacity == 0U || length + 2U > capacity) {
        if (capacity > 0U) {
            key[0] = '\0';
        }
        return;
    }
    memcpy(key, stem, length);
    key[length] = slot == 0U ? 'a' : 'b';
    key[length + 1U] = '\0';
}

static config_store_result_t map_nvs_read_error(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND ||
        error == ESP_ERR_NVS_TYPE_MISMATCH ||
        error == ESP_ERR_NVS_INVALID_LENGTH ||
        error == ESP_ERR_NVS_VALUE_TOO_LONG) {
        return CONFIG_STORE_RESULT_INVALID;
    }
    return CONFIG_STORE_RESULT_IO_ERROR;
}

static config_store_result_t map_nvs_write_error(esp_err_t error)
{
    (void)error;
    return CONFIG_STORE_RESULT_IO_ERROR;
}

static config_store_result_t read_u8(nvs_handle_t handle,
                                     const char *key,
                                     uint8_t *value)
{
    esp_err_t error = nvs_get_u8(handle, key, value);
    return error == ESP_OK ? CONFIG_STORE_RESULT_OK : map_nvs_read_error(error);
}

static config_store_result_t read_u16(nvs_handle_t handle,
                                      const char *key,
                                      uint16_t *value)
{
    esp_err_t error = nvs_get_u16(handle, key, value);
    return error == ESP_OK ? CONFIG_STORE_RESULT_OK : map_nvs_read_error(error);
}

static config_store_result_t read_u32(nvs_handle_t handle,
                                      const char *key,
                                      uint32_t *value)
{
    esp_err_t error = nvs_get_u32(handle, key, value);
    return error == ESP_OK ? CONFIG_STORE_RESULT_OK : map_nvs_read_error(error);
}

static config_store_result_t read_string(nvs_handle_t handle,
                                         const char *key,
                                         char *value,
                                         size_t capacity)
{
    size_t required = 0U;
    esp_err_t error;

    error = nvs_get_str(handle, key, NULL, &required);
    if (error != ESP_OK) {
        return map_nvs_read_error(error);
    }
    if (required == 0U || required > capacity) {
        return CONFIG_STORE_RESULT_INVALID;
    }
    error = nvs_get_str(handle, key, value, &required);
    if (error != ESP_OK) {
        return map_nvs_read_error(error);
    }
    if (required == 0U || required > capacity || value[required - 1U] != '\0') {
        return CONFIG_STORE_RESULT_INVALID;
    }
    return CONFIG_STORE_RESULT_OK;
}

static uint32_t crc_update(uint32_t crc, uint8_t byte)
{
    unsigned bit;

    crc ^= (uint32_t)byte;
    for (bit = 0U; bit < 8U; bit++) {
        crc = (crc & UINT32_C(1)) != 0U
                  ? (crc >> 1U) ^ UINT32_C(0xedb88320)
                  : crc >> 1U;
    }
    return crc;
}

static uint32_t crc_u8(uint32_t crc, uint8_t value)
{
    return crc_update(crc, value);
}

static uint32_t crc_u16(uint32_t crc, uint16_t value)
{
    crc = crc_update(crc, (uint8_t)value);
    return crc_update(crc, (uint8_t)(value >> 8U));
}

static uint32_t crc_u32(uint32_t crc, uint32_t value)
{
    unsigned index;

    for (index = 0U; index < 4U; index++) {
        crc = crc_update(crc, (uint8_t)(value >> (index * 8U)));
    }
    return crc;
}

static uint32_t crc_string(uint32_t crc, const char *value)
{
    size_t length = strlen(value);
    size_t index;

    crc = crc_u32(crc, (uint32_t)length);
    for (index = 0U; index < length; index++) {
        crc = crc_update(crc, (uint8_t)value[index]);
    }
    return crc;
}

static uint32_t config_checksum(const runtime_config_t *config,
                                uint32_t generation)
{
    uint32_t crc = UINT32_C(0xffffffff);

    crc = crc_u32(crc, CONFIG_STORE_SCHEMA);
    crc = crc_u32(crc, generation);
    crc = crc_string(crc, config->wifi_ssid);
    crc = crc_string(crc, config->wifi_password);
    crc = crc_string(crc, config->bridge_origin);
    crc = crc_string(crc, config->device_token);
    crc = crc_u8(crc, (uint8_t)config->unit);
    crc = crc_u8(crc, config->brightness_day_percent);
    crc = crc_u8(crc, config->brightness_night_percent);
    crc = crc_u16(crc, config->night_start_minute);
    crc = crc_u16(crc, config->night_end_minute);
    crc = crc_string(crc, config->tz);
    crc = crc_u32(crc, config->freshness.fresh_max_s);
    crc = crc_u32(crc, config->freshness.aging_max_s);
    crc = crc_u32(crc, config->freshness.stale_max_s);
    crc = crc_u32(crc, config->poll_s);
    crc = crc_u8(crc, config->ota_enabled);
    return crc ^ UINT32_C(0xffffffff);
}

static config_store_result_t read_slot(nvs_handle_t handle,
                                       uint8_t slot,
                                       config_store_slot_data_t *slot_out)
{
    char key[CONFIG_STORE_KEY_CAPACITY];
    uint8_t valid = 0U;
    uint8_t unit = 0U;
    uint8_t brightness_day = 0U;
    uint8_t brightness_night = 0U;
    uint8_t ota_enabled = 0U;
    uint16_t night_start = 0U;
    uint16_t night_end = 0U;
    uint32_t schema = 0U;
    uint32_t generation = 0U;
    uint32_t checksum = 0U;
    config_store_result_t result;

    memset(slot_out, 0, sizeof(*slot_out));

    slot_key(key, sizeof(key), CONFIG_STORE_META_VALID, slot);
    result = read_u8(handle, key, &valid);
    if (result != CONFIG_STORE_RESULT_OK || valid != CONFIG_STORE_VALID_MAGIC) {
        return result == CONFIG_STORE_RESULT_OK ? CONFIG_STORE_RESULT_INVALID : result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_META_SCHEMA, slot);
    result = read_u32(handle, key, &schema);
    if (result != CONFIG_STORE_RESULT_OK || schema != CONFIG_STORE_SCHEMA) {
        return result == CONFIG_STORE_RESULT_OK ? CONFIG_STORE_RESULT_INVALID : result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_META_GENERATION, slot);
    result = read_u32(handle, key, &generation);
    if (result != CONFIG_STORE_RESULT_OK || generation == 0U) {
        return result == CONFIG_STORE_RESULT_OK ? CONFIG_STORE_RESULT_INVALID : result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_WIFI_SSID, slot);
    result = read_string(handle, key, slot_out->config.wifi_ssid,
                         sizeof(slot_out->config.wifi_ssid));
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_WIFI_PASSWORD, slot);
    result = read_string(handle, key, slot_out->config.wifi_password,
                         sizeof(slot_out->config.wifi_password));
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIDGE_ORIGIN, slot);
    result = read_string(handle, key, slot_out->config.bridge_origin,
                         sizeof(slot_out->config.bridge_origin));
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_DEVICE_TOKEN, slot);
    result = read_string(handle, key, slot_out->config.device_token,
                         sizeof(slot_out->config.device_token));
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_UNIT, slot);
    result = read_u8(handle, key, &unit);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.unit = (glucose_unit_t)unit;
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIGHTNESS_DAY, slot);
    result = read_u8(handle, key, &brightness_day);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.brightness_day_percent = brightness_day;
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIGHTNESS_NIGHT, slot);
    result = read_u8(handle, key, &brightness_night);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.brightness_night_percent = brightness_night;
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_NIGHT_START, slot);
    result = read_u16(handle, key, &night_start);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.night_start_minute = night_start;
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_NIGHT_END, slot);
    result = read_u16(handle, key, &night_end);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.night_end_minute = night_end;
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_TZ, slot);
    result = read_string(handle, key, slot_out->config.tz,
                         sizeof(slot_out->config.tz));
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_FRESH, slot);
    result = read_u32(handle, key, &slot_out->config.freshness.fresh_max_s);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_AGING, slot);
    result = read_u32(handle, key, &slot_out->config.freshness.aging_max_s);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_STALE, slot);
    result = read_u32(handle, key, &slot_out->config.freshness.stale_max_s);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_POLL, slot);
    result = read_u32(handle, key, &slot_out->config.poll_s);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_OTA, slot);
    result = read_u8(handle, key, &ota_enabled);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }
    slot_out->config.ota_enabled = ota_enabled;
    slot_key(key, sizeof(key), CONFIG_STORE_META_CHECKSUM, slot);
    result = read_u32(handle, key, &checksum);
    if (result != CONFIG_STORE_RESULT_OK) {
        return result;
    }

    if (checksum != config_checksum(&slot_out->config, generation) ||
        !runtime_config_validate(&slot_out->config, NULL)) {
        return CONFIG_STORE_RESULT_INVALID;
    }
    slot_out->generation = generation;
    return CONFIG_STORE_RESULT_OK;
}

static esp_err_t write_slot_payload(nvs_handle_t handle,
                                    uint8_t slot,
                                    const runtime_config_t *config,
                                    uint32_t generation)
{
    char key[CONFIG_STORE_KEY_CAPACITY];
    esp_err_t error;
    uint32_t checksum = config_checksum(config, generation);

    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_WIFI_SSID, slot);
    error = nvs_set_str(handle, key, config->wifi_ssid);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_WIFI_PASSWORD, slot);
    error = nvs_set_str(handle, key, config->wifi_password);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIDGE_ORIGIN, slot);
    error = nvs_set_str(handle, key, config->bridge_origin);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_DEVICE_TOKEN, slot);
    error = nvs_set_str(handle, key, config->device_token);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_UNIT, slot);
    error = nvs_set_u8(handle, key, (uint8_t)config->unit);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIGHTNESS_DAY, slot);
    error = nvs_set_u8(handle, key, config->brightness_day_percent);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_BRIGHTNESS_NIGHT, slot);
    error = nvs_set_u8(handle, key, config->brightness_night_percent);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_NIGHT_START, slot);
    error = nvs_set_u16(handle, key, config->night_start_minute);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_NIGHT_END, slot);
    error = nvs_set_u16(handle, key, config->night_end_minute);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_TZ, slot);
    error = nvs_set_str(handle, key, config->tz);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_FRESH, slot);
    error = nvs_set_u32(handle, key, config->freshness.fresh_max_s);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_AGING, slot);
    error = nvs_set_u32(handle, key, config->freshness.aging_max_s);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_THRESH_STALE, slot);
    error = nvs_set_u32(handle, key, config->freshness.stale_max_s);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_POLL, slot);
    error = nvs_set_u32(handle, key, config->poll_s);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_FIELD_OTA, slot);
    error = nvs_set_u8(handle, key, config->ota_enabled);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_META_SCHEMA, slot);
    error = nvs_set_u32(handle, key, CONFIG_STORE_SCHEMA);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_META_GENERATION, slot);
    error = nvs_set_u32(handle, key, generation);
    if (error != ESP_OK) {
        return error;
    }
    slot_key(key, sizeof(key), CONFIG_STORE_META_CHECKSUM, slot);
    return nvs_set_u32(handle, key, checksum);
}

static esp_err_t write_slot_valid(nvs_handle_t handle, uint8_t slot)
{
    char key[CONFIG_STORE_KEY_CAPACITY];

    slot_key(key, sizeof(key), CONFIG_STORE_META_VALID, slot);
    return nvs_set_u8(handle, key, CONFIG_STORE_VALID_MAGIC);
}

static esp_err_t write_active(nvs_handle_t handle, uint8_t slot)
{
    return nvs_set_u8(handle, CONFIG_STORE_ACTIVE, slot);
}

config_store_result_t config_store_init(void)
{
    esp_err_t error;

    if (s_transaction_mutex == NULL) {
        s_transaction_mutex = xSemaphoreCreateMutexStatic(&s_transaction_mutex_storage);
        if (s_transaction_mutex == NULL) {
            return CONFIG_STORE_RESULT_IO_ERROR;
        }
    }
    if (!store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    if (error == ESP_OK) {
        s_store_initialized = true;
    }
    if (!store_unlock()) {
        s_store_initialized = false;
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    return error == ESP_OK ? CONFIG_STORE_RESULT_OK : CONFIG_STORE_RESULT_IO_ERROR;
}

config_store_result_t config_store_load(runtime_config_t *config_out)
{
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    uint8_t active = 0U;
    config_store_slot_data_t candidate;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    esp_err_t error;

    if (config_out == NULL) {
        return CONFIG_STORE_RESULT_BAD_ARGUMENT;
    }
    memset(config_out, 0, sizeof(*config_out));
    memset(&candidate, 0, sizeof(candidate));
    if (!s_store_initialized || !store_lock()) {
        secure_zero(&candidate, sizeof(candidate));
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;
    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result = CONFIG_STORE_RESULT_ABSENT;
        goto cleanup;
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_get_u8(handle, CONFIG_STORE_ACTIVE, &active);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result = CONFIG_STORE_RESULT_ABSENT;
        goto cleanup;
    }
    if (error != ESP_OK) {
        result = map_nvs_read_error(error);
        goto cleanup;
    }
    if (active > 1U) {
        result = CONFIG_STORE_RESULT_INVALID;
        goto cleanup;
    }
    result = read_slot(handle, active, &candidate);
cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    } else if (result == CONFIG_STORE_RESULT_OK) {
        *config_out = candidate.config;
    }
    secure_zero(&candidate, sizeof(candidate));
    return result;
}

config_store_result_t config_store_save(const runtime_config_t *config)
{
    runtime_config_t staged;
    config_store_slot_data_t current;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    uint8_t active = 0U;
    uint8_t target = 0U;
    uint32_t generation = 1U;
    esp_err_t error;

    if (config == NULL) {
        return CONFIG_STORE_RESULT_BAD_ARGUMENT;
    }
    memcpy(&staged, config, sizeof(staged));
    if (!runtime_config_validate(&staged, NULL)) {
        secure_zero(&staged, sizeof(staged));
        return CONFIG_STORE_RESULT_INVALID;
    }
    memset(&current, 0, sizeof(current));
    if (!s_store_initialized || !store_lock()) {
        secure_zero(&staged, sizeof(staged));
        secure_zero(&current, sizeof(current));
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;

    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_get_u8(handle, CONFIG_STORE_ACTIVE, &active);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        target = 0U;
        generation = 1U;
    } else if (error == ESP_ERR_NVS_TYPE_MISMATCH) {
        /* A malformed selector cannot identify a trustworthy old slot. */
        target = 0U;
        generation = 1U;
    } else if (error != ESP_OK) {
        result = map_nvs_read_error(error);
        goto cleanup;
    } else {
        if (active > 1U) {
            target = 0U;
            generation = 1U;
        } else {
            result = read_slot(handle, active, &current);
            if (result == CONFIG_STORE_RESULT_OK) {
                if (current.generation == UINT32_MAX) {
                    result = CONFIG_STORE_RESULT_INVALID;
                    goto cleanup;
                }
                target = active == 0U ? 1U : 0U;
                generation = current.generation + 1U;
            } else if (result == CONFIG_STORE_RESULT_INVALID) {
                target = active == 0U ? 1U : 0U;
                generation = 1U;
            } else {
                goto cleanup;
            }
        }
    }

    {
        char key[CONFIG_STORE_KEY_CAPACITY];
        slot_key(key, sizeof(key), CONFIG_STORE_META_VALID, target);
        error = nvs_erase_key(handle, key);
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = write_slot_payload(handle, target, &staged, generation);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = write_slot_valid(handle, target);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = write_active(handle, target);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        result = map_nvs_write_error(error);
        goto cleanup;
    }
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    }
    secure_zero(&current, sizeof(current));
    secure_zero(&staged, sizeof(staged));
    return result;
}

config_store_result_t config_store_setup_request_set(void)
{
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    esp_err_t error;

    if (!s_store_initialized || !store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;
    error = nvs_open(CONFIG_STORE_SETUP_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_set_u8(handle, CONFIG_STORE_SETUP_REQUEST,
                       CONFIG_STORE_SETUP_REQUEST_MAGIC);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    }
    return result;
}

config_store_result_t config_store_setup_request_get(bool *requested_out)
{
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    bool requested = false;
    uint8_t marker = 0U;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    esp_err_t error;

    if (requested_out == NULL) {
        return CONFIG_STORE_RESULT_BAD_ARGUMENT;
    }
    *requested_out = false;
    if (!s_store_initialized || !store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;
    error = nvs_open(CONFIG_STORE_SETUP_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result = CONFIG_STORE_RESULT_OK;
        goto cleanup;
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_get_u8(handle, CONFIG_STORE_SETUP_REQUEST, &marker);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result = CONFIG_STORE_RESULT_OK;
        goto cleanup;
    }
    if (error != ESP_OK) {
        result = map_nvs_read_error(error);
        goto cleanup;
    }
    if (marker != CONFIG_STORE_SETUP_REQUEST_MAGIC) {
        result = CONFIG_STORE_RESULT_INVALID;
        goto cleanup;
    }
    requested = true;
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    } else if (result == CONFIG_STORE_RESULT_OK) {
        *requested_out = requested;
    }
    return result;
}

config_store_result_t config_store_setup_request_clear(void)
{
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    esp_err_t error;

    if (!s_store_initialized || !store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;
    error = nvs_open(CONFIG_STORE_SETUP_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_erase_key(handle, CONFIG_STORE_SETUP_REQUEST);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result = CONFIG_STORE_RESULT_OK;
        goto cleanup;
    }
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    }
    return result;
}

config_store_result_t config_store_factory_reset_request(void)
{
    nvs_handle_t setup_handle = 0U;
    nvs_handle_t config_handle = 0U;
    bool setup_opened = false;
    bool config_opened = false;
    bool locked = false;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;
    esp_err_t error;

    if (!s_store_initialized || !store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;

    error = nvs_open(CONFIG_STORE_SETUP_NAMESPACE, NVS_READWRITE,
                     &setup_handle);
    if (error != ESP_OK) goto cleanup;
    setup_opened = true;
    error = nvs_set_u8(setup_handle, CONFIG_STORE_SETUP_REQUEST,
                       CONFIG_STORE_SETUP_REQUEST_MAGIC);
    if (error != ESP_OK) goto cleanup;
    error = nvs_commit(setup_handle);
    if (error != ESP_OK) goto cleanup;
    nvs_close(setup_handle);
    setup_opened = false;

    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &config_handle);
    if (error != ESP_OK) goto cleanup;
    config_opened = true;
    error = nvs_erase_all(config_handle);
    if (error != ESP_OK) goto cleanup;
    error = nvs_commit(config_handle);
    if (error != ESP_OK) goto cleanup;
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (config_opened) nvs_close(config_handle);
    if (setup_opened) nvs_close(setup_handle);
    if (locked && !store_unlock()) result = CONFIG_STORE_RESULT_IO_ERROR;
    return result;
}

config_store_result_t config_store_erase_namespace(void)
{
    nvs_handle_t handle = 0U;
    bool opened = false;
    bool locked = false;
    esp_err_t error;
    config_store_result_t result = CONFIG_STORE_RESULT_IO_ERROR;

    if (!s_store_initialized || !store_lock()) {
        return CONFIG_STORE_RESULT_IO_ERROR;
    }
    locked = true;
    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    opened = true;
    error = nvs_erase_all(handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = nvs_commit(handle);
    if (error != ESP_OK) {
        goto cleanup;
    }
    result = CONFIG_STORE_RESULT_OK;

cleanup:
    if (opened) {
        nvs_close(handle);
    }
    if (locked && !store_unlock()) {
        result = CONFIG_STORE_RESULT_IO_ERROR;
    }
    return result;
}
