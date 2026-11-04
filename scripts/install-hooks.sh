#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
hook_dir="$repo_root/.git/hooks"

if [[ ! -d "$hook_dir" ]]; then
  printf 'Not a Git checkout: %s\n' "$repo_root" >&2
  exit 1
fi

gitleaks_bin="${GITLEAKS_BIN:-}"
if [[ -z "$gitleaks_bin" ]]; then
  gitleaks_bin=$(command -v gitleaks || true)
fi
if [[ -z "$gitleaks_bin" ]]; then
  printf 'gitleaks is required to install the pre-commit secret scan. Install it, then rerun make install-hooks.\n' >&2
  exit 1
fi

ln -sfn "$repo_root/scripts/pre-commit-gitleaks.sh" "$hook_dir/pre-commit"
printf 'Installed gitleaks pre-commit hook at %s\n' "$hook_dir/pre-commit"
