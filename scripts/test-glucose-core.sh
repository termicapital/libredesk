#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPONENT_DIR="$ROOT_DIR/firmware/components/glucose_core"
NATIVE_BUILD="$COMPONENT_DIR/.native-build"
SANITIZER_BUILD="$COMPONENT_DIR/.sanitizer-build"
COVERAGE_BUILD="$COMPONENT_DIR/.coverage-build"

python3 "$COMPONENT_DIR/test/test_check_coverage.py" -v

cmake --fresh -S "$COMPONENT_DIR" -B "$NATIVE_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$NATIVE_BUILD" --parallel
ctest --test-dir "$NATIVE_BUILD" --output-on-failure

cmake --fresh -S "$COMPONENT_DIR" -B "$SANITIZER_BUILD" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGLUCOSE_CORE_ENABLE_SANITIZERS=ON
cmake --build "$SANITIZER_BUILD" --parallel
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  ctest --test-dir "$SANITIZER_BUILD" --output-on-failure

cmake --fresh -S "$COMPONENT_DIR" -B "$COVERAGE_BUILD" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGLUCOSE_CORE_ENABLE_COVERAGE=ON
cmake --build "$COVERAGE_BUILD" --parallel
ctest --test-dir "$COVERAGE_BUILD" --output-on-failure
python3 "$COMPONENT_DIR/test/check_coverage.py" "$COVERAGE_BUILD" "$COMPONENT_DIR"

echo "glucose_core native, sanitizer, and coverage test runs passed"
