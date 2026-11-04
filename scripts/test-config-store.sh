#!/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
BASE_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/libre-desk-config-store.XXXXXXXX")
trap 'rm -rf "$BASE_BUILD"' EXIT
requested_compiler=${CC:-gcc}
report=false
if test "${1:-}" = "--report"; then report=true; fi
command -v "$requested_compiler" >/dev/null 2>&1 || { echo "requested compiler is unavailable: $requested_compiler" >&2; exit 127; }
compilers="$requested_compiler"
for candidate in gcc clang; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    case " $compilers " in *" $candidate "*) ;; *) compilers="$compilers $candidate";; esac
done
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -pthread -DTASK7_HOST_TEST -I$ROOT_DIR/firmware/main/test/fakes_task7 -I$ROOT_DIR/firmware/main/test -I$ROOT_DIR/firmware/main -I$ROOT_DIR/firmware/components/runtime_core/include -I$ROOT_DIR/firmware/components/glucose_core/include"
sources="$ROOT_DIR/firmware/main/test/config_store_host.c $ROOT_DIR/firmware/main/test/test_config_store.c $ROOT_DIR/firmware/main/config_store.c $ROOT_DIR/firmware/main/provisioning.c $ROOT_DIR/firmware/components/runtime_core/runtime_config.c"
for source in "$ROOT_DIR"/firmware/components/glucose_core/*.c; do sources="$sources $source"; done
run_silent() { binary=$1; output=$2; shift 2; if ! "$@" "$binary" --silence >"$output" 2>&1; then cat "$output" >&2; return 1; fi; test ! -s "$output"; }
for compiler in $compilers; do
    tag=${compiler##*/}
    for name in strict signed unsigned; do
        binary="$BASE_BUILD/$tag-$name"; output="$binary.out"; extra=""
        test "$name" = signed && extra="-fsigned-char"; test "$name" = unsigned && extra="-funsigned-char"
        "$compiler" $common_flags $extra $sources -lm -o "$binary"
        run_silent "$binary" "$output" env
    done
    binary="$BASE_BUILD/$tag-sanitizer"; output="$binary.out"
    "$compiler" $common_flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all $sources -lm -o "$binary"
    run_silent "$binary" "$output" env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
    a=$(mktemp -d "${TMPDIR:-/tmp}/config-repro-a.XXXXXXXX"); b=$(mktemp -d "${TMPDIR:-/tmp}/config-repro-b.XXXXXXXX")
    "$compiler" $common_flags $sources -lm -o "$a/config-store"; "$compiler" $common_flags $sources -lm -o "$b/config-store"
    run_silent "$a/config-store" "$a/output" env; run_silent "$b/config-store" "$b/output" env
    test "$(sha256sum "$a/config-store" | cut -d' ' -f1)" = "$(sha256sum "$b/config-store" | cut -d' ' -f1)"
    cmp -s "$a/output" "$b/output"; printf 'config-store %s reproducible output=empty\n' "$compiler"
    rm -rf "$a" "$b"
done
if $report; then "$BASE_BUILD/${requested_compiler##*/}-strict" --report; fi
printf 'config-store authentic C99/compiler/sanitizer/signedness/silence matrix passed\n'
