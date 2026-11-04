#!/usr/bin/env sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$repo_dir/firmware/main" \
  -I"$repo_dir/firmware/components/glucose_core/include" \
  "$repo_dir/firmware/main/test/test_wifi_startup_flow.c" \
  "$repo_dir/firmware/main/wifi_startup_flow.c" \
  -o "$build_dir/test_wifi_startup_flow"
"$build_dir/test_wifi_startup_flow"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$repo_dir/firmware/main/test/fakes_task7" \
  -I"$repo_dir/firmware/main" \
  "$repo_dir/firmware/main/test/test_wifi_setup_sequence.c" \
  "$repo_dir/firmware/main/wifi_setup_sequence.c" \
  -o "$build_dir/test_wifi_setup_sequence"
"$build_dir/test_wifi_setup_sequence"
