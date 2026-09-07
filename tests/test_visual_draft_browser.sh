#!/usr/bin/env bash
set -euo pipefail

binary=$1
source_root=$2
test_root=$(mktemp -d)
server_pid=
browser_pid=
cleanup() {
  [[ -z $browser_pid ]] || kill "$browser_pid" 2>/dev/null || true
  [[ -z $server_pid ]] || kill "$server_pid" 2>/dev/null || true
  [[ -z $browser_pid ]] || wait "$browser_pid" 2>/dev/null || true
  [[ -z $server_pid ]] || wait "$server_pid" 2>/dev/null || true
  for _ in {1..3}; do
    rm -rf "$test_root" 2>/dev/null && break
    sleep 0.05
  done
}
trap cleanup EXIT

cp "$source_root/examples/html-serving-demo.md" "$test_root/document.md"
"$binary" serve "$test_root/document.md" --port=0 >"$test_root/server.json" 2>"$test_root/server.err" &
server_pid=$!
for _ in {1..100}; do
  [[ -s $test_root/server.json ]] && break
  sleep 0.02
done
server_url=$(jq -r .url "$test_root/server.json")
server_token=$(jq -r .token "$test_root/server.json")

chromium --headless=new --no-sandbox --disable-gpu --remote-debugging-port=0 \
  --user-data-dir="$test_root/chromium" "$server_url" >"$test_root/chromium.log" 2>&1 &
browser_pid=$!
for _ in {1..200}; do
  [[ -s $test_root/chromium/DevToolsActivePort ]] && break
  sleep 0.02
done
[[ -s $test_root/chromium/DevToolsActivePort ]]
debug_port=$(sed -n '1p' "$test_root/chromium/DevToolsActivePort")

node "$source_root/tests/visual_draft_browser.mjs" \
  "http://127.0.0.1:$debug_port" "$server_url" "$server_token"
