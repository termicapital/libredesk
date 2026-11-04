#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cc_bin="${CC:-gcc}"
out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

if ! command -v "$cc_bin" >/dev/null 2>&1; then
  printf 'requested compiler is unavailable: %s\n' "$cc_bin" >&2
  exit 127
fi

common=(
  -std=c99 -Wall -Wextra -Werror -pedantic
  -I"$repo_root/firmware/main"
  "$repo_root/firmware/main/display_port_flow.c"
  "$repo_root/firmware/main/test_display_port_flow.c"
  -o "$out_dir/display_port_flow"
)
"$cc_bin" "${common[@]}"
"$out_dir/display_port_flow"

"$cc_bin" -std=c99 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  -I"$repo_root/firmware/main" \
  "$repo_root/firmware/main/display_port_flow.c" \
  "$repo_root/firmware/main/test_display_port_flow.c" \
  -o "$out_dir/display_port_flow-sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/display_port_flow-sanitized"

"$cc_bin" -std=c99 -Wall -Wextra -Werror -pedantic -funsigned-char \
  -I"$repo_root/firmware/main" \
  "$repo_root/firmware/main/display_port_flow.c" \
  "$repo_root/firmware/main/test_display_port_flow.c" \
  -o "$out_dir/display_port_flow-unsigned"
"$out_dir/display_port_flow-unsigned"

printf 'display port flow matrix passed\n'
