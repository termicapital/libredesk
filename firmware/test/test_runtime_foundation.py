from __future__ import annotations

import csv
import io
import re
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
PARTITIONS_PATH = REPO_ROOT / "firmware" / "partitions.csv"
SDKCONFIG_DEFAULTS_PATH = REPO_ROOT / "firmware" / "sdkconfig.defaults"
VERSION_PATH = REPO_ROOT / "firmware" / "version.txt"
BUILD_SCRIPT_PATH = REPO_ROOT / "scripts" / "build-display-spike.sh"
MAIN_CMAKE_PATH = REPO_ROOT / "firmware" / "main" / "CMakeLists.txt"
WIFI_MANAGER_PATH = REPO_ROOT / "firmware" / "main" / "wifi_manager.c"
APP_MAIN_PATH = REPO_ROOT / "firmware" / "main" / "app_main.c"
FLASH_SIZE_BYTES = 4 * 1024 * 1024

PartitionTuple = tuple[str, str, str, str, str]
EXPECTED_PARTITIONS: list[PartitionTuple] = [
    ("nvs", "data", "nvs", "0x9000", "0x6000"),
    ("otadata", "data", "ota", "0xf000", "0x2000"),
    ("phy_init", "data", "phy", "0x11000", "0x1000"),
    ("ota_0", "app", "ota_0", "0x20000", "0x190000"),
    ("ota_1", "app", "ota_1", "0x1b0000", "0x190000"),
    ("coredump", "data", "coredump", "0x340000", "0x10000"),
]
REQUIRED_SDKCONFIG = {
    "CONFIG_PARTITION_TABLE_CUSTOM": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": '"partitions.csv"',
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH": "y",
    "CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF": "y",
    "CONFIG_ESP_COREDUMP_CHECKSUM_SHA256": "y",
    "CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE": "y",
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
    "CONFIG_ESP_TASK_WDT_EN": "y",
    "CONFIG_ESP_TASK_WDT_INIT": "y",
    "CONFIG_ESP_TASK_WDT_PANIC": "y",
    "CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN": "y",
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN": "4096",
    "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN": "2048",
    "CONFIG_MBEDTLS_HAVE_TIME_DATE": "n",
    "CONFIG_LV_FONT_MONTSERRAT_24": "y",
    "CONFIG_LWIP_SNTP_MAX_SERVERS": "3",
    "CONFIG_LWIP_SNTP_STARTUP_DELAY": "y",
    "CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY": "5000",
}

SNTP_RECV_TIMEOUT_MS = 15000


def parse_partition_csv(text: str) -> list[PartitionTuple]:
    partitions: list[PartitionTuple] = []
    for line_number, row in enumerate(csv.reader(io.StringIO(text)), start=1):
        values = [value.strip() for value in row]
        if not values or not values[0] or values[0].startswith("#"):
            continue
        if len(values) != 6 or values[5]:
            raise ValueError(f"partition row {line_number} must have five fields and empty flags")
        partitions.append((values[0], values[1], values[2], values[3], values[4]))
    return partitions


def parse_numeric_value(value: str) -> int:
    normalized = value.strip().upper()
    multiplier = 1
    if normalized.endswith("K"):
        normalized = normalized[:-1]
        multiplier = 1024
    elif normalized.endswith("M"):
        normalized = normalized[:-1]
        multiplier = 1024 * 1024
    return int(normalized, 0) * multiplier


def normalized_partition_layout(
    partitions: list[PartitionTuple],
) -> list[tuple[str, str, str, int, int]]:
    return [
        (name, partition_type, subtype, parse_numeric_value(offset), parse_numeric_value(size))
        for name, partition_type, subtype, offset, size in partitions
    ]


