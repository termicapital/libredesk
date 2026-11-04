#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPONENT_DIR="$ROOT_DIR/firmware/components/runtime_core"
BASE_BUILD="${TMPDIR:-/tmp}/libre-desk-runtime-core-build-$$"
trap 'rm -rf "$BASE_BUILD"' EXIT
mkdir -p "$BASE_BUILD"

COMPILER=${CC:-gcc}
if ! command -v "$COMPILER" >/dev/null 2>&1; then
  echo "requested compiler is unavailable: $COMPILER" >&2
  exit 127
fi
COMPILERS=("$COMPILER")
for candidate in gcc clang; do
  if command -v "$candidate" >/dev/null 2>&1 && [[ "$candidate" != "$COMPILER" ]]; then
    COMPILERS+=("$candidate")
  fi
done

run_test_binary() {
  local binary="$1"; shift
  local output
  output=$("$binary" "$@" 2>&1) || {
    printf '%s\n' "$output" >&2
    return 1
  }
  if [[ -n "$output" ]]; then
    printf 'validator emitted output in silence mode:\n%s\n' "$output" >&2
    return 1
  fi
}

run_cmake_build() {
  local compiler="$1"; local name="$2"; shift 2
  local build="$BASE_BUILD/${compiler##*/}-$name"
  cmake -S "$COMPONENT_DIR" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$compiler" "$@"
  cmake --build "$build" --parallel
  ctest --test-dir "$build" --output-on-failure
  run_test_binary "$build/runtime_core_tests" --silence
  ASAN_OPTIONS=detect_leaks=0 "$build/ui_task_host_tests" >/dev/null
}

if command -v cmake >/dev/null 2>&1; then
  for compiler in "${COMPILERS[@]}"; do
    run_cmake_build "$compiler" production
    run_cmake_build "$compiler" host-only -DRUNTIME_CORE_HOST_TEST=ON
    run_cmake_build "$compiler" allow-only -DRUNTIME_ALLOW_IP_LITERAL=ON
    run_cmake_build "$compiler" doubly-guarded -DRUNTIME_CORE_HOST_TEST=ON -DRUNTIME_ALLOW_IP_LITERAL=ON
    run_cmake_build "$compiler" sanitizer -DRUNTIME_CORE_ENABLE_SANITIZERS=ON
    run_cmake_build "$compiler" signed-char -DCMAKE_C_FLAGS=-fsigned-char
    run_cmake_build "$compiler" unsigned-char -DCMAKE_C_FLAGS=-funsigned-char
  done
else
  CFLAGS=(-std=c99 -Wall -Wextra -Werror -pedantic -I"$COMPONENT_DIR/include" -I"$ROOT_DIR/firmware/components/glucose_core/include")
  for compiler in "${COMPILERS[@]}"; do
    compiler_tag=${compiler##*/}
    build_dir="$BASE_BUILD/$compiler_tag"
    mkdir -p "$build_dir"
    for variant in production host-only allow-only doubly-guarded; do
      defs=()
      case "$variant" in
        host-only) defs=(-DRUNTIME_CORE_HOST_TEST) ;;
        allow-only) defs=(-DRUNTIME_ALLOW_IP_LITERAL) ;;
        doubly-guarded) defs=(-DRUNTIME_CORE_HOST_TEST -DRUNTIME_ALLOW_IP_LITERAL) ;;
      esac
      binary="$build_dir/runtime-$variant"
      "$compiler" "${CFLAGS[@]}" "${defs[@]}" \
        "$COMPONENT_DIR/runtime_config.c" "$COMPONENT_DIR/http_exchange.c" \
        "$COMPONENT_DIR/ota_manifest.c" "$COMPONENT_DIR/ui_snapshot.c" \
        "$COMPONENT_DIR/wifi_provisioning_core.c" \
        "$ROOT_DIR/firmware/components/glucose_core"/*.c "$COMPONENT_DIR/test/test_runtime_core.c" -lm -o "$binary"
      "$binary"
      run_test_binary "$binary" --silence
    done
    "$compiler" "${CFLAGS[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
      -fno-sanitize-recover=all "$COMPONENT_DIR/runtime_config.c" \
      "$COMPONENT_DIR/http_exchange.c" "$COMPONENT_DIR/ota_manifest.c" \
      "$COMPONENT_DIR/ui_snapshot.c" "$COMPONENT_DIR/wifi_provisioning_core.c" \
      "$ROOT_DIR/firmware/components/glucose_core"/*.c \
      "$COMPONENT_DIR/test/test_runtime_core.c" -lm -o "$build_dir/runtime-sanitizer"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      "$build_dir/runtime-sanitizer"
    run_test_binary "$build_dir/runtime-sanitizer" --silence
    for signedness in signed unsigned; do
      flag=-fsigned-char
      [[ "$signedness" == unsigned ]] && flag=-funsigned-char
      binary="$build_dir/runtime-$signedness"
      "$compiler" "${CFLAGS[@]}" "$flag" "$COMPONENT_DIR/runtime_config.c" \
        "$COMPONENT_DIR/http_exchange.c" "$COMPONENT_DIR/ota_manifest.c" \
        "$COMPONENT_DIR/ui_snapshot.c" "$COMPONENT_DIR/wifi_provisioning_core.c" \
      "$ROOT_DIR/firmware/components/glucose_core"/*.c \
        "$COMPONENT_DIR/test/test_runtime_core.c" -lm -o "$binary"
      "$binary"
      run_test_binary "$binary" --silence
    done
    HOST_CFLAGS=("${CFLAGS[@]}" -I"$COMPONENT_DIR/test/fakes" -I"$ROOT_DIR/firmware/main" -I"$COMPONENT_DIR/test")
    host_binary="$build_dir/ui-task-host-tests"
    "$compiler" "${HOST_CFLAGS[@]}" "$COMPONENT_DIR/ui_snapshot.c" \
      "$ROOT_DIR/firmware/components/glucose_core"/*.c \
      "$COMPONENT_DIR/test/ui_task_host.c" "$COMPONENT_DIR/test/test_ui_task_host.c" \
      "$ROOT_DIR/firmware/main/ui_task.c" -lm -o "$host_binary"
    "$host_binary"
  done
fi

# Existing glucose_core suite, with a direct fallback for hosts without CMake.
if command -v cmake >/dev/null 2>&1; then
  "$ROOT_DIR/scripts/test-glucose-core.sh"
else
  GCORE="$ROOT_DIR/firmware/components/glucose_core"
  "$COMPILER" -std=c99 -Wall -Wextra -Werror -pedantic -I"$GCORE/include" \
    "$GCORE"/*.c "$GCORE/test/test_glucose_core.c" -lm -o "$BASE_BUILD/glucose-core-tests"
  (cd "$ROOT_DIR" && "$BASE_BUILD/glucose-core-tests")
fi
echo "runtime_core native/sanitizer/compiler/signedness and glucose_core test runs passed"
