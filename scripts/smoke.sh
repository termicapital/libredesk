#!/usr/bin/env bash
set -euo pipefail

base_url=${BASE_URL:?set BASE_URL to https://REPLACE_WITH_STABLE_HOSTNAME}
device_token=${DEVICE_TOKEN:?set DEVICE_TOKEN to the raw device token for this smoke test}

health=$(curl --fail --silent --show-error --max-time 10 "$base_url/healthz")
printf '%s' "$health" | node -e '
let input=""; process.stdin.on("data", c => input += c); process.stdin.on("end", () => {
  const value=JSON.parse(input);
  if (value.ok !== true || typeof value.upstream_ok !== "boolean") process.exit(1);
  console.log(`healthz ok; upstream_ok=${value.upstream_ok}`);
});'

check_latest() {
  local path=$1
  local expected_schema=$2
  local local_time_policy=$3
  local latest
  latest=$(printf 'header = "Authorization: Bearer %s"\n' "$device_token" \
    | curl --config - --fail --silent --show-error --max-time 10 "$base_url$path")
  printf '%s' "$latest" | node -e '
let input=""; process.stdin.on("data", c => input += c); process.stdin.on("end", () => {
  const expectedSchema=Number(process.argv[1]);
  const localTimePolicy=process.argv[2];
  const value=JSON.parse(input);
  if (value.schema !== expectedSchema || !["librelinkup","mock"].includes(value.source)) process.exit(1);
  if (typeof value.timestamp !== "string" || !Number.isInteger(value.age_s)) process.exit(1);
  const hasLocalTime=Object.hasOwn(value, "libre_local_time");
  if (localTimePolicy === "absent" && hasLocalTime) process.exit(1);
  if (localTimePolicy === "required" &&
      (!hasLocalTime || !/^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$/.test(value.libre_local_time))) process.exit(1);
  console.log(`latest contract ok; schema=${value.schema}; source=${value.source}`);
});' "$expected_schema" "$local_time_policy"
}

check_latest /api/v1/latest 1 absent
check_latest /api/v2/latest 2 required
