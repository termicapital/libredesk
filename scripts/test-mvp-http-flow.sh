#!/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/libre-desk-mvp-http-flow-$$"
trap 'rm -rf "$BUILD_DIR"' EXIT
mkdir -p "$BUILD_DIR"
CC_BIN=${CC:-gcc}
CFLAGS="-std=c99 -Wall -Wextra -Werror -pedantic -I$ROOT_DIR/firmware/main -I$ROOT_DIR/firmware/components/runtime_core/include -I$ROOT_DIR/firmware/components/glucose_core/include -I$ROOT_DIR/firmware/components/ui_screens/include"
$CC_BIN $CFLAGS \
  "$ROOT_DIR/firmware/main/test/test_mvp_http_flow.c" \
  "$ROOT_DIR/firmware/main/mvp_http_flow.c" \
  "$ROOT_DIR/firmware/components/runtime_core/http_exchange.c" \
  "$ROOT_DIR/firmware/components/runtime_core/ui_snapshot.c" \
  "$ROOT_DIR/firmware/components/glucose_core"/*.c -lm -o "$BUILD_DIR/test"
"$BUILD_DIR/test"
$CC_BIN $CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
  "$ROOT_DIR/firmware/main/test/test_mvp_http_flow.c" \
  "$ROOT_DIR/firmware/main/mvp_http_flow.c" \
  "$ROOT_DIR/firmware/components/runtime_core/http_exchange.c" \
  "$ROOT_DIR/firmware/components/runtime_core/ui_snapshot.c" \
  "$ROOT_DIR/firmware/components/glucose_core"/*.c -lm -o "$BUILD_DIR/test-sanitized"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$BUILD_DIR/test-sanitized"
echo "mvp http flow strict and sanitizer tests passed"
