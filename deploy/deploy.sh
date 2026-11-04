#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

env_file=${BRIDGE_ENV_FILE:-/etc/libre-desk/bridge.env}
if [[ ! -f "$env_file" ]]; then
  printf 'Missing bridge environment file: %s\n' "$env_file" >&2
  exit 1
fi
mode=$(stat -c '%a' "$env_file")
if [[ "$mode" != "600" && "$mode" != "400" ]]; then
  printf 'Bridge environment file must be mode 0600 or 0400; got %s\n' "$mode" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  printf 'Docker daemon is unavailable to the current user.\n' >&2
  exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
  printf 'Refusing deployment from a dirty working tree. Commit or stash changes first.\n' >&2
  exit 1
fi
if git remote get-url origin >/dev/null 2>&1; then
  git pull --ff-only
fi

compose=(docker compose -f deploy/docker-compose.yml)
"${compose[@]}" config -q
if [[ -z "$("${compose[@]}" ps -q caddy 2>/dev/null)" ]]; then
  python3 - <<'PY'
from pathlib import Path
busy=[]
for source in ('/proc/net/tcp','/proc/net/tcp6'):
    for row in Path(source).read_text().splitlines()[1:]:
        fields=row.split()
        if fields[3]=='0A':
            port=int(fields[1].split(':')[1],16)
            if port in (80,443): busy.append(port)
if busy:
    raise SystemExit(f"ports already in use: {sorted(set(busy))}")
PY
fi

"${compose[@]}" build --pull
"${compose[@]}" up -d
"${compose[@]}" ps

if [[ -n "${DEVICE_TOKEN:-}" ]]; then
  BASE_URL=${BASE_URL:-https://${LIBRE_HOST:-localhost}} DEVICE_TOKEN=$DEVICE_TOKEN scripts/smoke.sh
else
  printf 'Deployment started. Set DEVICE_TOKEN and BASE_URL to run scripts/smoke.sh.\n'
fi
