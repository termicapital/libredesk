#!/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/libre-desk-mvp-config-build-$$"
SECRETS_HEADER="$BUILD_DIR/mvp_test_secrets.h"
trap 'rm -rf "$BUILD_DIR"' EXIT
mkdir -p "$BUILD_DIR"

cat > "$SECRETS_HEADER" <<'EOF'
#define MVP_WIFI_SSID "fixture-ssid"
#define MVP_WIFI_PASSWORD "fixture-password"
#define MVP_BRIDGE_ORIGIN "https://fixture.example.test"
#define MVP_DEVICE_TOKEN "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBE"
EOF

cc="${CC:-gcc}"
command -v "$cc" >/dev/null 2>&1 || {
  echo "requested compiler is unavailable: $cc" >&2
  exit 127
}
"$cc" -std=c99 -Wall -Wextra -Werror -pedantic \
  -I"$ROOT_DIR/firmware/components/runtime_core/include" \
  -I"$ROOT_DIR/firmware/components/glucose_core/include" \
  -I"$ROOT_DIR/firmware/main" -I"$BUILD_DIR" \
  -DMVP_CONFIG_SECRETS_HEADER='"mvp_test_secrets.h"' \
  "$ROOT_DIR/firmware/components/runtime_core/runtime_config.c" \
  "$ROOT_DIR/firmware/main/mvp_config.c" \
  "$ROOT_DIR/firmware/main/test/test_mvp_config.c" \
  -lm -o "$BUILD_DIR/test_mvp_config"
"$BUILD_DIR/test_mvp_config"
echo "mvp_config host tests passed"