def verify_generated_partition_binary(binary_path: Path, partition_tool_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(partition_tool_path), str(binary_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise ValueError(f"could not decode generated partition binary: {result.stderr.strip()}")
    decoded = parse_partition_csv(result.stdout)
    if normalized_partition_layout(decoded) != normalized_partition_layout(EXPECTED_PARTITIONS):
        raise ValueError("generated partition binary does not match the exact runtime layout")


def verify_linked_symbol(elf_path: Path, nm_path: Path, symbol: str) -> None:
    if not elf_path.is_file():
        raise ValueError(f"linked ELF is missing: {elf_path}")
    if not nm_path.is_file():
        raise ValueError(f"nm executable is missing: {nm_path}")
    result = subprocess.run(
        [str(nm_path), "--defined-only", "--extern-only", str(elf_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise ValueError(f"could not inspect linked ELF: {result.stderr.strip()}")
    definitions = [
        line.split()[-1]
        for line in result.stdout.splitlines()
        if line.split() and line.split()[-1] == symbol
    ]
    if len(definitions) != 1:
        raise ValueError(f"linked ELF must define {symbol} exactly once")


def validate_partition_bounds(partitions: list[PartitionTuple]) -> None:
    ranges: list[tuple[int, int, str]] = []
    for name, _partition_type, _subtype, offset_text, size_text in partitions:
        try:
            offset = parse_numeric_value(offset_text)
            size = parse_numeric_value(size_text)
        except ValueError as error:
            raise ValueError(f"partition {name} has a non-numeric offset or size") from error
        if offset < 0 or size <= 0 or offset + size > FLASH_SIZE_BYTES:
            raise ValueError(f"partition {name} exceeds the 4 MiB flash boundary")
        ranges.append((offset, offset + size, name))

    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] > current[0]:
            raise ValueError(f"partitions {previous[2]} and {current[2]} overlap")


def read_active_sdkconfig(path: Path) -> dict[str, str]:
    settings: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        if name in settings:
            raise ValueError(f"duplicate sdkconfig default {name} on line {line_number}")
        settings[name] = value
    return settings


def model_sntp_fallback(
    hosts: list[str],
    outcomes: list[str],
    maximum_startup_delay_ms: int,
    receive_timeout_ms: int,
) -> tuple[list[str], int]:
    if len(hosts) != len(outcomes):
        raise ValueError("one outcome is required per SNTP host")
    attempted: list[str] = []
    elapsed_ms = maximum_startup_delay_ms
    for host, outcome in zip(hosts, outcomes):
        attempted.append(host)
        if outcome == "success":
            return attempted, elapsed_ms
        if outcome == "timeout":
            elapsed_ms += receive_timeout_ms
        elif outcome != "dns_failure":
            raise ValueError(f"unsupported SNTP outcome: {outcome}")
    return attempted, elapsed_ms


class RuntimeFoundationTests(unittest.TestCase):
    def test_partition_table_matches_exact_ota_coredump_layout(self) -> None:
        self.assertTrue(PARTITIONS_PATH.is_file(), "firmware/partitions.csv must exist")
        partitions = parse_partition_csv(PARTITIONS_PATH.read_text(encoding="utf-8"))
        self.assertEqual(partitions, EXPECTED_PARTITIONS)
        validate_partition_bounds(partitions)

    def test_partition_validator_rejects_overlap(self) -> None:
        overlapping = EXPECTED_PARTITIONS.copy()
        overlapping[1] = ("otadata", "data", "ota", "0xe000", "0x2000")
        with self.assertRaisesRegex(ValueError, "overlap"):
            validate_partition_bounds(overlapping)

    def test_partition_validator_rejects_end_beyond_4_mib(self) -> None:
        oversized = EXPECTED_PARTITIONS.copy()
        oversized[-1] = ("coredump", "data", "coredump", "0x3f8000", "0x10000")
        with self.assertRaisesRegex(ValueError, "4 MiB"):
            validate_partition_bounds(oversized)

    def test_sdkconfig_defaults_pin_runtime_security_foundation(self) -> None:
        settings = read_active_sdkconfig(SDKCONFIG_DEFAULTS_PATH)
        for name, expected in REQUIRED_SDKCONFIG.items():
            with self.subTest(name=name):
                self.assertEqual(settings.get(name), expected)
        self.assertNotEqual(settings.get("CONFIG_PARTITION_TABLE_SINGLE_APP"), "y")
        self.assertNotEqual(settings.get("CONFIG_ESP_COREDUMP_ENABLE_TO_UART"), "y")
        self.assertNotEqual(settings.get("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE"), "y")
        self.assertNotEqual(settings.get("CONFIG_ESP_COREDUMP_DATA_FORMAT_BIN"), "y")
        self.assertNotEqual(settings.get("CONFIG_ESP_COREDUMP_CHECKSUM_CRC32"), "y")

    def test_build_script_runs_foundation_gate_before_idf_build(self) -> None:
        script = BUILD_SCRIPT_PATH.read_text(encoding="utf-8")
        foundation_gate = (
            '"$python_bin" "$repo_root/firmware/test/test_runtime_foundation.py" -v'
        )
        self.assertIn(foundation_gate, script)
        self.assertLess(script.index(foundation_gate), script.index("idf.py"))
        self.assertNotIn("TEST_COMPONENTS", script)
        self.assertIn('idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" set-target esp32c6', script)
        self.assertIn('idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" build', script)
        font_linkage_gate = '--verify-symbol "$IDF_BUILD_DIR/libre_desk_mvp.elf"'
        self.assertIn(font_linkage_gate, script)
        self.assertGreater(
            script.index(font_linkage_gate),
            script.index('idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" build'),
        )
        self.assertLess(script.index(font_linkage_gate), script.index("package_firmware.py"))
        self.assertIn(
            '"$python_bin" "$repo_root/firmware/test/test_runtime_foundation.py" \\\n  --verify-binary "$IDF_BUILD_DIR/partition_table/partition-table.bin"',
            script,
        )
        self.assertIn('"$IDF_PATH/components/partition_table/gen_esp32part.py"', script)
        self.assertGreater(script.index('--verify-binary "$IDF_BUILD_DIR/partition_table/partition-table.bin"'), script.index('idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" build'))
        self.assertIn('--build-dir "$IDF_BUILD_DIR"', script)
        self.assertIn('--output-dir "$FLASHER_OUTPUT_DIR"', script)
        self.assertIn('--project-name libre_desk_mvp', script)
        self.assertIn('--release-name "Libre Desk live-data MVP"', script)

    def test_coredump_is_a_production_component_dependency(self) -> None:
        component = MAIN_CMAKE_PATH.read_text(encoding="utf-8")
        self.assertIn("PRIV_REQUIRES espcoredump", component)

    def test_decoded_partition_units_normalize_to_exact_layout(self) -> None:
        decoded = parse_partition_csv(
            """# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,24K,
otadata,data,ota,0xf000,8K,
phy_init,data,phy,0x11000,4K,
ota_0,app,ota_0,0x20000,1600K,
ota_1,app,ota_1,0x1b0000,1600K,
coredump,data,coredump,0x340000,64K,
"""
        )
        self.assertEqual(
            normalized_partition_layout(decoded),
            normalized_partition_layout(EXPECTED_PARTITIONS),
        )

    def test_runtime_foundation_has_distinct_version(self) -> None:
        self.assertEqual(
            VERSION_PATH.read_text(encoding="utf-8").strip(),
            "0.5.4-webserial-wifi",
        )

    def test_sntp_runs_in_background_without_blocking_https_startup(self) -> None:
        wifi_manager = WIFI_MANAGER_PATH.read_text(encoding="utf-8")
        app_main = APP_MAIN_PATH.read_text(encoding="utf-8")
        settings = read_active_sdkconfig(SDKCONFIG_DEFAULTS_PATH)
        server_list = re.search(
            r"ESP_SNTP_SERVER_LIST\((.*?)\)\s*\)",
            wifi_manager,
            flags=re.DOTALL,
        )
        if server_list is None:
            self.fail("SNTP server list must be configured")
        hosts = re.findall(r'"([^"]+)"', server_list.group(1))
        self.assertEqual(
            hosts,
            ["time.google.com", "time.cloudflare.com", "pool.ntp.org"],
        )
        self.assertEqual(int(settings["CONFIG_LWIP_SNTP_MAX_SERVERS"]), len(hosts))

        timeout_match = re.search(
            r"#define\s+MVP_SNTP_TIMEOUT_MS\s+(\d+)U",
            app_main,
        )
        if timeout_match is None:
            self.fail("SNTP startup timeout must be configured")
        startup_grace_ms = int(timeout_match.group(1))
        self.assertEqual(startup_grace_ms, 5000)
        self.assertIn("WIFI_STARTUP_EVENT_TIME_UNAVAILABLE", wifi_manager)
        self.assertNotIn("esp_netif_sntp_deinit", wifi_manager)
        self.assertEqual(settings.get("CONFIG_LWIP_SNTP_STARTUP_DELAY"), "y")
        self.assertEqual(
            settings.get("CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY"),
            "5000",
        )
        startup_delay_ms = int(settings["CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY"])

        timeout_attempts, timeout_elapsed_ms = model_sntp_fallback(
            hosts,
            ["timeout"] * len(hosts),
            startup_delay_ms,
            SNTP_RECV_TIMEOUT_MS,
        )
        self.assertEqual(timeout_attempts, hosts)
        self.assertEqual(
            timeout_elapsed_ms,
            startup_delay_ms + len(hosts) * SNTP_RECV_TIMEOUT_MS,
        )
        self.assertGreater(timeout_elapsed_ms, startup_grace_ms)

        dns_attempts, dns_elapsed_ms = model_sntp_fallback(
            hosts,
            ["dns_failure", "dns_failure", "success"],
            startup_delay_ms,
            SNTP_RECV_TIMEOUT_MS,
        )
        self.assertEqual(dns_attempts, hosts)
        self.assertEqual(dns_elapsed_ms, startup_delay_ms)

    def test_linked_symbol_verifier_requires_one_exact_definition(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="00000000 D lv_font_montserrat_24\n",
            stderr="",
        )
        with mock.patch("subprocess.run", return_value=completed):
            with mock.patch.object(Path, "is_file", return_value=True):
                verify_linked_symbol(Path("firmware.elf"), Path("nm"), "lv_font_montserrat_24")

        substring_only = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="00000000 D lv_font_montserrat_24_extra\n",
            stderr="",
        )
        with mock.patch("subprocess.run", return_value=substring_only):
            with mock.patch.object(Path, "is_file", return_value=True):
                with self.assertRaisesRegex(ValueError, "exactly once"):
                    verify_linked_symbol(Path("firmware.elf"), Path("nm"), "lv_font_montserrat_24")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--verify-binary":
        verify_generated_partition_binary(Path(sys.argv[2]), Path(sys.argv[3]))
        print("generated partition binary matches exact runtime layout")
    elif len(sys.argv) == 5 and sys.argv[1] == "--verify-symbol":
        verify_linked_symbol(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
        print(f"linked ELF defines {sys.argv[4]} exactly once")
    else:
        unittest.main()
