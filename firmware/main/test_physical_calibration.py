#!/usr/bin/env python3
"""Regression for the physically observed Waveshare LCD calibration."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
BOARD_HEADER = REPOSITORY / "firmware" / "main" / "board.h"
DISPLAY_SPIKE = REPOSITORY / "firmware" / "main" / "display_spike.c"
DISPLAY_PORT = REPOSITORY / "firmware" / "main" / "display_port.c"
CALIBRATION = REPOSITORY / "firmware" / "main" / "accepted_display_calibration.json"
ACCEPTED_COMMIT = "dffced31f7ce0f6f2195b6b5c39703123e3d4df5"
ACCEPTED_SHA256 = "fa0af4d29e55a4f61b51e9b3d3e74ea15302f3facba12d2107aefbf6777bf824"
ACCEPTED_REFERENCE = REPOSITORY / "firmware/reference/accepted_display_spike.c"


def macro_value(source: str, name: str) -> str:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(\S+)\s*$", source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing macro {name}")
    return match.group(1)


def ordered(source: str, *tokens: str) -> None:
    positions = []
    for token in tokens:
        position = source.find(token)
        if position < 0:
            raise AssertionError(f"missing ordered token: {token}")
        positions.append(position)
    if positions != sorted(positions):
        raise AssertionError(f"operation order changed: {tokens}")


def accepted_source() -> bytes:
    # Keep calibration verification reproducible without private Git history.
    return ACCEPTED_REFERENCE.read_bytes()


def assert_port_contract(source: str) -> None:
    if "abort(" in source or "ESP_ERROR_CHECK" in source:
        raise AssertionError("display_port must return errors, not abort")
    for required in (
        "display_port_start(display_port_t *port",
        "display_port_set_brightness(uint8_t percent)",
        "DISPLAY_FLOW_BACKLIGHT_INIT_OFF",
        "DISPLAY_FLOW_LVGL_ADD_DISPLAY",
        "DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF",
        "DISPLAY_FLOW_PUBLISH_DISPLAY",
        "lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_90)",
        ".sw_rotate = true",
        ".swap_bytes = false",
        ".buff_dma = true",
        "BOARD_LCD_SWAP_XY",
        "BOARD_LCD_MIRROR_X",
        "BOARD_LCD_MIRROR_Y",
        ".swap_xy = BOARD_LCD_SWAP_XY",
        ".mirror_x = BOARD_LCD_MIRROR_X",
        ".mirror_y = BOARD_LCD_MIRROR_Y",
    ):
        if required not in source:
            raise AssertionError(f"missing port contract token: {required}")
    execution = source[source.index("static int32_t display_port_execute"):]
    ordered(
        execution,
        "case DISPLAY_FLOW_BACKLIGHT_INIT_OFF",
        "case DISPLAY_FLOW_SHARED_SD_CS_HIGH",
        "case DISPLAY_FLOW_CREATE_SEMAPHORE",
        "case DISPLAY_FLOW_SPI_BUS_INIT",
        "case DISPLAY_FLOW_PANEL_IO_CREATE",
        "case DISPLAY_FLOW_ST7789_CREATE",
        "case DISPLAY_FLOW_PANEL_RESET",
        "case DISPLAY_FLOW_PANEL_INIT",
        "case DISPLAY_FLOW_PANEL_SET_GAP",
        "case DISPLAY_FLOW_PANEL_SWAP_XY",
        "case DISPLAY_FLOW_PANEL_MIRROR",
        "case DISPLAY_FLOW_PANEL_INVERT",
        "case DISPLAY_FLOW_ALLOCATE_FRAME",
        "case DISPLAY_FLOW_PRESENT_BLACK",
        "case DISPLAY_FLOW_PANEL_DISPLAY_ON",
        "case DISPLAY_FLOW_SET_INITIAL_BRIGHTNESS",
        "case DISPLAY_FLOW_LVGL_INIT",
        "case DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF",
        "case DISPLAY_FLOW_LVGL_ADD_DISPLAY",
        "case DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE",
        "case DISPLAY_FLOW_LVGL_LOCK",
        "case DISPLAY_FLOW_LVGL_ROTATE_90",
        "case DISPLAY_FLOW_ACTIVE_SCREEN_BLACK",
        "case DISPLAY_FLOW_ACCEPTANCE_CARD",
        "case DISPLAY_FLOW_LVGL_UNLOCK",
        "case DISPLAY_FLOW_PUBLISH_DISPLAY",
    )


def assert_flow_contract(source: str) -> None:
    source = source[source.index("int32_t display_port_flow_run"):]
    required = (
        "if (run_acceptance_sequence) {",
        "state.handoff_invoked = true;\n    const int32_t add_result = invoke(",
        "DISPLAY_FLOW_PUBLISH_DISPLAY);\n    return 0;",
    )
    for token in required:
        if token not in source:
            raise AssertionError(f"missing flow contract token: {token}")
    if source.count("if (run_acceptance_sequence) {") != 2:
        raise AssertionError("acceptance gating changed")
    ordered(
        source,
        "DISPLAY_FLOW_PRESENT_BLACK",
        "DISPLAY_FLOW_PANEL_DISPLAY_ON",
        "DISPLAY_FLOW_FREE_FRAME",
        "DISPLAY_FLOW_LVGL_INIT",
        "DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF",
        "state.handoff_invoked = true;",
        "DISPLAY_FLOW_LVGL_ADD_DISPLAY",
        "DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE",
        "DISPLAY_FLOW_LVGL_UNLOCK",
        "DISPLAY_FLOW_PUBLISH_DISPLAY",
    )


def assert_manifest_source_equivalence(manifest: dict, board: str, port: str) -> None:
    expected_board = {
        "id": "waveshare-esp32-c6-lcd-1.3",
        "orientation": "usb-c-right",
        "spi": {"host": "SPI2_HOST", "pixel_clock_hz": 12000000,
                "mode": 0, "command_bits": 8, "parameter_bits": 8, "queue_depth": 1},
        "pins": {"sclk": 7, "mosi": 6, "lcd_cs": 14, "dc": 15,
                 "reset": 21, "backlight": 22, "shared_sd_cs": 4},
    }
    expected_panel = {
        "width": 240, "height": 240, "gap_x": 0, "gap_y": 0,
        "bits_per_pixel": 16, "rgb_element_order": "LCD_RGB_ELEMENT_ORDER_RGB",
        "data_endian": "LCD_RGB_DATA_ENDIAN_LITTLE", "invert_color": True,
        "swap_xy": False, "mirror_x": False, "mirror_y": False,
    }
    expected_backlight = {
        "speed_mode": "LEDC_LOW_SPEED_MODE", "timer": "LEDC_TIMER_0",
        "channel": "LEDC_CHANNEL_0", "resolution_bits": 10,
        "frequency_hz": 5000, "active_high": True, "maximum_duty": 1023,
        "initial_percent": 25,
    }
    expected_transfer = {
        "frame_pixels": 57600, "frame_bytes": 115200,
        "maximum_transfer_bytes": 115208, "dma_capabilities": ["MALLOC_CAP_DMA", "MALLOC_CAP_INTERNAL"],
        "timeout_ms": 1000, "black_before_display_on": True,
    }
    expected_lvgl = {
        "horizontal_resolution": 240, "vertical_resolution": 240,
        "buffer_rows": 40, "buffer_pixels": 9600, "double_buffer": True,
        "monochrome": False, "color_format": "LV_COLOR_FORMAT_RGB565",
        "buff_dma": True, "software_rotation": True,
        "swap_bytes": False, "rotation": "LV_DISPLAY_ROTATION_90",
    }
    expected_startup = [
        "backlight_init_off", "shared_sd_cs_high", "create_semaphore",
        "spi_bus_init", "panel_io_create", "st7789_create", "panel_reset",
        "panel_init", "panel_set_gap", "panel_swap_xy", "panel_mirror",
        "panel_invert", "allocate_frame", "present_black_and_wait",
        "panel_display_on", "set_initial_brightness",
        "acceptance_solids_if_enabled", "free_completed_frame", "lvgl_init",
        "lvgl_ready_for_handoff", "lvgl_add_display", "delete_direct_semaphore",
        "lvgl_lock", "lvgl_rotate_90", "active_screen_black",
        "acceptance_card_if_enabled", "lvgl_unlock", "publish_display",
    ]
    expected_acceptance = {
        "solid_frames": [
            {"name": "RED", "rgb565": "0xF800", "duration_ms": 1000},
            {"name": "GREEN", "rgb565": "0x07E0", "duration_ms": 1000},
            {"name": "BLUE", "rgb565": "0x001F", "duration_ms": 1000},
            {"name": "WHITE", "rgb565": "0xFFFF", "duration_ms": 1000},
        ],
        "corner_blocks": [
            {"name": "red_tl", "x": 4, "y": 4, "width": 36, "height": 36, "rgb888": "0xFF0000"},
            {"name": "green_tr", "x": 200, "y": 4, "width": 36, "height": 36, "rgb888": "0x00FF00"},
            {"name": "blue_bl", "x": 4, "y": 200, "width": 36, "height": 36, "rgb888": "0x0000FF"},
            {"name": "white_br", "x": 200, "y": 200, "width": 36, "height": 36, "rgb888": "0xFFFFFF"},
        ],
        "digit_blocks": [
            {"x": 91, "y": 58, "width": 25, "height": 24, "rgb888": "0xFFFF00"},
            {"x": 108, "y": 42, "width": 28, "height": 156, "rgb888": "0xFFFF00"},
            {"x": 78, "y": 190, "width": 88, "height": 28, "rgb888": "0xFFFF00"},
        ],
    }
    if manifest["startup_order"] != expected_startup or manifest["acceptance"] != expected_acceptance:
        raise AssertionError("startup or acceptance manifest drift")
    if manifest["board"] != expected_board or manifest["panel"] != expected_panel:
        raise AssertionError("board/panel manifest drift")
    if manifest["backlight"] != expected_backlight or manifest["direct_transfer"] != expected_transfer:
        raise AssertionError("backlight/transfer manifest drift")
    if manifest["lvgl"] != expected_lvgl:
        raise AssertionError("LVGL manifest drift")
    source_tokens = (
        ".pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ", ".spi_mode = BOARD_LCD_SPI_MODE",
        ".lcd_cmd_bits = BOARD_LCD_CMD_BITS", ".lcd_param_bits = BOARD_LCD_PARAM_BITS",
        ".sclk_io_num = BOARD_LCD_PIN_SCLK", ".mosi_io_num = BOARD_LCD_PIN_MOSI",
        ".dc_gpio_num = BOARD_LCD_PIN_DC", ".cs_gpio_num = BOARD_LCD_PIN_CS",
        ".reset_gpio_num = BOARD_LCD_PIN_RESET", ".rgb_ele_order = BOARD_LCD_RGB_ELEMENT_ORDER",
        ".data_endian = LCD_RGB_DATA_ENDIAN_LITTLE", ".bits_per_pixel = 16",
        ".swap_xy = BOARD_LCD_SWAP_XY", ".mirror_x = BOARD_LCD_MIRROR_X",
        ".mirror_y = BOARD_LCD_MIRROR_Y", ".freq_hz = 5000", ".duty_resolution = LEDC_TIMER_10_BIT",
        ".channel = LEDC_CHANNEL_0", ".timer_num = LEDC_TIMER_0",
        ".max_transfer_sz = (int)(FRAME_BYTES + 8U)",
        ".buffer_size = BOARD_LCD_WIDTH * 40U", ".hres = BOARD_LCD_WIDTH",
        ".vres = BOARD_LCD_HEIGHT", ".double_buffer = true",
        ".color_format = LV_COLOR_FORMAT_RGB565", ".buff_dma = true",
        ".sw_rotate = true", ".swap_bytes = false",
        "LV_DISPLAY_ROTATION_90", "DIRECT_TRANSFER_TIMEOUT_MS",
        "show_solid(RGB565_RED)", "show_solid(RGB565_GREEN)",
        "show_solid(RGB565_BLUE)", "show_solid(RGB565_WHITE)",
        "if (percent > 100U)", "if (s_state != DISPLAY_PORT_STARTED)",
        "port->display = NULL", "s_state = DISPLAY_PORT_FAILED",
        "vTaskDelay(pdMS_TO_TICKS(ACCEPTANCE_DELAY_MS))",
        "create_orientation_pattern()", "DISPLAY_FLOW_PUBLISH_DISPLAY",
        "{4, 4, 36, 36, 0xFF0000}", "{200, 4, 36, 36, 0x00FF00}",
        "{4, 200, 36, 36, 0x0000FF}", "{200, 200, 36, 36, 0xFFFFFF}",
        "{91, 58, 25, 24, 0xFFFF00}", "{108, 42, 28, 156, 0xFFFF00}",
        "{78, 190, 88, 28, 0xFFFF00}",
    )
    for token in source_tokens:
        if token not in port:
            raise AssertionError(f"source/manifest mismatch: {token}")
    ordered(
        port,
        "esp_lcd_panel_draw_bitmap",
        "if (xSemaphoreTake(s_transfer_done,",
        "pdMS_TO_TICKS(DIRECT_TRANSFER_TIMEOUT_MS)",
        "case DISPLAY_FLOW_FREE_FRAME",
    )
    if "s_state = DISPLAY_PORT_STARTING;" not in port:
        raise AssertionError("startup state publication missing")


class PhysicalCalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.board = BOARD_HEADER.read_text(encoding="utf-8")
        cls.display = DISPLAY_SPIKE.read_text(encoding="utf-8")
        cls.manifest = json.loads(CALIBRATION.read_text(encoding="utf-8"))
        cls.port = DISPLAY_PORT.read_text(encoding="utf-8") if DISPLAY_PORT.exists() else ""
        flow_path = REPOSITORY / "firmware" / "main" / "display_port_flow.c"
        cls.flow = flow_path.read_text(encoding="utf-8") if flow_path.exists() else ""

    def test_usb_right_orientation_uses_partial_flush_safe_rotation(self) -> None:
        self.assertEqual(macro_value(self.board, "BOARD_LCD_SWAP_XY"), "false")
        self.assertEqual(macro_value(self.board, "BOARD_LCD_MIRROR_X"), "false")
        self.assertEqual(macro_value(self.board, "BOARD_LCD_MIRROR_Y"), "false")
        self.assertIn(".sw_rotate = true", self.port)
        self.assertIn("LV_DISPLAY_ROTATION_90", self.port)

    def test_panel_uses_physically_observed_rgb_element_order(self) -> None:
        self.assertEqual(
            macro_value(self.board, "BOARD_LCD_RGB_ELEMENT_ORDER"),
            "LCD_RGB_ELEMENT_ORDER_RGB",
        )
        self.assertIn("BOARD_LCD_RGB_ELEMENT_ORDER", self.port)

    def test_manifest_is_closed_and_pins_accepted_source(self) -> None:
        self.assertEqual(set(self.manifest), {
            "schema", "accepted_source", "board", "panel", "backlight",
            "direct_transfer", "lvgl", "startup_order", "acceptance",
        })
        source = accepted_source()
        self.assertEqual(self.manifest["schema"], 1)
        self.assertEqual(self.manifest["accepted_source"]["commit"], ACCEPTED_COMMIT)
        self.assertEqual(self.manifest["accepted_source"]["path"], str(ACCEPTED_REFERENCE.relative_to(REPOSITORY)))
        self.assertEqual(self.manifest["accepted_source"]["bytes"], len(source))
        self.assertEqual(self.manifest["accepted_source"]["bytes"], 9763)
        self.assertEqual(hashlib.sha256(source).hexdigest(), ACCEPTED_SHA256)
        self.assertEqual(self.manifest["accepted_source"]["sha256"], ACCEPTED_SHA256)

    def test_port_is_sole_production_owner_and_spike_is_thin(self) -> None:
        self.assertTrue(DISPLAY_PORT.exists())
        self.assertIn("display_port_start(&port, true)", self.display)
        for forbidden in (
            "esp_lcd_", "lvgl_port_", "heap_caps_", "xSemaphoreCreateBinary",
            "esp_lcd_new_panel", "lv_display_set_rotation",
        ):
            self.assertNotIn(forbidden, self.display)
        for path in (REPOSITORY / "firmware" / "main").glob("*.c"):
            if path.name in {"display_port.c", "display_port_flow.c"} or path.name.startswith("test_"):
                continue
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("esp_lcd_new_panel", source, path.name)
            self.assertNotIn("lvgl_port_add_disp", source, path.name)

    def test_port_source_contract_and_exact_successful_order(self) -> None:
        assert_port_contract(self.port)
        assert_flow_contract(self.flow)
        assert_manifest_source_equivalence(self.manifest, self.board, self.port)
        self.assertIn("port->display = NULL", self.port)
        self.assertIn("ESP_ERR_INVALID_STATE", self.port)
        self.assertIn("DISPLAY_FLOW_CLEANUP_LVGL_UNLOCK", self.port)
        self.assertIn("DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT", self.port)
        ordered(
            self.port,
            "if (port == NULL)", "if (s_state != DISPLAY_PORT_NOT_STARTED)",
            "port->display = NULL", "s_state = DISPLAY_PORT_STARTING",
            "display_port_flow_run", "s_state = DISPLAY_PORT_FAILED",
            "port->display = s_display", "s_state = DISPLAY_PORT_STARTED",
        )
        ordered(
            self.port,
            "if (percent > 100U)", "if (s_state != DISPLAY_PORT_STARTED)",
            "return backlight_set_percent(percent)",
        )

    def test_clock_firmware_polls_only_the_schema_2_display_route(self) -> None:
        net_task = (REPOSITORY / "firmware" / "main" / "net_task.c").read_text(encoding="utf-8")
        self.assertIn('"%s/api/v2/latest"', net_task)
        self.assertNotIn('"%s/api/v1/latest"', net_task)
        self.assertIn("#define NET_TASK_TICK_MS 1000U", net_task)
        self.assertIn("runtime_ui_snapshot_equal", net_task)
        self.assertIn("next_poll_monotonic_s", net_task)
        self.assertNotIn("vTaskDelay(pdMS_TO_TICKS(context->poll_s * 1000U))", net_task)
        self.assertIn("static void http_task_worker", net_task)
        self.assertIn("static void net_task_worker", net_task)
        http_path = net_task.split("static bool perform_http_exchange", 1)[1].split(
            "static void net_task_worker", 1
        )[0]
        runtime_worker = net_task.split("static void net_task_worker", 1)[1].split(
            "esp_err_t net_task_start", 1
        )[0]
        self.assertIn("esp_http_client_perform", http_path)
        self.assertIn("perform_http_exchange", http_path)
        self.assertNotIn("esp_http_client_perform", runtime_worker)
        self.assertNotIn("perform_http_exchange", runtime_worker)
        self.assertIn("mvp_http_flow_tick", runtime_worker)
        self.assertIn("vTaskDelayUntil", runtime_worker)
        self.assertGreaterEqual(net_task.count("xTaskCreate("), 2)

    def test_mutated_calibration_is_rejected_by_the_same_oracle(self) -> None:
        port_mutations = (
            (".sw_rotate = true", ".sw_rotate = false"),
            (".swap_bytes = false", ".swap_bytes = true"),
            (".buff_dma = true", ".buff_dma = false"),
            (".rgb_ele_order = BOARD_LCD_RGB_ELEMENT_ORDER", ".rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR"),
            ("BOARD_LCD_SWAP_XY", "true /* mutated */"),
            (
                "case DISPLAY_FLOW_PANEL_RESET:\n        return esp_lcd_panel_reset(s_panel);\n    case DISPLAY_FLOW_PANEL_INIT:",
                "case DISPLAY_FLOW_PANEL_INIT:\n        return esp_lcd_panel_init(s_panel);\n    case DISPLAY_FLOW_PANEL_RESET:",
            ),
            ("LV_DISPLAY_ROTATION_90", "LV_DISPLAY_ROTATION_0"),
            ("if (xSemaphoreTake(s_transfer_done,\n                       pdMS_TO_TICKS(DIRECT_TRANSFER_TIMEOUT_MS))", "/* DMA wait removed */"),
            ("if (percent > 100U)", "if (percent > 101U)"),
            ("{4, 4, 36, 36, 0xFF0000}", "{5, 4, 36, 36, 0xFF0000}"),
        )
        for original, replacement in port_mutations:
            with self.subTest(source="port", original=original):
                count = 2 if original == "if (percent > 100U)" else 1
                mutated = self.port.replace(original, replacement, count)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "display_port.c"
                    path.write_text(mutated, encoding="utf-8")
                    mutated = path.read_text(encoding="utf-8")
                with self.assertRaises(AssertionError):
                    assert_port_contract(mutated)
                    assert_manifest_source_equivalence(self.manifest, self.board, mutated)

        flow_mutations = (
            (
                "DISPLAY_FLOW_PRESENT_BLACK,\n        DISPLAY_FLOW_PANEL_DISPLAY_ON",
                "DISPLAY_FLOW_PANEL_DISPLAY_ON,\n        DISPLAY_FLOW_PRESENT_BLACK",
            ),
            (
                "state.handoff_invoked = true;\n    const int32_t add_result = invoke(",
                "const int32_t add_result = invoke(\n        execute, context, DISPLAY_FLOW_LVGL_ADD_DISPLAY);\n    state.handoff_invoked = true;",
            ),
            ("if (run_acceptance_sequence) {", "if (true) {"),
            (
                "const int32_t unlock_result = invoke(execute, context, DISPLAY_FLOW_LVGL_UNLOCK);\n    if (unlock_result != 0)",
                "const int32_t unlock_result = invoke(execute, context, DISPLAY_FLOW_PUBLISH_DISPLAY);\n    if (unlock_result != 0)",
            ),
        )
        for original, replacement in flow_mutations:
            with self.subTest(source="flow", original=original):
                mutated = self.flow.replace(original, replacement, 1)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "display_port_flow.c"
                    path.write_text(mutated, encoding="utf-8")
                    mutated = path.read_text(encoding="utf-8")
                with self.assertRaises(AssertionError):
                    assert_flow_contract(mutated)

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "display_port.c"
            mutated = self.port.replace("case DISPLAY_FLOW_FREE_FRAME:", "", 1)
            mutated = mutated.replace("static esp_err_t frame_present(void)", "case DISPLAY_FLOW_FREE_FRAME:\nstatic esp_err_t frame_present(void)", 1)
            path.write_text(mutated, encoding="utf-8")
            with self.assertRaises(AssertionError):
                assert_manifest_source_equivalence(self.manifest, self.board, path.read_text(encoding="utf-8"))

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "display_port_flow.c"
            original = (
                "const int32_t unlock_result = invoke(execute, context, DISPLAY_FLOW_LVGL_UNLOCK);\n"
                "    if (unlock_result != 0) {\n"
                "        quarantine(&state, execute, context);\n"
                "        return unlock_result;\n"
                "    }\n"
                "    state.lock_held = false;\n\n"
                "    (void)invoke(execute, context, DISPLAY_FLOW_PUBLISH_DISPLAY);"
            )
            replacement = (
                "(void)invoke(execute, context, DISPLAY_FLOW_PUBLISH_DISPLAY);\n\n"
                "    const int32_t unlock_result = invoke(execute, context, DISPLAY_FLOW_LVGL_UNLOCK);\n"
                "    if (unlock_result != 0) {\n"
                "        quarantine(&state, execute, context);\n"
                "        return unlock_result;\n"
                "    }\n"
                "    state.lock_held = false;"
            )
            mutated = self.flow.replace(original, replacement, 1)
            path.write_text(mutated, encoding="utf-8")
            with self.assertRaises(AssertionError):
                assert_flow_contract(path.read_text(encoding="utf-8"))
        manifest_mutated = json.loads(json.dumps(self.manifest))
        manifest_mutated["panel"]["swap_xy"] = True
        with self.assertRaises(AssertionError):
            assert_manifest_source_equivalence(manifest_mutated, self.board, self.port)


if __name__ == "__main__":
    unittest.main()
