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

one_shot_ref=$("$mustermark_binary" inspect "$test_root/document.md" | jq -r \
  '.nodes[] | select(.text == "and a checklist") | .sourceRef')
one_shot_snapshot=$("$mustermark_binary" snapshot "$test_root/document.md" \
  "$one_shot_ref" --scope=item)
jq -e '.ok and .snapshotVersion == 1 and .units[0].text == "and a checklist"' \
  <<<"$one_shot_snapshot" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"

coproc MUSTERMARK_API { "$mustermark_binary" api --stdio; }
server_pid=$MUSTERMARK_API_PID

request() {
  local payload=$1
  printf '%s\n' "$payload" >&"${MUSTERMARK_API[1]}"
  IFS= read -r response <&"${MUSTERMARK_API[0]}"
  printf '%s\n' "$response"
}

state=$(request "$(jq -nc --arg path "$test_root/document.md" \
  '{jsonrpc:"2.0",id:1,method:"document.inspect",params:{path:$path}}')")
jq -e '.result.ok and .result.tracked' <<<"$state" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"

revision=$(jq -r .result.revision <<<"$state")
item=$(jq -r '.result.nodes[] | select(.text == "and a checklist") | .id' <<<"$state")
jq -e '.result.externalIdBindingVersion == 1 and .result.identityScope == "session" and
       .result.sectionSchemaVersion == 1 and (.result.sections | length > 0)' <<<"$state" >/dev/null
binding_request() {
  request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$1" \
    --arg node "$2" --arg method "$3" \
    '{jsonrpc:"2.0",id:20,method:$method,params:{path:$path,baseRevision:$revision,
      namespace:"feed-the-flock",value:"task-1",node:$node}}')"
}
binding=$(binding_request "$revision" "$item" document.external_id.bind)
jq -e --arg node "$item" '.result.ok and .result.node == $node' <<<"$binding" >/dev/null
binding=$(binding_request stale "$item" document.external_id.bind)
jq -e '.result.error.code == "stale_revision"' <<<"$binding" >/dev/null
other=$(jq -r --arg item "$item" '.result.nodes[] | select(.id != $item) | .id' <<<"$state" | head -1)
binding=$(binding_request "$revision" "$other" document.external_id.bind)
jq -e '.result.error.code == "external_id_collision"' <<<"$binding" >/dev/null
binding=$(binding_request "$revision" "" document.external_id.resolve)
jq -e --arg node "$item" '.result.ok and .result.node == $node' <<<"$binding" >/dev/null
cmp "$test_root/pristine.md" "$test_root/document.md"
snapshot=$(request "$(jq -nc --arg path "$test_root/document.md" --arg node "$item" \
  '{jsonrpc:"2.0",id:10,method:"document.snapshot",params:{path:$path,node:$node,scope:"item"}}')")
jq -e '.result.ok and .result.snapshotVersion == 1 and .result.scope == "item" and
       (.result.units | length) == 1 and .result.units[0].text == "and a checklist"' \
  <<<"$snapshot" >/dev/null
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

binding=$(binding_request "$(jq -r .result.revision <<<"$state")" "" document.external_id.resolve)
jq -e --arg node "$item" '.result.ok and .result.node == $node' <<<"$binding" >/dev/null

revision=$(jq -r .result.revision <<<"$state")
state=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  '{jsonrpc:"2.0",id:4,method:"document.tracking_stop",params:{path:$path,baseRevision:$revision}}')")
jq -e --arg node "$item" '.result.ok and .result.tracked and
       (.result.nodes[] | select(.id == $node) | .labels == ["demo/stdio"])' \
  <<<"$state" >/dev/null
! grep -Fq 'mustermark:' "$test_root/document.md"

state=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  --arg node "$item" \
  '{jsonrpc:"2.0",id:30,method:"document.apply",params:{path:$path,baseRevision:$revision,node:$node,action:"delete"}}')")
jq -e '.result.ok' <<<"$state" >/dev/null
revision=$(jq -r .result.revision <<<"$state")
binding=$(binding_request "$revision" "" document.external_id.resolve)
jq -e '.result.error.code == "external_id_retired"' <<<"$binding" >/dev/null

# Identical items keep separate identities across repeated disk reads in one API process.
printf '# Duplicates\n\n- same\n- same\n' > "$test_root/document.md"
state=$(request "$(jq -nc --arg path "$test_root/document.md" \
  '{jsonrpc:"2.0",id:31,method:"document.inspect",params:{path:$path}}')")
revision=$(jq -r .result.revision <<<"$state")
item=$(jq -r '.result.sections[0].items[0]' <<<"$state")
binding=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  --arg node "$item" '{jsonrpc:"2.0",id:32,method:"document.external_id.bind",
  params:{path:$path,baseRevision:$revision,node:$node,namespace:"test",value:"duplicate"}}')")
jq -e --arg node "$item" '.result.ok and .result.node == $node' <<<"$binding" >/dev/null
binding=$(request "$(jq -nc --arg path "$test_root/document.md" --arg revision "$revision" \
  '{jsonrpc:"2.0",id:33,method:"document.external_id.resolve",
  params:{path:$path,baseRevision:$revision,namespace:"test",value:"duplicate"}}')")
jq -e --arg node "$item" '.result.ok and .result.node == $node' <<<"$binding" >/dev/null
