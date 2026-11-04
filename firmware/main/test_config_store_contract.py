#!/usr/bin/env python3
"""Exhaustive source-copy oracle for the frozen Task 7 contract.

Every mutant below is applied to a fresh complete source tree.  The oracle is
passed paths into that tree; it never reads production files while validating a
mutant.
"""
from __future__ import annotations

import re
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware" / "main"


class ContractViolation(Exception):
    pass


class ContractText(str):
    def find_required(self, token: str, start: int = 0) -> int:
        position = self.find(token, start)
        if position < 0:
            raise ContractViolation(f"missing token during validation: {token}")
        return position


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractViolation(message)


def body(source: str, signature: str) -> str:
    start = source.find_required(signature)
    opening = source.find_required("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return ContractText(source[opening + 1:index])
    raise ContractViolation(f"unterminated function: {signature}")


def order(source: str, *tokens: str) -> None:
    positions = [source.find(token) for token in tokens]
    require(all(position >= 0 for position in positions), "missing ordering token")
    require(positions == sorted(positions) and len(set(positions)) == len(positions),
            "required ordering changed")


def read_inputs(paths: dict[str, Path]) -> dict[str, str]:
    return {name: ContractText(path.read_text(encoding="utf-8")) for name, path in paths.items()}


def validate_sources(paths: dict[str, Path]) -> None:
    source = read_inputs(paths)
    config, provisioning, board, cmake = (source[name] for name in
                                           ("config_store.c", "provisioning.c", "board.h", "CMakeLists.txt"))
    header = source["config_store.h"]
    pheader = source["provisioning.h"]

    # Public API order is frozen, not merely membership-tested.
    require(re.search(r"typedef enum config_store_result \{\s*"
                      r"CONFIG_STORE_RESULT_OK = 0,\s*"
                      r"CONFIG_STORE_RESULT_ABSENT,\s*"
                      r"CONFIG_STORE_RESULT_INVALID,\s*"
                      r"CONFIG_STORE_RESULT_IO_ERROR,\s*"
                      r"CONFIG_STORE_RESULT_BAD_ARGUMENT,\s*"
                      r"CONFIG_STORE_RESULT_COUNT", header, re.S), "config enum/order changed")
    require(re.search(r"typedef enum provisioning_result \{\s*"
                      r"PROVISIONING_RESULT_OK = 0,\s*"
                      r"PROVISIONING_RESULT_ALREADY_STARTED,\s*"
                      r"PROVISIONING_RESULT_IO_ERROR,\s*"
                      r"PROVISIONING_RESULT_COUNT", pheader, re.S), "provisioning enum/order changed")
    require(bool(re.search(r"config_store_result_t config_store_init\(void\);", header))
            and bool(re.search(r"config_store_result_t config_store_load\(runtime_config_t \*config_out\);", header))
            and bool(re.search(r"config_store_result_t config_store_save\(const runtime_config_t \*config\);", header))
            and bool(re.search(r"config_store_result_t config_store_erase_namespace\(void\);", header)),
            "public API changed")
    require(bool(re.search(r"provisioning_result_t provisioning_reset_monitor_start\(void\);", pheader)),
            "provisioning public API changed")

    require('#define CONFIG_STORE_NAMESPACE "libre_desk"' in config, "namespace changed")
    stems = ["wifi_ssid", "wifi_pass", "bridge_url", "device_token", "unit",
             "brightness_day", "bright_night", "night_start", "night_end", "tz",
             "thresh_fresh_s", "thresh_aging_s", "thresh_stale_s", "poll_s", "ota_enabled"]
    metadata = ["schema", "generation", "checksum", "valid"]
    for stem in stems + metadata + ["active"]:
        require(f'"{stem}"' in config, f"missing persisted key stem: {stem}")
    require(len(stems) == 15 and len(metadata) == 4, "schema field count changed")
    require("key[length] = slot == 0U ? 'a' : 'b';" in config, "slot suffix changed")
    require("CONFIG_STORE_KEY_CAPACITY 16U" in config, "key capacity changed")
    require(all(len(stem) + (0 if stem == "active" else 1) <= 15 for stem in stems + metadata + ["active"]),
            "physical NVS key exceeds 15 characters")

    payload = body(config, "static esp_err_t write_slot_payload")
    mappings = [
        ("nvs_set_str(handle, key, config->wifi_ssid)", "read_string(handle, key, slot_out->config.wifi_ssid"),
        ("nvs_set_str(handle, key, config->wifi_password)", "read_string(handle, key, slot_out->config.wifi_password"),
        ("nvs_set_str(handle, key, config->bridge_origin)", "read_string(handle, key, slot_out->config.bridge_origin"),
        ("nvs_set_str(handle, key, config->device_token)", "read_string(handle, key, slot_out->config.device_token"),
        ("nvs_set_u8(handle, key, (uint8_t)config->unit)", "read_u8(handle, key, &unit)"),
        ("nvs_set_u8(handle, key, config->brightness_day_percent)", "read_u8(handle, key, &brightness_day)"),
        ("nvs_set_u8(handle, key, config->brightness_night_percent)", "read_u8(handle, key, &brightness_night)"),
        ("nvs_set_u16(handle, key, config->night_start_minute)", "read_u16(handle, key, &night_start)"),
        ("nvs_set_u16(handle, key, config->night_end_minute)", "read_u16(handle, key, &night_end)"),
        ("nvs_set_str(handle, key, config->tz)", "read_string(handle, key, slot_out->config.tz"),
        ("nvs_set_u32(handle, key, config->freshness.fresh_max_s)", "read_u32(handle, key, &slot_out->config.freshness.fresh_max_s)"),
        ("nvs_set_u32(handle, key, config->freshness.aging_max_s)", "read_u32(handle, key, &slot_out->config.freshness.aging_max_s)"),
        ("nvs_set_u32(handle, key, config->freshness.stale_max_s)", "read_u32(handle, key, &slot_out->config.freshness.stale_max_s)"),
        ("nvs_set_u32(handle, key, config->poll_s)", "read_u32(handle, key, &slot_out->config.poll_s)"),
        ("nvs_set_u8(handle, key, config->ota_enabled)", "read_u8(handle, key, &ota_enabled)"),
        ("nvs_set_u32(handle, key, CONFIG_STORE_SCHEMA)", "read_u32(handle, key, &schema)"),
        ("nvs_set_u32(handle, key, generation)", "read_u32(handle, key, &generation)"),
        ("nvs_set_u32(handle, key, checksum)", "read_u32(handle, key, &checksum)"),
        ("nvs_set_u8(handle, CONFIG_STORE_ACTIVE, slot)", "nvs_get_u8(handle, CONFIG_STORE_ACTIVE, &active)"),
    ]
    for write, read in mappings:
        require(write in payload or write in body(config, "static esp_err_t write_active"), f"write type mapping changed: {write}")
        require(read in config, f"read type mapping changed: {read}")
    require("nvs_set_u8(handle, key, CONFIG_STORE_VALID_MAGIC)" in config, "valid type mapping changed")
    require("read_u8(handle, key, &valid)" in config, "valid read mapping changed")
    require("nvs_set_u8(handle, CONFIG_STORE_ACTIVE, slot)" in config, "active write mapping changed")

    require("CONFIG_STORE_SCHEMA UINT32_C(1)" in config and "CONFIG_STORE_VALID_MAGIC UINT8_C(0xa5)" in config,
            "schema or valid magic changed")
    require("UINT32_C(0xedb88320)" in config and "uint32_t crc = UINT32_C(0xffffffff);" in config
            and "return crc ^ UINT32_C(0xffffffff);" in config, "CRC constants changed")
    checksum = body(config, "static uint32_t config_checksum")
    crc_string_body = body(config, "static uint32_t crc_string")
    require("crc = crc_u32(crc, (uint32_t)length);" in crc_string_body,
            "CRC string length is not canonical u32 little-endian")
    order(checksum, "crc_u32(crc, CONFIG_STORE_SCHEMA)", "crc_u32(crc, generation)",
          "crc_string(crc, config->wifi_ssid)", "crc_string(crc, config->wifi_password)",
          "crc_string(crc, config->bridge_origin)", "crc_string(crc, config->device_token)",
          "crc_u8(crc, (uint8_t)config->unit)", "crc_u8(crc, config->brightness_day_percent)",
          "crc_u8(crc, config->brightness_night_percent)", "crc_u16(crc, config->night_start_minute)",
          "crc_u16(crc, config->night_end_minute)", "crc_string(crc, config->tz)",
          "crc_u32(crc, config->freshness.fresh_max_s)", "crc_u32(crc, config->freshness.aging_max_s)",
          "crc_u32(crc, config->freshness.stale_max_s)", "crc_u32(crc, config->poll_s)",
          "crc_u8(crc, config->ota_enabled)")
    require("value[required - 1U] != '\\0'" in config and
            "nvs_get_str(handle, key, NULL, &required)" in config, "bounded string read changed")

    save = body(config, "config_store_result_t config_store_save")
    require("runtime_config_validate(&staged, NULL)" in save and "store_lock()" in save and
            save.find_required("runtime_config_validate(&staged, NULL)") < save.find_required("store_lock()")
            and save.find_required("runtime_config_validate(&staged, NULL)") < save.find_required("nvs_open(CONFIG_STORE_NAMESPACE"),
            "save validation no longer precedes lock/I/O")
    require("static StaticSemaphore_t s_transaction_mutex_storage;" in config and
            "static SemaphoreHandle_t s_transaction_mutex;" in config and
            "s_transaction_mutex = xSemaphoreCreateMutexStatic(&s_transaction_mutex_storage);" in config and
            "xSemaphoreCreateMutex(" not in config and
            "store_lock()" in save and "store_unlock()" in save and "locked = true" in save and save.find_required("store_lock()") < save.find_required("nvs_open")
            and save.find_required("nvs_close(handle)") < save.find_required("store_unlock()"), "save mutex scope changed")
    require(save.count("nvs_commit(handle)") == 4, "save commit count changed")
    commits = [match.start() for match in re.finditer(r"nvs_commit\(handle\)", save)]
    require(commits[0] > save.find_required("nvs_erase_key") and commits[0] < save.find_required("write_slot_payload")
            and commits[1] > save.find_required("write_slot_payload") and commits[1] < save.find_required("write_slot_valid")
            and commits[2] > save.find_required("write_slot_valid") and commits[2] < save.find_required("write_active")
            and commits[3] > save.find_required("write_active"), "save transaction ordering changed")
    require("target = 0U;" in save and "generation = 1U;" in save and "current.generation == UINT32_MAX" in save,
            "fresh/recovery/generation contract changed")
    require("ESP_ERR_NVS_NOT_FOUND" in save and "if (error == ESP_ERR_NVS_NOT_FOUND)" in save and
            "ESP_ERR_NVS_TYPE_MISMATCH" in save and
            "} else if (result == CONFIG_STORE_RESULT_INVALID) {\n                target = active == 0U ? 1U : 0U;" in save,
            "active or invalid-slot recovery changed")

    init = body(config, "config_store_result_t config_store_init")
    require(init.find_required("store_lock()") < init.find_required("nvs_flash_init()") < init.find_required("store_unlock()"),
            "init mutex scope changed")
    load = body(config, "config_store_result_t config_store_load")
    require("memset(config_out, 0, sizeof(*config_out));" in load and
            load.find_required("memset(config_out, 0, sizeof(*config_out));") < load.find_required("if (!s_store_initialized")
            and load.find_required("store_lock()") < load.find_required("nvs_open"),
            "load output clear/validation boundary changed")
    fresh_open = load[load.find_required("error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &handle);"):load.find_required("opened = true;")]
    require("if (error == ESP_ERR_NVS_NOT_FOUND) {\n        result = CONFIG_STORE_RESULT_ABSENT;\n        goto cleanup;\n    }" in fresh_open, "load fresh-store absence handling changed")
    require("if (active > 1U)" in load and "CONFIG_STORE_RESULT_ABSENT" in load
            and "read_slot(handle, active, &candidate)" in load
            and load.count("read_slot(handle,") == 1
            and "active == 0U ? 1U : 0U, &candidate" not in load, "selected slot/fresh mapping changed")
    require(load.find_required("nvs_close(handle)") < load.find_required("store_unlock()") < load.find_required("*config_out = candidate.config;"),
            "load published before close and successful unlock")
    require(load.count("secure_zero(&candidate, sizeof(candidate));") == 2, "candidate wipe changed")

    init = body(config, "config_store_result_t config_store_init")
    require(init.count("nvs_flash_erase()") == 1 and
            bool(re.search(r"if \(error == ESP_ERR_NVS_NO_FREE_PAGES \|\|\s*"
                           r"error == ESP_ERR_NVS_NEW_VERSION_FOUND\)", init)),
            "init global erase recovery widened")
    reset = body(config, "config_store_result_t config_store_erase_namespace")
    require("store_lock()" in reset and reset.find_required("store_lock()") < reset.find_required("nvs_open")
            and "nvs_erase_all(handle)" in reset and "nvs_commit(handle)" in reset
            and reset.find_required("nvs_close(handle)") < reset.find_required("store_unlock()")
            and "nvs_flash_erase" not in reset,
            "factory reset is not namespace-only")
    require(config.count("secure_zero(&staged, sizeof(staged));") == 3 and
            config.count("secure_zero(&current, sizeof(current));") == 2 and
            config.count("secure_zero(&candidate, sizeof(candidate));") == 2 and
            "volatile unsigned char *bytes" in config, "secret wipe coverage changed")
    require(not any(token in config for token in ("malloc", "calloc", "realloc", "xTaskCreate(", "printf(", "ESP_LOG")),
            "dynamic allocation/task/logging added")
    require("xTaskCreate(" not in provisioning and "printf(" not in provisioning and "ESP_LOG" not in provisioning,
            "dynamic task/logging added")

    require("#define BOARD_BOOT_BUTTON_PIN        9" in board and
            "BOARD_BOOT_BUTTON_ACTIVE_LOW true" in board and "files.waveshare.com/wiki/ESP32-C6-LCD-1.3/ESP32C6-1.3.pdf" in board,
            "board pin/polarity citation changed")
    require(bool(re.search(r"#ifdef TASK7_HOST_TEST\s*/\* Host-only reset hook.*?\n#endif", config, re.S))
            and bool(re.search(r"#ifdef TASK7_HOST_TEST\s*void provisioning_test_reset\(void\).*?\n#endif", provisioning, re.S)),
            "host reset hook guard changed")
    require("#if !BOARD_BOOT_BUTTON_ACTIVE_LOW" in provisioning and "GPIO_MODE_INPUT" in provisioning
            and "GPIO_PULLUP_ENABLE" in provisioning and "GPIO_PULLDOWN_DISABLE" in provisioning
            and "GPIO_INTR_DISABLE" in provisioning, "reset GPIO contract changed")
    require("xTaskCreateStatic" in provisioning and "s_reset_monitor_stack[RESET_MONITOR_STACK_DEPTH]" in provisioning
            and "RESET_MONITOR_STACK_DEPTH 1024U" in provisioning and "RESET_MONITOR_SAMPLE_MS 20U" in provisioning
            and "RESET_MONITOR_HOLD_US INT64_C(5000000)" in provisioning, "reset timing/task contract changed")
    task = body(provisioning, "static void reset_monitor_task")
    require("if (config_store_erase_namespace() == CONFIG_STORE_RESULT_OK)" in task
            and "if (level != 0)" in task and "config_store_erase_namespace()" in task
            and task.find_required("config_store_erase_namespace()") < task.find_required("esp_restart()"), "runtime polarity/restart changed")
    start = body(provisioning, "provisioning_result_t provisioning_reset_monitor_start")
    exits_before_gpio = [match.start() for match in re.finditer(r"taskEXIT_CRITICAL\(&s_reset_monitor_lock\)", start)
                         if match.start() < start.find_required("error = gpio_config")]
    exits_after_task = [match.start() for match in re.finditer(r"taskEXIT_CRITICAL\(&s_reset_monitor_lock\)", start)
                        if match.start() > start.find_required("task_handle = xTaskCreateStatic")]
    require("RESET_MONITOR_STOPPED" in start and "RESET_MONITOR_STARTING" in start and "RESET_MONITOR_STARTED" in provisioning
            and len(exits_before_gpio) == 2 and bool(exits_after_task), "reset critical/external ordering changed")
    require("tskIDLE_PRIORITY + 1U" in start and "RESET_MONITOR_STACK_DEPTH," in start,
            "reset task priority/stack changed")
    require("taskENTER_CRITICAL" in provisioning and "taskEXIT_CRITICAL" in provisioning, "reset state guard removed")
    require('"config_store.c"' in cmake and '"provisioning.c"' in cmake and "nvs_flash" in cmake,
            "CMake source/dependency integration changed")


def copy_tree() -> tuple[tempfile.TemporaryDirectory[str], dict[str, Path]]:
    temporary = tempfile.TemporaryDirectory(prefix="libre-desk-task7-oracle-")
    root = Path(temporary.name)
    paths: dict[str, Path] = {}
    for name in ("config_store.c", "config_store.h", "provisioning.c", "provisioning.h", "board.h", "CMakeLists.txt"):
        paths[name] = root / name
        shutil.copy2(MAIN / name, paths[name])
    return temporary, paths


def mutate_and_reject(name: str, relative: str, old: str, new: str) -> None:
    temporary, paths = copy_tree()
    try:
        target = paths[relative]
        text = target.read_text(encoding="utf-8")
        require(text.count(old) == 1, f"{name}: mutation was not applied exactly once")
        target.write_text(text.replace(old, new), encoding="utf-8")
        try:
            validate_sources(paths)
        except ContractViolation:
            return
        raise ContractViolation(f"mutation accepted: {name}")
    finally:
        temporary.cleanup()


def mutations() -> list[tuple[str, str, str, str]]:
    result: list[tuple[str, str, str, str]] = []
    result.append(("namespace", "config_store.c", '#define CONFIG_STORE_NAMESPACE "libre_desk"', '#define CONFIG_STORE_NAMESPACE "wrong_namespace"'))
    keys = ["wifi_ssid", "wifi_pass", "bridge_url", "device_token", "unit", "brightness_day", "bright_night", "night_start", "night_end", "tz", "thresh_fresh_s", "thresh_aging_s", "thresh_stale_s", "poll_s", "ota_enabled", "schema", "generation", "checksum", "valid", "active"]
    for key in keys:
        constant_names = {
            "wifi_ssid": "CONFIG_STORE_FIELD_WIFI_SSID", "wifi_pass": "CONFIG_STORE_FIELD_WIFI_PASSWORD",
            "bridge_url": "CONFIG_STORE_FIELD_BRIDGE_ORIGIN", "device_token": "CONFIG_STORE_FIELD_DEVICE_TOKEN",
            "unit": "CONFIG_STORE_FIELD_UNIT", "brightness_day": "CONFIG_STORE_FIELD_BRIGHTNESS_DAY",
            "bright_night": "CONFIG_STORE_FIELD_BRIGHTNESS_NIGHT", "night_start": "CONFIG_STORE_FIELD_NIGHT_START",
            "night_end": "CONFIG_STORE_FIELD_NIGHT_END", "tz": "CONFIG_STORE_FIELD_TZ",
            "thresh_fresh_s": "CONFIG_STORE_FIELD_THRESH_FRESH", "thresh_aging_s": "CONFIG_STORE_FIELD_THRESH_AGING",
            "thresh_stale_s": "CONFIG_STORE_FIELD_THRESH_STALE", "poll_s": "CONFIG_STORE_FIELD_POLL",
            "ota_enabled": "CONFIG_STORE_FIELD_OTA", "schema": "CONFIG_STORE_META_SCHEMA",
            "generation": "CONFIG_STORE_META_GENERATION", "checksum": "CONFIG_STORE_META_CHECKSUM",
            "valid": "CONFIG_STORE_META_VALID", "active": "CONFIG_STORE_ACTIVE",
        }
        constant = constant_names[key]
        # Constants are named irregularly; target the first literal in its declaration.
        pattern = re.compile(rf'(static const char \*const {re.escape(constant)} = ")({re.escape(key)})(";)')
        text = (MAIN / "config_store.c").read_text(encoding="utf-8")
        match = pattern.search(text)
        require(match is not None, f"key declaration missing: {key}")
        result.append((f"key:{key}", "config_store.c", match.group(0), match.group(1) + "mutated_" + key + match.group(3)))
    # One exact type-site mutant per persisted logical/metadata/active mapping.
    type_sites = [
        ("wifi_ssid", "nvs_set_str(handle, key, config->wifi_ssid)", "nvs_set_u8(handle, key, 0U)"),
        ("wifi_pass", "nvs_set_str(handle, key, config->wifi_password)", "nvs_set_u8(handle, key, 0U)"),
        ("bridge_url", "nvs_set_str(handle, key, config->bridge_origin)", "nvs_set_u8(handle, key, 0U)"),
        ("device_token", "nvs_set_str(handle, key, config->device_token)", "nvs_set_u8(handle, key, 0U)"),
        ("unit", "nvs_set_u8(handle, key, (uint8_t)config->unit)", "nvs_set_u16(handle, key, config->unit)"),
        ("brightness_day", "nvs_set_u8(handle, key, config->brightness_day_percent)", "nvs_set_u16(handle, key, config->brightness_day_percent)"),
        ("brightness_night", "nvs_set_u8(handle, key, config->brightness_night_percent)", "nvs_set_u16(handle, key, config->brightness_night_percent)"),
        ("night_start", "nvs_set_u16(handle, key, config->night_start_minute)", "nvs_set_u32(handle, key, config->night_start_minute)"),
        ("night_end", "nvs_set_u16(handle, key, config->night_end_minute)", "nvs_set_u32(handle, key, config->night_end_minute)"),
        ("tz", "nvs_set_str(handle, key, config->tz)", "nvs_set_u8(handle, key, 0U)"),
        ("thresh_fresh_s", "nvs_set_u32(handle, key, config->freshness.fresh_max_s)", "nvs_set_u16(handle, key, config->freshness.fresh_max_s)"),
        ("thresh_aging_s", "nvs_set_u32(handle, key, config->freshness.aging_max_s)", "nvs_set_u16(handle, key, config->freshness.aging_max_s)"),
        ("thresh_stale_s", "nvs_set_u32(handle, key, config->freshness.stale_max_s)", "nvs_set_u16(handle, key, config->freshness.stale_max_s)"),
        ("poll_s", "nvs_set_u32(handle, key, config->poll_s)", "nvs_set_u16(handle, key, config->poll_s)"),
        ("ota_enabled", "nvs_set_u8(handle, key, config->ota_enabled)", "nvs_set_u32(handle, key, config->ota_enabled)"),
        ("schema", "nvs_set_u32(handle, key, CONFIG_STORE_SCHEMA)", "nvs_set_u16(handle, key, CONFIG_STORE_SCHEMA)"),
        ("generation", "nvs_set_u32(handle, key, generation)", "nvs_set_u16(handle, key, generation)"),
        ("checksum", "nvs_set_u32(handle, key, checksum)", "nvs_set_u16(handle, key, checksum)"),
        ("valid", "nvs_set_u8(handle, key, CONFIG_STORE_VALID_MAGIC)", "nvs_set_u16(handle, key, CONFIG_STORE_VALID_MAGIC)"),
        ("active", "nvs_set_u8(handle, CONFIG_STORE_ACTIVE, slot)", "nvs_set_u16(handle, CONFIG_STORE_ACTIVE, slot)"),
    ]
    for name, old, new in type_sites:
        result.append((f"type:{name}", "config_store.c", old, new))
    result += [
        ("schema-value", "config_store.c", "CONFIG_STORE_SCHEMA UINT32_C(1)", "CONFIG_STORE_SCHEMA UINT32_C(2)"),
        ("valid-magic", "config_store.c", "CONFIG_STORE_VALID_MAGIC UINT8_C(0xa5)", "CONFIG_STORE_VALID_MAGIC UINT8_C(0x5a)"),
        ("crc-polynomial", "config_store.c", "UINT32_C(0xedb88320)", "UINT32_C(0x04c11db7)"),
        ("crc-string-length-u16", "config_store.c", "crc = crc_u32(crc, (uint32_t)length);", "crc = crc_u16(crc, (uint16_t)length);"),
        ("crc-field-order-swap", "config_store.c", "crc_string(crc, config->wifi_ssid);\n    crc = crc_string(crc, config->wifi_password);", "crc = crc_string(crc, config->wifi_password);\n    crc = crc_string(crc, config->wifi_ssid);"),
        ("active-before-valid", "config_store.c", "    error = write_slot_valid(handle, target);\n    if (error != ESP_OK) {\n        result = map_nvs_write_error(error);\n        goto cleanup;\n    }\n    error = nvs_commit(handle);\n    if (error != ESP_OK) {\n        result = map_nvs_write_error(error);\n        goto cleanup;\n    }\n    error = write_active(handle, target);", "    error = write_active(handle, target);\n    if (error != ESP_OK) {\n        result = map_nvs_write_error(error);\n        goto cleanup;\n    }\n    error = nvs_commit(handle);\n    if (error != ESP_OK) {\n        result = map_nvs_write_error(error);\n        goto cleanup;\n    }\n    error = write_slot_valid(handle, target);"),
        ("removed-validation", "config_store.c", "if (!runtime_config_validate(&staged, NULL))", "if (false)"),
        ("removed-output-clear", "config_store.c", "memset(config_out, 0, sizeof(*config_out));", "/* output clear removed */"),
        ("load-publication-before-unlock", "config_store.c", "} else if (result == CONFIG_STORE_RESULT_OK) {\n        *config_out = candidate.config;\n    }", "} else if (result == CONFIG_STORE_RESULT_OK) {\n        *config_out = candidate.config;\n    }\n    if (locked) {"),
        ("removed-mutex-take", "config_store.c", "    memset(&current, 0, sizeof(current));\n    if (!s_store_initialized || !store_lock()) {", "    memset(&current, 0, sizeof(current));\n    if (!s_store_initialized || false) {"),
        ("removed-mutex-give", "config_store.c", "    if (locked && !store_unlock()) {\n        result = CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    secure_zero(&current, sizeof(current));", "    if (locked && false) {\n        result = CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    secure_zero(&current, sizeof(current));"),
        ("open-before-lock", "config_store.c", "locked = true;\n\n    error = nvs_open", "error = nvs_open"),
        ("fresh-not-found-recovery-removed", "config_store.c", "if (error == ESP_ERR_NVS_NOT_FOUND) {\n        target = 0U;\n        generation = 1U;\n    } else if", "if (false) {\n        target = 0U;\n        generation = 1U;\n    } else if"),
        ("wrong-type-active-recovery-removed", "config_store.c", "else if (error == ESP_ERR_NVS_TYPE_MISMATCH)", "else if (false)"),
        ("invalid-slot-stale-fallback", "config_store.c", "target = active == 0U ? 1U : 0U;\n                generation = 1U;\n            } else {", "target = active;\n                generation = 1U;\n            } else {"),
        ("generation-wrap-check-removed", "config_store.c", "if (current.generation == UINT32_MAX)", "if (false)"),
        ("global-reset-erase", "config_store.c", "error = nvs_erase_all(handle);", "error = nvs_flash_erase();"),
        ("wipe-staged-early", "config_store.c", "secure_zero(&staged, sizeof(staged));\n        return CONFIG_STORE_RESULT_INVALID;", "/* staged wipe removed */\n        return CONFIG_STORE_RESULT_INVALID;"),
        ("wipe-staged-cleanup", "config_store.c", "secure_zero(&current, sizeof(current));\n    secure_zero(&staged, sizeof(staged));", "secure_zero(&current, sizeof(current));\n    /* staged wipe removed */"),
        ("wipe-current", "config_store.c", "    secure_zero(&current, sizeof(current));\n    secure_zero(&staged, sizeof(staged));\n    return result;", "    /* current wipe removed */\n    secure_zero(&staged, sizeof(staged));\n    return result;"),
        ("wipe-candidate-early", "config_store.c", "secure_zero(&candidate, sizeof(candidate));\n        return CONFIG_STORE_RESULT_IO_ERROR;", "/* candidate early wipe removed */\n        return CONFIG_STORE_RESULT_IO_ERROR;"),
        ("wipe-candidate", "config_store.c", "secure_zero(&candidate, sizeof(candidate));\n    return result;", "/* candidate wipe removed */\n    return result;"),
        ("nonvolatile-wipe-pointer", "config_store.c", "volatile unsigned char *bytes", "unsigned char *bytes"),
        ("true-runtime-polarity", "provisioning.c", "if (level != 0) {", "if (level != 1) {"),
        ("reset-threshold", "provisioning.c", "INT64_C(5000000)", "INT64_C(4000000)"),
        ("gpio-inside-critical", "provisioning.c", "taskEXIT_CRITICAL(&s_reset_monitor_lock);\n\n    error = gpio_config", "/* GPIO moved into critical */\n\n    error = gpio_config"),
        ("restart-inverted", "provisioning.c", "if (config_store_erase_namespace() == CONFIG_STORE_RESULT_OK)", "if (config_store_erase_namespace() != CONFIG_STORE_RESULT_OK)"),
        ("reset-global-erase", "provisioning.c", "config_store_erase_namespace()", "nvs_flash_erase()"),
        ("introduce-malloc", "config_store.c", "#include <stddef.h>", "#include <stddef.h>\nstatic void *task7_mutant_malloc = (void *)malloc(1U);"),
        ("introduce-calloc", "config_store.c", "#include <stddef.h>", "#include <stddef.h>\nstatic void *task7_mutant_calloc = (void *)calloc(1U, 1U);"),
        ("introduce-xTaskCreate", "provisioning.c", "#include \"freertos/task.h\"", "#include \"freertos/task.h\"\nstatic void *task7_mutant_task = (void *)xTaskCreate(NULL, NULL, 0U, NULL, 0U, NULL);"),
        ("introduce-value-printf", "config_store.c", "#include <string.h>", "#include <string.h>\nstatic void task7_mutant_log(void) { printf(\"value=%u\\n\", 7U); }"),
        ("api-config-erase-removed", "config_store.h", "config_store_result_t config_store_erase_namespace(void);\n", ""),
        ("api-provisioning-start-removed", "provisioning.h", "provisioning_result_t provisioning_reset_monitor_start(void);\n", ""),
        ("config-reset-guard-removed", "config_store.c", "#ifdef TASK7_HOST_TEST\n/* Host-only reset hook; not part of the public API. */", "/* Host hook guard removed */\n/* Host-only reset hook; not part of the public API. */"),
        ("provisioning-reset-guard-removed", "provisioning.c", "#ifdef TASK7_HOST_TEST\nvoid provisioning_test_reset(void)", "/* Host hook guard removed */\nvoid provisioning_test_reset(void)"),
        ("dynamic-mutex", "config_store.c", "s_transaction_mutex = xSemaphoreCreateMutexStatic(&s_transaction_mutex_storage);", "s_transaction_mutex = xSemaphoreCreateMutex(&s_transaction_mutex_storage);"),
        ("load-lock-removed", "config_store.c", "if (!s_store_initialized || !store_lock()) {\n        secure_zero(&candidate, sizeof(candidate));", "if (!s_store_initialized || false) {\n        secure_zero(&candidate, sizeof(candidate));"),
        ("erase-lock-removed", "config_store.c", "if (!s_store_initialized || !store_lock()) {\n        return CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    locked = true;\n    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &handle);", "if (!s_store_initialized || false) {\n        return CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    locked = true;\n    error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &handle);"),
        ("validation-after-lock", "config_store.c", "if (!runtime_config_validate(&staged, NULL)) {\n        secure_zero(&staged, sizeof(staged));\n        return CONFIG_STORE_RESULT_INVALID;\n    }\n    memset(&current, 0, sizeof(current));\n    if (!s_store_initialized || !store_lock()) {", "memset(&current, 0, sizeof(current));\n    if (!s_store_initialized || !store_lock()) {\n        secure_zero(&staged, sizeof(staged));\n        secure_zero(&current, sizeof(current));\n        return CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    if (!runtime_config_validate(&staged, NULL)) {\n        secure_zero(&staged, sizeof(staged));\n        return CONFIG_STORE_RESULT_INVALID;\n    }"),
        ("load-clear-after-init", "config_store.c", "memset(config_out, 0, sizeof(*config_out));\n    memset(&candidate, 0, sizeof(candidate));\n    if (!s_store_initialized", "memset(&candidate, 0, sizeof(candidate));\n    if (!s_store_initialized || !store_lock()) {\n        secure_zero(&candidate, sizeof(candidate));\n        return CONFIG_STORE_RESULT_IO_ERROR;\n    }\n    memset(config_out, 0, sizeof(*config_out));\n    if (false"),
        ("load-fresh-branch-removed", "config_store.c", "error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &handle);\n    if (error == ESP_ERR_NVS_NOT_FOUND) {\n        result = CONFIG_STORE_RESULT_ABSENT;\n        goto cleanup;\n    }\n    if (error != ESP_OK)", "error = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &handle);\n    if (false) {\n        result = CONFIG_STORE_RESULT_ABSENT;\n        goto cleanup;\n    }\n    if (error != ESP_OK)"),
        ("init-error-widened", "config_store.c", "error == ESP_ERR_NVS_NEW_VERSION_FOUND)", "error == ESP_ERR_NVS_NEW_VERSION_FOUND || error == ESP_FAIL)"),
        ("boot-pin", "board.h", "#define BOARD_BOOT_BUTTON_PIN        9", "#define BOARD_BOOT_BUTTON_PIN        10"),
        ("reset-priority", "provisioning.c", "tskIDLE_PRIORITY + 1U", "tskIDLE_PRIORITY + 2U"),
        ("erase-mutex-removed", "config_store.c", "if (!s_store_initialized || !store_lock()) {\n        return CONFIG_STORE_RESULT_IO_ERROR;", "if (!s_store_initialized) {\n        return CONFIG_STORE_RESULT_IO_ERROR;"),
        ("load-opposite-slot-fallback", "config_store.c", "result = read_slot(handle, active, &candidate);", "result = read_slot(handle, active, &candidate);\n            if (result == CONFIG_STORE_RESULT_INVALID) {\n                (void)read_slot(handle, active == 0U ? 1U : 0U, &candidate);\n            }"),
    ]
    return result


def main() -> int:
    paths = {name: MAIN / name for name in ("config_store.c", "config_store.h", "provisioning.c", "provisioning.h", "board.h", "CMakeLists.txt")}
    validate_sources(paths)
    listed = mutations()
    for mutation in listed:
        mutate_and_reject(*mutation)
    print(f"config-store contract oracle passed: {len(listed)} temporary-copy mutations rejected")
    for name, *_ in listed:
        print(f"  {name}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractViolation, OSError) as error:
        print(f"config-store contract oracle failed: {error}")
        raise SystemExit(1)
