#!/usr/bin/env bash

set -euo pipefail

mustermark_binary=${1:?mustermark binary is required}
source_root=${2:?source root is required}
test_root=$(mktemp -d)
app_pid=
cleanup() {
  local status=$?
  if [[ -n $app_pid ]]; then
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  if (( status != 0 )) && [[ -s $test_root/server.err ]]; then
    printf 'Mustermark server log:\n' >&2
    sed -n '1,160p' "$test_root/server.err" >&2
  fi
  rm -rf "$test_root"
  return "$status"
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

printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=' \
  | base64 -d > "$test_root/visual.png"
revision=$(jq -r .revision <<<"$state")
item_query=$(jq -rn --arg value "$item" '$value | @uri')
revision_query=$(jq -rn --arg value "$revision" '$value | @uri')
attachment_response=$(curl -sS -w $'\n%{http_code}' -X POST \
  "${server_url}api/attachments?node=${item_query}&baseRevision=${revision_query}&filename=visual.png" \
  -H 'Content-Type: image/png' \
  -H "X-Mustermark-Token: $server_token" \
  --data-binary "@$test_root/visual.png")
attachment_status=${attachment_response##*$'\n'}
state=${attachment_response%$'\n'*}
if [[ $attachment_status != 200 ]]; then
  printf 'attachment upload returned HTTP %s: %s\n' "$attachment_status" "$state" >&2
  exit 1
fi
jq -e --arg node "$item" \
  '.ok and ((.nodes[] | select(.id == $node) | .attachments) | length == 1)' <<<"$state" >/dev/null
relative_image=$(jq -r --arg node "$item" \
  '.nodes[] | select(.id == $node) | .attachments[0].path' <<<"$state")
[[ -f "$test_root/$relative_image" ]]
grep -Fq '![visual]' "$test_root/document.md"
curl -fsS "${server_url}api/state" | jq -e \
  '.instructions[0].instruction.action == "attachment_upload" and .instructions[0].ok' >/dev/null

revision=$(jq -r .revision <<<"$state")
state=$(curl -fsS -X POST "${server_url}api/actions" \
  -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg revision "$revision" \
    '{action:"undo",origin:"ui",baseRevision:$revision}')")
jq -e --arg node "$item" \
  '.ok and ((.nodes[] | select(.id == $node) | .attachments) | length == 0)' <<<"$state" >/dev/null
[[ ! -f "$test_root/$relative_image" ]]
! grep -Fq '![visual]' "$test_root/document.md"

revision=$(jq -r .revision <<<"$state")
state=$(curl -fsS -X POST "${server_url}api/actions" \
  -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg revision "$revision" \
    '{action:"undo",origin:"ui",baseRevision:$revision}')")
jq -e --arg node "$item" \
  '.ok and ((.nodes[] | select(.id == $node) | .checked) | not)' <<<"$state" >/dev/null
grep -Fq -- '- [ ] and a checklist' "$test_root/document.md"

revision=$(jq -r .revision <<<"$state")
undo_status=$(curl -sS -o "$test_root/undo.json" -w '%{http_code}' -X POST \
  "${server_url}api/actions" -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg revision "$revision" \
    '{action:"undo",origin:"ui",baseRevision:$revision}')")
[[ $undo_status == 400 ]]
jq -e '.error.code == "nothing_to_undo" and (.error.message | length > 0)' \
  "$test_root/undo.json" >/dev/null
curl -fsS "${server_url}api/state" | jq -e \
  '.instructions[0].instruction.action == "undo" and
   (.instructions[0].ok | not) and
   .instructions[0].error.code == "nothing_to_undo"' >/dev/null
