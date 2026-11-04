from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MAIN = ROOT / "firmware" / "main"


class ProvisioningSecurityPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.portal = (MAIN / "provisioning_portal.c").read_text(encoding="utf-8")
        cls.session = (MAIN / "provisioning_session.c").read_text(encoding="utf-8")
        cls.app = (MAIN / "app_main.c").read_text(encoding="utf-8")
        cls.store = (MAIN / "config_store.c").read_text(encoding="utf-8")
        cls.gesture = (MAIN / "provisioning.c").read_text(encoding="utf-8")
        cls.wifi_manager = (MAIN / "wifi_manager.c").read_text(encoding="utf-8")

    def test_http_surface_has_exactly_wifi_fields(self) -> None:
        html_match = re.search(
            r"static const char PORTAL_HTML\[\] =(?P<body>.*?);\nstatic const char PORTAL_ACCEPTED",
            self.portal,
            re.DOTALL,
        )
        if html_match is None:
            self.fail("PORTAL_HTML declaration is missing")
        html = html_match.group("body")
        self.assertEqual(
            re.findall(r"<input[^>]*\bname=([a-z_]+)", html),
            ["ssid", "password"],
        )
        self.assertIn("maxlength=32", html)
        self.assertIn("maxlength=63", html)
        for forbidden in ("bridge_origin", "bridge_url", "device_token", "token"):
            self.assertNotIn(forbidden, html.lower())
            self.assertNotIn(forbidden, self.portal.lower())

    def test_submission_is_acknowledged_before_radio_validation_wakeup(self) -> None:
        handler = self.portal[
            self.portal.index("static esp_err_t post_handler") :
            self.portal.index("static void wifi_event_handler")
        ]
        self.assertLess(
            handler.index('send_response(request, "202 Accepted"'),
            handler.index("xTaskNotifyGive(portal->worker_task)"),
        )
        self.assertIn("wifi_provisioning_parse_form", handler)
        self.assertIn("WIFI_PROVISIONING_FORM_MAX_BYTES", handler)
        self.assertNotIn("ESP_LOG", handler)

    def test_candidate_requires_got_ip_and_apsta(self) -> None:
        self.assertIn("WIFI_MODE_APSTA", self.portal)
        self.assertIn("IP_EVENT_STA_GOT_IP", self.portal)
        connect = self.portal[
            self.portal.index("static bool connect_candidate") :
            self.portal.index("static config_store_result_t save_candidate")
        ]
        self.assertIn("xEventGroupWaitBits", connect)
        self.assertIn("PORTAL_GOT_IP_BIT", connect)
        self.assertNotIn("config_store_save", connect)

    def test_transaction_order_and_marker_boundary_are_explicit(self) -> None:
        self.assertLess(
            self.session.index("ops->connect_candidate"),
            self.session.index("ops->save_candidate"),
        )
        self.assertLess(
            self.session.index("ops->reload_active"),
            self.session.index("ops->clear_setup_request"),
        )
        self.assertLess(
            self.session.index("ops->clear_setup_request"),
            self.session.index("ops->restart"),
        )
        self.assertIn('#define CONFIG_STORE_NAMESPACE "libre_desk"', self.store)
        self.assertIn('#define CONFIG_STORE_SETUP_NAMESPACE "libre_setup"', self.store)
        factory = self.store[
            self.store.index("config_store_factory_reset_request") :
            self.store.index("config_store_erase_namespace")
        ]
        self.assertLess(
            factory.index("CONFIG_STORE_SETUP_NAMESPACE"),
            factory.index("nvs_erase_all"),
        )
        self.assertNotIn("nvs_flash_erase", factory)

    def test_boot_selects_before_normal_network_tasks(self) -> None:
        self.assertLess(self.app.index("initialize_nvs()"), self.app.index("app_boot_flow_select"))
        self.assertLess(self.app.index("config_store_init()"), self.app.index("app_boot_flow_select"))
        self.assertLess(
            self.app.index("app_boot_flow_select"),
            self.app.index("wifi_manager_connect_and_sync"),
        )
        self.assertLess(
            self.app.index("APP_BOOT_MODE_SETUP"),
            self.app.index("wifi_manager_connect_and_sync"),
        )
        self.assertIn("provisioning_portal_start", self.app)
        self.assertIn("provisioning_reset_monitor_start", self.app)

    def test_runtime_gesture_is_only_five_second_setup(self) -> None:
        self.assertIn("RESET_MONITOR_SETUP_HOLD_US INT64_C(5000000)", self.gesture)
        self.assertIn("config_store_setup_request_set", self.gesture)
        self.assertNotIn("FACTORY_HOLD", self.gesture)
        self.assertNotIn("config_store_factory_reset_request", self.gesture)
        self.assertNotIn("nvs_flash_erase", self.gesture)

    def test_usb_install_provisioning_precedes_normal_wifi(self) -> None:
        source_path = MAIN / "improv_wifi.c"
        self.assertTrue(source_path.is_file(), "missing USB Improv adapter")
        source = source_path.read_text(encoding="utf-8")
        defaults = (ROOT / "firmware" / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )

        self.assertIn('#include "improv_wifi.h"', self.app)
        setup_start = self.app.index("if (boot_mode == APP_BOOT_MODE_SETUP)")
        prepare_start = self.app.index("wifi_manager_prepare")
        self.assertLess(setup_start, prepare_start)
        self.assertIn("return;", self.app[setup_start:prepare_start])
        self.assertLess(
            prepare_start,
            self.app.index("improv_wifi_start"),
        )
        self.assertIn("improv_wifi_start", self.app)
        self.assertLess(
            self.app.index("improv_wifi_start"),
            self.app.index("wifi_manager_connect_and_sync"),
        )
        self.assertIn("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", defaults)
        self.assertIn("CONFIG_ESP_CONSOLE_SECONDARY_NONE=y", defaults)
        self.assertIn("provisioning_session_apply", source)
        self.assertIn("wifi_manager_try_candidate", source)
        self.assertNotIn("accept_without_radio_test", source)
        self.assertIn("IMPROV_SERIAL_RPC_REQUEST_WIFI_NETWORKS", source)
        self.assertIn("IMPROV_SERIAL_ERROR_UNKNOWN_RPC", source)
        self.assertNotIn("wifi.ssid", source)
        self.assertNotIn("wifi.password", source)

        candidate = self.wifi_manager[
            self.wifi_manager.index("wifi_manager_try_candidate") :
        ]
        self.assertIn("xEventGroupWaitBits", candidate)
        self.assertIn("WIFI_CONNECTED_BIT", candidate)
        self.assertIn("connected_ssid_matches", candidate)
        self.assertIn("esp_wifi_sta_get_ap_info", self.wifi_manager)
        self.assertIn("fallback", candidate)

    def test_wifi_manager_accepts_exact_driver_credential_capacities(self) -> None:
        start = self.wifi_manager.index("static bool station_config_build")
        end = self.wifi_manager.index("static esp_err_t restore_fallback_locked", start)
        builder = self.wifi_manager[start:end]
        self.assertIn("ssid_length > sizeof(station_out->sta.ssid)", builder)
        self.assertIn("password_length > sizeof(station_out->sta.password)", builder)
        self.assertNotIn("ssid_length >=", builder)
        self.assertNotIn("password_length >=", builder)

    def test_usb_protocol_suppresses_shared_console_logs_before_task_start(self) -> None:
        source = (MAIN / "improv_wifi.c").read_text(encoding="utf-8")
        self.assertIn("discard_log_output", source)
        self.assertIn("esp_log_set_vprintf(discard_log_output)", source)
        self.assertLess(
            source.index("esp_log_set_vprintf(discard_log_output)"),
            source.index("xTaskCreateStatic"),
        )

    def test_fallback_restore_is_verified_before_candidate_override_clears(self) -> None:
        start = self.wifi_manager.index("static esp_err_t restore_fallback_locked")
        end = self.wifi_manager.index("static bool connected_ssid_matches", start)
        restore = self.wifi_manager[start:end]
        set_config = restore.index("esp_wifi_set_config")
        clear_override = restore.index("candidate_override = false")
        self.assertLess(set_config, clear_override)
        self.assertIn("if (error != ESP_OK) return error", restore)
        self.assertIn("return error", restore)


if __name__ == "__main__":
    unittest.main()
