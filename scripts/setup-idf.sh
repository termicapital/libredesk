#!/usr/bin/env bash
set -euo pipefail

IDF_VERSION="v5.5.5"
IDF_SERIES="5.5"
IDF_ROOT="${IDF_ROOT:-$HOME/.local/share/esp-idf-v5.5.5}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
PYTHON_MINOR="$($PYTHON_BIN -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$IDF_TOOLS_PATH/python_env/idf${IDF_SERIES}_py${PYTHON_MINOR}_env}"

if [[ ! -d "$IDF_ROOT/.git" ]]; then
  git clone --branch "$IDF_VERSION" --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_ROOT"
fi

git -C "$IDF_ROOT" submodule update --init --recursive --depth 1
actual_version="$(git -C "$IDF_ROOT" describe --tags --always --dirty)"
if [[ "$actual_version" != "$IDF_VERSION" ]]; then
  printf 'Refusing to use %s: expected %s, found %s\n' "$IDF_ROOT" "$IDF_VERSION" "$actual_version" >&2
  exit 1
fi

export IDF_PATH="$IDF_ROOT" IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH
"$PYTHON_BIN" "$IDF_PATH/tools/idf_tools.py" install --targets=esp32c6

if [[ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
  command -v uv >/dev/null 2>&1 || {
    printf 'uv is required for no-root Python environment creation on this host.\n' >&2
    exit 1
  }
  uv venv --clear --seed --python "$PYTHON_BIN" "$IDF_PYTHON_ENV_PATH"
  "$PYTHON_BIN" -c 'from pathlib import Path; import sys; Path(sys.argv[1]).write_text("5.5", encoding="utf-8")' \
    "$IDF_PYTHON_ENV_PATH/idf_version.txt"
fi

"$PYTHON_BIN" "$IDF_PATH/tools/idf_tools.py" install-python-env --features=core
"$PYTHON_BIN" "$IDF_PATH/tools/idf_tools.py" install cmake ninja
"$PYTHON_BIN" "$IDF_PATH/tools/idf_tools.py" check

printf 'ESP-IDF %s is ready. Activate it with:\n' "$IDF_VERSION"
printf '  export IDF_PATH=%q IDF_TOOLS_PATH=%q IDF_PYTHON_ENV_PATH=%q\n' \
  "$IDF_PATH" "$IDF_TOOLS_PATH" "$IDF_PYTHON_ENV_PATH"
printf '  . "$IDF_PATH/export.sh"\n'
