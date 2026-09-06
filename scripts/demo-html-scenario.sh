#!/usr/bin/env bash

set -euo pipefail

demo_url=${1:?usage: demo-html-scenario.sh URL TOKEN}
demo_token=${2:?usage: demo-html-scenario.sh URL TOKEN}
demo_delay=${MUSTERMARK_DEMO_DELAY:-1.4}

state() {
  curl -fsS "${demo_url%/}/api/state"
}

revision() {
  state | jq -r .revision
}

node_for_text() {
  local text=$1
  state | jq -r --arg text "$text" '.nodes[] | select(.text == $text) | (.id // .ref)' | head -1
}

send_instruction() {
  local payload=$1
  printf '\n→ %s\n' "$payload"
  curl -fsS -X POST "${demo_url%/}/api/actions" \
    -H 'Content-Type: application/json' \
    -H "X-Mustermark-Token: $demo_token" \
    --data "$payload" |
    jq '{ok, revision, error}'
  sleep "$demo_delay"
}

send_instruction "$(jq -nc --arg revision "$(revision)" \
  '{action:"track",baseRevision:$revision}')"

checklist=$(node_for_text 'and a checklist')
group=$(node_for_text 'with 2 items')
printf '\nTracked identities:\n  checklist  %s\n  group      %s\n' "$checklist" "$group"

assert_tracked_items() {
  state | jq -e --arg checklist "$checklist" --arg group "$group" \
    '([.nodes[] | select(.id == $checklist)] | length == 1) and
     ([.nodes[] | select(.id == $group)] | length == 1)' >/dev/null
  printf '  identity check: both tracked items retained their IDs\n'
}

send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$checklist" \
  '{action:"task_set",node:$node,checked:true,baseRevision:$revision}')"
assert_tracked_items
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$checklist" \
  '{action:"label_add",node:$node,label:"demo/done",baseRevision:$revision}')"
assert_tracked_items

send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$group" \
  '{action:"label_add",node:$node,label:"demo/group",baseRevision:$revision}')"
assert_tracked_items

send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$checklist" --arg target "$group" \
  '{action:"move_after",node:$node,target:$target,baseRevision:$revision}')"
assert_tracked_items
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$checklist" --arg target "$group" \
  '{action:"move_before",node:$node,target:$target,baseRevision:$revision}')"
assert_tracked_items

title=$(node_for_text 'just a title')
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$title" \
  '{action:"demote",node:$node,scope:"subtree",baseRevision:$revision}')"
title=$(node_for_text 'just a title')
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$title" \
  '{action:"promote",node:$node,scope:"subtree",baseRevision:$revision}')"

subtitle=$(node_for_text 'and a subtitle')
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$subtitle" \
  '{action:"demote",node:$node,scope:"self",baseRevision:$revision}')"
subtitle=$(node_for_text 'and a subtitle')
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$subtitle" \
  '{action:"promote",node:$node,scope:"self",baseRevision:$revision}')"

send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$group" \
  '{action:"demote",node:$node,scope:"subtree",baseRevision:$revision}')"
assert_tracked_items
send_instruction "$(jq -nc --arg revision "$(revision)" --arg node "$group" \
  '{action:"promote",node:$node,scope:"subtree",baseRevision:$revision}')"
assert_tracked_items

printf '\nScenario complete. The server remains at %s\n' "$demo_url"
