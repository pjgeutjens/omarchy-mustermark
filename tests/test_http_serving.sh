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
mkdir -p "$test_root/document.assets"
printf 'mustermark-image-fixture' > "$test_root/document.assets/pixel.png"
printf '\n- [ ] image fixture\n  ![pixel](document.assets/pixel.png)\n' >> "$test_root/document.md"
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

curl -fsS "$server_url" > "$test_root/page.html"
grep -Fq 'API instructions' "$test_root/page.html"
grep -Fq 'hide diagnostics' "$test_root/page.html"
grep -Fq 'controls.className="mm-controls"' "$test_root/page.html"
grep -Fq 'handle.draggable=true' "$test_root/page.html"
grep -Fq '.mm-hovered>.mm-controls' "$test_root/page.html"
grep -Fq 'positionControls(hovered,event)' "$test_root/page.html"
grep -Fq 'beginInlineItem(element,node,state)' "$test_root/page.html"
grep -Fq 'beginInlineSection(element,node)' "$test_root/page.html"
grep -Fq 'if(event.key==="Escape")' "$test_root/page.html"
grep -Fq 'function cancelTransientUi()' "$test_root/page.html"
grep -Fq 'attachment.classList.remove("mm-open")' "$test_root/page.html"
grep -Fq '<dialog id="mm-dialog">' "$test_root/page.html"
grep -Fq 'confirmRemoval(noun)' "$test_root/page.html"
grep -Fq 'isEmptyStructure(node)||skipRemovalConfirmations||await confirmRemoval(noun)' "$test_root/page.html"
grep -Fq '#mm-dialog [hidden]{display:none!important}' "$test_root/page.html"
grep -Fq 'mustermark.skipRemovalConfirmations' "$test_root/page.html"
grep -Fq "Don't ask again for removals in this preview" "$test_root/page.html"
! grep -Fq 'window.prompt' "$test_root/page.html"
! grep -Fq 'window.confirm' "$test_root/page.html"
grep -Fq 'action:"section_add"' "$test_root/page.html"
grep -Fq 'action:"list_add"' "$test_root/page.html"
grep -Fq 'action:"item_add"' "$test_root/page.html"
grep -Fq 'beginInlineEdit(element,node)' "$test_root/page.html"
grep -Fq 'action:"replace"' "$test_root/page.html"
grep -Fq '"Edit Markdown"' "$test_root/page.html"
grep -Fq 'resize:none;overflow:auto' "$test_root/page.html"
grep -Fq 'navigator.clipboard.writeText(markdown)' "$test_root/page.html"
grep -Fq 'new ClipboardItem({"image/png":blob})' "$test_root/page.html"
grep -Fq '"Attach image"' "$test_root/page.html"
grep -Fq 'document.addEventListener("paste"' "$test_root/page.html"
grep -Fq 'uploadImage(node,file)' "$test_root/page.html"
grep -Fq 'action:"undo",origin:"ui"' "$test_root/page.html"
grep -Fq 'span.className="mm-task-text"' "$test_root/page.html"
grep -Fq 'text-decoration:line-through' "$test_root/page.html"
grep -Fq 'className="entry-error"' "$test_root/page.html"
grep -Fq 'value.error?.message' "$test_root/page.html"
style=$(curl -fsS "${server_url}style.css")
grep -Fq -- '--bg:' <<<"$style"
grep -Fq -- '--accent: #cfafbd' <<<"$style"
grep -Fq -- 'border-color: #9fc5a5' <<<"$style"
grep -Fq -- 'color: #d96b6b' <<<"$style"
grep -Fq -- 'text-decoration: none' <<<"$style"
state=$(curl -fsS "${server_url}api/state")
jq -e 'any(.nodes[]; .kind == "item" and .text == "image fixture" and
       (.attachments | length) == 1 and .attachments[0].alt == "pixel" and
       .attachments[0].path == "document.assets/pixel.png")' \
  <<<"$state" >/dev/null
curl -fsS "${server_url}document.assets/pixel.png" > "$test_root/downloaded.png"
cmp "$test_root/document.assets/pixel.png" "$test_root/downloaded.png"
revision=$(jq -r .revision <<<"$state")

post() {
  curl -fsS -X POST "${server_url}api/actions" \
    -H 'Content-Type: application/json' \
    -H "X-Mustermark-Token: $server_token" \
    --data "$1"
}

