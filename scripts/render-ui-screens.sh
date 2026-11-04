#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UISIM_DIR="$ROOT_DIR/tools/uisim"
LVGL_DIR="$ROOT_DIR/firmware/managed_components/lvgl__lvgl"
TARGET_DIR="$ROOT_DIR/docs/screens"
LOCK_HELPER="$UISIM_DIR/secure_render_lock.py"
PUBLISH_HELPER="$UISIM_DIR/publish_screens.py"

if [[ "${LIBRE_DESK_RENDER_LOCK_HELD:-0}" != "1" ]]; then
  exec python3 "$LOCK_HELPER" acquire "$ROOT_DIR" -- "$0" "$@"
fi
if [[ -z "${LIBRE_DESK_RENDER_LOCK_FD:-}" || -z "${LIBRE_DESK_RENDER_REPOSITORY_FD:-}" ]]; then
  echo "Inherited UI render lock descriptor is missing" >&2
  exit 73
fi
python3 "$LOCK_HELPER" validate "$ROOT_DIR" \
  "$LIBRE_DESK_RENDER_LOCK_FD" "$LIBRE_DESK_RENDER_REPOSITORY_FD"
unset LIBRE_DESK_RENDER_LOCK_HELD LIBRE_DESK_RENDER_LOCK_FD \
  LIBRE_DESK_RENDER_REPOSITORY_FD

if [[ ! -f "$LVGL_DIR/lv_version.h" || ! -f "$LVGL_DIR/CMakeLists.txt" ]]; then
  echo "Exact managed LVGL source is missing at $LVGL_DIR" >&2
  echo "Run scripts/build-display-spike.sh first, then retry make screens." >&2
  exit 1
fi

WORK_DIR="$(mktemp -d /tmp/libre-desk-uisim.XXXXXXXX)"
chmod 0700 "$WORK_DIR"
BUILD_DIR="$WORK_DIR/build"
RAW_DIR="$WORK_DIR/raw"
RAW_VERIFY_DIR="$WORK_DIR/raw-verify"
OUTPUT_DIR="$WORK_DIR/output"
VERIFY_DIR="$WORK_DIR/verify"
mkdir -m 0700 -- "$BUILD_DIR" "$RAW_DIR" "$RAW_VERIFY_DIR" "$OUTPUT_DIR" "$VERIFY_DIR"

cleanup() {
  rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT

cmake -S "$UISIM_DIR" -B "$BUILD_DIR" -DLVGL_SOURCE_DIR="$LVGL_DIR"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

"$BUILD_DIR/uisim" "$RAW_DIR"
python3 "$UISIM_DIR/generate_screens.py" "$RAW_DIR" "$OUTPUT_DIR"
python3 "$UISIM_DIR/test_outputs.py" "$OUTPUT_DIR"

"$BUILD_DIR/uisim" "$RAW_VERIFY_DIR" >/dev/null
python3 "$UISIM_DIR/generate_screens.py" "$RAW_VERIFY_DIR" "$VERIFY_DIR" >/dev/null
python3 "$UISIM_DIR/test_outputs.py" "$VERIFY_DIR" >/dev/null
if ! diff -qr -- "$OUTPUT_DIR" "$VERIFY_DIR" >/dev/null; then
  echo "UI renderer is not deterministic across consecutive runs" >&2
  exit 1
fi

python3 "$PUBLISH_HELPER" --candidate "$OUTPUT_DIR" --target "$TARGET_DIR"

echo "Published deterministic LVGL review artifacts to docs/screens"
