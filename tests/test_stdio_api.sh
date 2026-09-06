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

coproc MUSTERMARK_API { "$mustermark_binary" api --stdio; }
server_pid=$MUSTERMARK_API_PID

request() {
  local payload=$1
  printf '%s\n' "$payload" >&"${MUSTERMARK_API[1]}"
  IFS= read -r response <&"${MUSTERMARK_API[0]}"
  printf '%s\n' "$response"
}

state=$(request "$(jq -nc --arg path "$test_root/document.md" \
  '{jsonrpc:"2.0",id:1,method:"document.tracking_start",params:{path:$path}}')")
jq -e '.result.ok and .result.tracked' <<<"$state" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"

revision=$(jq -r .result.revision <<<"$state")
item=$(jq -r '.result.nodes[] | select(.text == "and a checklist") | .id' <<<"$state")
state=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  --arg node "$item" \
  '{jsonrpc:"2.0",id:2,method:"document.apply",params:{path:$path,action:"label_add",node:$node,label:"demo/stdio",baseRevision:$revision}}')")
jq -e --arg node "$item" \
  '.result.ok and (.result.nodes[] | select(.id == $node) | .labels == ["demo/stdio"])' \
  <<<"$state" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"

revision=$(jq -r .result.revision <<<"$state")
state=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  --arg node "$item" \
  '{jsonrpc:"2.0",id:3,method:"document.apply",params:{path:$path,action:"task_set",node:$node,checked:true,baseRevision:$revision}}')")
jq -e --arg node "$item" \
  '.result.ok and (.result.nodes[] | select(.id == $node) | .checked)' <<<"$state" >/dev/null
grep -Fq -- '- [x] and a checklist' "$test_root/document.md"
! grep -Fq 'mustermark:' "$test_root/document.md"

revision=$(jq -r .result.revision <<<"$state")
state=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  '{jsonrpc:"2.0",id:4,method:"document.tracking_stop",params:{path:$path,baseRevision:$revision}}')")
jq -e '.result.ok and (.result.tracked | not) and
       ([.result.nodes[] | select(.id != null)] | length == 0) and
       ([.result.nodes[] | select(.labels | length > 0)] | length == 0)' <<<"$state" >/dev/null
! grep -Fq 'mustermark:' "$test_root/document.md"
