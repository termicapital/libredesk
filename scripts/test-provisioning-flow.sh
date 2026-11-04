#!/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/libre-desk-provisioning-flow.XXXXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
requested_compiler=${CC:-gcc}
command -v "$requested_compiler" >/dev/null 2>&1 || {
    echo "requested compiler is unavailable: $requested_compiler" >&2
    exit 127
}
compilers="$requested_compiler"
for candidate in gcc clang; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    case " $compilers " in *" $candidate "*) ;; *) compilers="$compilers $candidate";; esac
done
includes="-I$ROOT_DIR/firmware/main -I$ROOT_DIR/firmware/components/runtime_core/include -I$ROOT_DIR/firmware/components/glucose_core/include"
flags="-std=c99 -Wall -Wextra -Werror -pedantic"
runtime_config="$ROOT_DIR/firmware/components/runtime_core/runtime_config.c"
wifi_core="$ROOT_DIR/firmware/components/runtime_core/wifi_provisioning_core.c"

compile_and_run() {
    compiler=$1
    suffix=$2
    extra=$3
    sanitizer_env=$4
    boot="$BUILD_DIR/${compiler##*/}-boot-$suffix"
    session="$BUILD_DIR/${compiler##*/}-session-$suffix"
    "$compiler" $flags $extra $includes \
        "$ROOT_DIR/firmware/main/test/test_app_boot_flow.c" \
        "$ROOT_DIR/firmware/main/app_boot_flow.c" "$runtime_config" \
        -o "$boot"
    "$compiler" $flags $extra $includes \
        "$ROOT_DIR/firmware/main/test/test_provisioning_session.c" \
        "$ROOT_DIR/firmware/main/provisioning_session.c" \
        "$wifi_core" "$runtime_config" -o "$session"
    if test "$sanitizer_env" = yes; then
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$boot" >/dev/null
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$session" >/dev/null
    else
        "$boot" >/dev/null
        "$session" >/dev/null
    fi
}

for compiler in $compilers; do
    compile_and_run "$compiler" strict "" no
    compile_and_run "$compiler" signed "-fsigned-char" no
    compile_and_run "$compiler" unsigned "-funsigned-char" no
    compile_and_run "$compiler" sanitizer \
        "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" yes
done
python3 "$ROOT_DIR/firmware/main/test/test_provisioning_security.py" -v
printf 'provisioning boot/session compiler, sanitizer, signedness, and security-policy matrix passed\n'
