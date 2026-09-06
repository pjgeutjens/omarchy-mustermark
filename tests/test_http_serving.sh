#!/usr/bin/env bash

set -euo pipefail

mustermark_binary=${1:?mustermark binary is required}
source_root=${2:?source root is required}
test_root=$(mktemp -d)
server_pid=
cleanup() {
  if [[ -n $server_pid ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

cp "$source_root/examples/html-serving-demo.md" "$test_root/document.md"
cp "$test_root/document.md" "$test_root/pristine.md"
"$mustermark_binary" serve "$test_root/document.md" --port=0 \
  --html-style="$source_root/examples/html-theme.css" \
  >"$test_root/server.json" 2>"$test_root/server.err" &
server_pid=$!

for _ in {1..100}; do
  [[ -s "$test_root/server.json" ]] && break
  sleep 0.02
done
[[ -s "$test_root/server.json" ]]
server_url=$(jq -r .url "$test_root/server.json")
server_token=$(jq -r .token "$test_root/server.json")

curl -fsS "$server_url" | grep -Fq 'API instructions'
curl -fsS "$server_url" | grep -Fq 'hide instructions'
curl -fsS "$server_url" | grep -Fq 'draggable=state.tracked'
curl -fsS "$server_url" | grep -Fq 'span.className="mm-task-text"'
curl -fsS "$server_url" | grep -Fq 'text-decoration:line-through'
style=$(curl -fsS "${server_url}style.css")
grep -Fq -- '--bg:' <<<"$style"
grep -Fq -- '--accent: #cfafbd' <<<"$style"
grep -Fq -- 'border-color: #9fc5a5' <<<"$style"
grep -Fq -- 'color: #d96b6b' <<<"$style"
grep -Fq -- 'text-decoration: none' <<<"$style"
state=$(curl -fsS "${server_url}api/state")
revision=$(jq -r .revision <<<"$state")

post() {
  curl -fsS -X POST "${server_url}api/actions" \
    -H 'Content-Type: application/json' \
    -H "X-Mustermark-Token: $server_token" \
    --data "$1"
}

state=$(post "$(jq -nc --arg revision "$revision" \
  '{action:"tracking_start",baseRevision:$revision}')")
jq -e '.ok and .tracked and
       (.source | contains("# just a title")) and
       ([.nodes[] | select(.kind == "item" and (.id | startswith("s:")))] | length == 6) and
       ([.nodes[] | select(.kind == "item" and (.fingerprint | startswith("mm1:")))] | length == 6)' \
  <<<"$state" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"
! grep -Fq 'mustermark:' "$test_root/document.md"

revision=$(jq -r .revision <<<"$state")
checklist=$(jq -r '.nodes[] | select(.text == "and a checklist") | .id' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$checklist" \
  '{action:"task_set",node:$node,checked:true,baseRevision:$revision}')")
jq -e --arg node "$checklist" '.ok and (.nodes[] | select(.id == $node) | .checked)' \
  <<<"$state" >/dev/null
curl -fsS "${server_url}api/view" | grep -Fq 'data-mm-checked="true"'

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$checklist" \
  '{action:"label_add",node:$node,label:"demo/done",baseRevision:$revision}')")
jq -e --arg node "$checklist" \
  '.ok and (.nodes[] | select(.id == $node) | .labels == ["demo/done"])' <<<"$state" >/dev/null

curl -fsS "${server_url}api/view" | grep -Fq "data-mm-id=\"$checklist\""
curl -fsS "${server_url}api/view" | grep -Fq 'data-mm-labels="demo/done"'
curl -fsS "${server_url}api/view" | grep -Fq 'data-mm-level="1"'
curl -fsS "${server_url}api/state" | jq -e '.instructions | length == 3' >/dev/null
! grep -Fq 'demo/done' "$test_root/document.md"
! grep -Fq 'mustermark:' "$test_root/document.md"

revision=$(jq -r .revision <<<"$state")
title=$(jq -r '.nodes[] | select(.text == "just a title") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$title" \
  '{action:"demote",node:$node,scope:"subtree",baseRevision:$revision}')")
jq -e '([.nodes[] | select(.text == "just a title") | .level] == [2]) and
       ([.nodes[] | select(.text == "and a subtitle") | .level] == [3])' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
title=$(jq -r '.nodes[] | select(.text == "just a title") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$title" \
  '{action:"promote",node:$node,scope:"subtree",baseRevision:$revision}')")
jq -e '([.nodes[] | select(.text == "just a title") | .level] == [1]) and
       ([.nodes[] | select(.text == "and a subtitle") | .level] == [2])' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
subtitle=$(jq -r '.nodes[] | select(.text == "and a subtitle") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$subtitle" \
  '{action:"demote",node:$node,scope:"self",baseRevision:$revision}')")
jq -e '([.nodes[] | select(.text == "just a title") | .level] == [1]) and
       ([.nodes[] | select(.text == "and a subtitle") | .level] == [3])' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
subtitle=$(jq -r '.nodes[] | select(.text == "and a subtitle") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$subtitle" \
  '{action:"promote",node:$node,scope:"self",baseRevision:$revision}')")

revision=$(jq -r .revision <<<"$state")
group=$(jq -r '.nodes[] | select(.text == "with 2 items") | .id' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$group" \
  '{action:"demote",node:$node,scope:"subtree",baseRevision:$revision}')")
jq -e --arg node "$group" \
  '(.diagnostics == []) and (.nodes[] | select(.id == $node) | .depth == 4)' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$group" \
  '{action:"promote",node:$node,scope:"subtree",baseRevision:$revision}')")
jq -e --arg node "$group" \
  '(.diagnostics == []) and (.nodes[] | select(.id == $node) | .depth == 2)' <<<"$state" >/dev/null

stale_status=$(curl -sS -o "$test_root/stale.json" -w '%{http_code}' -X POST \
  "${server_url}api/actions" -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg node "$checklist" \
    '{action:"task_set",node:$node,checked:false,baseRevision:"sha256:stale"}')")
[[ $stale_status == 409 ]]
jq -e '.error.code == "stale_revision"' "$test_root/stale.json" >/dev/null

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" \
  '{action:"tracking_stop",baseRevision:$revision}')")
jq -e '.ok and (.tracked | not) and
       ([.nodes[] | select(.id != null)] | length == 0) and
       ([.nodes[] | select(.labels | length > 0)] | length == 0)' <<<"$state" >/dev/null
! grep -Fq 'mustermark:' "$test_root/document.md"
