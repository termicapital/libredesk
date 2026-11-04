#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_idf_version="v5.5.5"
export IDF_PATH="${IDF_PATH:-$HOME/.local/share/esp-idf-v5.5.5}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
python_bin="${PYTHON_BIN:-python3}"
python_minor="$($python_bin -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$IDF_TOOLS_PATH/python_env/idf5.5_py${python_minor}_env}"
IDF_BUILD_DIR="${IDF_BUILD_DIR:-$repo_root/firmware/build}"
FLASHER_OUTPUT_DIR="${FLASHER_OUTPUT_DIR:-$repo_root/tools/flasher}"

case "$IDF_BUILD_DIR" in
  /*) ;;
  *) printf 'IDF_BUILD_DIR must be absolute\n' >&2; exit 1 ;;
esac
case "$FLASHER_OUTPUT_DIR" in
  /*) ;;
  *) printf 'FLASHER_OUTPUT_DIR must be absolute\n' >&2; exit 1 ;;
esac
if ! "$python_bin" - "$IDF_BUILD_DIR" "$FLASHER_OUTPUT_DIR" <<'PY'
import sys
from pathlib import Path

build = Path(sys.argv[1]).resolve(strict=False)
output = Path(sys.argv[2]).resolve(strict=False)
if build == output or build in output.parents or output in build.parents:
    raise SystemExit("IDF_BUILD_DIR and FLASHER_OUTPUT_DIR must be disjoint")
PY
then
  exit 1
fi

actual_idf_version="$(git -C "$IDF_PATH" describe --tags --always --dirty 2>/dev/null || true)"
if [[ "$actual_idf_version" != "$expected_idf_version" ]]; then
  printf 'Refusing to build with ESP-IDF %s; expected %s at %s\n' \
    "${actual_idf_version:-unknown}" "$expected_idf_version" "$IDF_PATH" >&2
  exit 1
fi

"$python_bin" "$repo_root/firmware/test/test_runtime_foundation.py" -v
"$python_bin" "$repo_root/firmware/main/test_physical_calibration.py" -v

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null

# Regenerate the ignored sdkconfig on every build so source-controlled defaults are authoritative.
rm -f "$repo_root/firmware/sdkconfig" "$repo_root/firmware/sdkconfig.old"
idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" set-target esp32c6
idf.py -C "$repo_root/firmware" -B "$IDF_BUILD_DIR" build
nm_bin="$(command -v riscv32-esp-elf-nm || true)"
if [[ -z "$nm_bin" ]]; then
  printf 'riscv32-esp-elf-nm is unavailable after ESP-IDF export\n' >&2
  exit 1
fi
"$python_bin" "$repo_root/firmware/test/test_runtime_foundation.py" \
  --verify-symbol "$IDF_BUILD_DIR/libre_desk_mvp.elf" \
  "$nm_bin" \
  lv_font_montserrat_24
"$python_bin" "$repo_root/firmware/test/test_runtime_foundation.py" \
  --verify-binary "$IDF_BUILD_DIR/partition_table/partition-table.bin" \
  "$IDF_PATH/components/partition_table/gen_esp32part.py"
firmware_version="$($python_bin -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").strip())' "$repo_root/firmware/version.txt")"
"$python_bin" "$repo_root/tools/flasher/package_firmware.py" \
  --build-dir "$IDF_BUILD_DIR" \
  --output-dir "$FLASHER_OUTPUT_DIR" \
  --version "$firmware_version" \
  --project-name libre_desk_mvp \
  --release-name "Libre Desk live-data MVP"

printf 'Live-data MVP built and packaged at %s\n' "$FLASHER_OUTPUT_DIR"
