#!/usr/bin/env bash

set -euo pipefail

mustermark_binary=${1:?mustermark binary is required}
source_root=${2:?source root is required}
test_root=$(mktemp -d)
app_pid=
cleanup() {
  if [[ -n $app_pid ]]; then
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

cp "$source_root/examples/html-serving-demo.md" "$test_root/document.md"
env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=generic \
  QT_QUICK_BACKEND=software QT_STYLE_OVERRIDE=Basic \
  XDG_DATA_HOME="$test_root/data" \
  "$mustermark_binary" "$test_root/document.md" --serve=0 \
  --html-style="$source_root/examples/html-theme.css" \
  >"$test_root/server.json" 2>"$test_root/server.err" &
app_pid=$!

for _ in {1..100}; do
  [[ -s "$test_root/server.json" ]] && break
  sleep 0.02
done
[[ -s "$test_root/server.json" ]]
server_url=$(jq -r .url "$test_root/server.json")
server_token=$(jq -r .token "$test_root/server.json")

state=
for _ in {1..100}; do
  if state=$(curl -fsS "${server_url}api/state" 2>/dev/null); then
    break
  fi
  sleep 0.02
done
[[ -n $state ]]
revision=$(jq -r .revision <<<"$state")
state=$(curl -fsS -X POST "${server_url}api/actions" \
  -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg revision "$revision" \
    '{action:"tracking_start",baseRevision:$revision}')")
item=$(jq -r '.nodes[] | select(.text == "and a checklist") | .id' <<<"$state")
revision=$(jq -r .revision <<<"$state")

state=$(curl -fsS -X POST "${server_url}api/actions" \
  -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg revision "$revision" --arg node "$item" \
    '{action:"task_set",node:$node,checked:true,baseRevision:$revision}')")
jq -e --arg node "$item" \
  '.ok and .tracked and (.nodes[] | select(.id == $node) | .checked)' <<<"$state" >/dev/null
grep -Fq -- '- [x] and a checklist' "$test_root/document.md"
! grep -Fq 'mustermark:' "$test_root/document.md"
