#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
python3 scripts/check-public-files.py

gitleaks_bin="${GITLEAKS_BIN:-}"
if [[ -z "$gitleaks_bin" ]]; then
  gitleaks_bin=$(command -v gitleaks || true)
fi
if [[ -z "$gitleaks_bin" ]]; then
  printf 'gitleaks is required for the pre-commit secret scan\n' >&2
  exit 1
fi

"$gitleaks_bin" git --pre-commit --staged --redact