jq -e '.ok and .tracked and
       (.source | contains("# just a title")) and
       ([.nodes[] | select(.kind == "item" and (.id | startswith("s:")))] | length == 7) and
       ([.nodes[] | select(.kind == "item" and (.fingerprint | startswith("mm1:")))] | length == 7)' \
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
state=$(post "$(jq -nc --arg revision "$revision" \
  '{action:"undo",origin:"ui",baseRevision:$revision}')")
jq -e --arg node "$checklist" '.ok and ((.nodes[] | select(.id == $node) | .checked) | not)' \
  <<<"$state" >/dev/null
grep -Fq -- '- [ ] and a checklist' "$test_root/document.md"

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$checklist" \
  '{action:"task_set",node:$node,checked:true,baseRevision:$revision}')")
jq -e --arg node "$checklist" '.ok and (.nodes[] | select(.id == $node) | .checked)' \
  <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$checklist" \
  '{action:"label_add",node:$node,label:"demo/done",baseRevision:$revision}')")
jq -e --arg node "$checklist" \
  '.ok and (.nodes[] | select(.id == $node) | .labels == ["demo/done"])' <<<"$state" >/dev/null

curl -fsS "${server_url}api/view" | grep -Fq "data-mm-id=\"$checklist\""
curl -fsS "${server_url}api/view" | grep -Fq 'data-mm-labels="demo/done"'
curl -fsS "${server_url}api/view" | grep -Fq 'data-mm-level="1"'
curl -fsS "${server_url}api/state" | jq -e '.instructions | length == 4' >/dev/null
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

revision=$(jq -r .revision <<<"$state")
title=$(jq -r '.nodes[] | select(.text == "just a title") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$title" \
  '{action:"section_add",node:$node,text:"API additions",baseRevision:$revision}')")
jq -e '.ok and
       ([.nodes[] | select(.kind == "heading" and .text == "API additions" and
                            (.id | startswith("s:")))] | length == 1)' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
additions=$(jq -r '.nodes[] | select(.kind == "heading" and .text == "API additions") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$additions" \
  '{action:"list_add",node:$node,text:"first API item",task:true,baseRevision:$revision}')")
jq -e '.ok and
       ([.nodes[] | select(.kind == "list" and (.id | startswith("s:")))] | length >= 3) and
       ([.nodes[] | select(.kind == "item" and .text == "first API item" and .task and
                            (.id | startswith("s:")))] | length == 1)' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
first_api_item=$(jq -r '.nodes[] | select(.kind == "item" and .text == "first API item") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$first_api_item" \
  '{action:"item_add",node:$node,text:"second API item",task:true,baseRevision:$revision}')")
jq -e '.ok and
       ([.nodes[] | select(.kind == "item" and .text == "second API item" and .task and
                            (.id | startswith("s:")))] | length == 1) and
       (.source | contains("- [ ] first API item\n- [ ] second API item"))' <<<"$state" >/dev/null

revision=$(jq -r .revision <<<"$state")
second_api_item=$(jq -r '.nodes[] | select(.kind == "item" and .text == "second API item") | .ref' <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" --arg node "$second_api_item" \
  --arg markdown $'- [ ] edited API item\n  continuation\n' \
  '{action:"replace",node:$node,markdown:$markdown,baseRevision:$revision}')")
jq -e '.ok and
       ([.nodes[] | select(.kind == "item" and .text == "edited API item")] | length == 1) and
       (.source | contains("- [ ] edited API item\n  continuation"))' <<<"$state" >/dev/null

stale_status=$(curl -sS -o "$test_root/stale.json" -w '%{http_code}' -X POST \
  "${server_url}api/actions" -H 'Content-Type: application/json' \
  -H "X-Mustermark-Token: $server_token" \
  --data "$(jq -nc --arg node "$checklist" \
    '{action:"task_set",node:$node,checked:false,baseRevision:"sha256:stale"}')")
[[ $stale_status == 409 ]]
jq -e '.error.code == "stale_revision"' "$test_root/stale.json" >/dev/null
curl -fsS "${server_url}api/state" | jq -e \
  '.instructions[0] | (.ok | not) and .error.code == "stale_revision" and
   (.error.message | length > 0)' >/dev/null

revision=$(jq -r .revision <<<"$state")
state=$(post "$(jq -nc --arg revision "$revision" \
  '{action:"tracking_stop",baseRevision:$revision}')")
jq -e '.ok and .tracked and
       ([.nodes[] | select(.id != null)] | length > 0)' <<<"$state" >/dev/null
! grep -Fq 'mustermark:' "$test_root/document.md"
