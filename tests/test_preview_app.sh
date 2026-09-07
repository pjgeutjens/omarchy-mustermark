#!/usr/bin/env bash
set -euo pipefail

binary=$1
source_root=$2
workdir=$(mktemp -d)
preview_pid=""

cleanup() {
  if [[ -n "$preview_pid" ]]; then
    kill "$preview_pid" 2>/dev/null || true
    wait "$preview_pid" 2>/dev/null || true
  fi
  rm -rf "$workdir"
}
trap cleanup EXIT

first="$workdir/first.md"
second="$workdir/second.md"
printf '# First preview\n' > "$first"
printf '# Second preview\n' > "$second"

export QT_QPA_PLATFORM=offscreen
export QT_QPA_PLATFORMTHEME=generic
export QT_QUICK_BACKEND=software
export QT_STYLE_OVERRIDE=Basic
export XDG_DATA_HOME="$workdir/data"
export MUSTERMARK_PREVIEW_SERVER_NAME="mustermark-preview-test-$$"
export QTWEBENGINE_REMOTE_DEBUGGING=$((20000 + $$ % 20000))

"$binary" "$first" --serve=0 > "$workdir/preview.log" 2>&1 &
preview_pid=$!

for _ in $(seq 1 100); do
  grep -q '"preview":false' "$workdir/preview.log" && break
  kill -0 "$preview_pid" 2>/dev/null || {
    cat "$workdir/preview.log"
    exit 1
  }
  sleep 0.02
done
grep -q '"preview":false' "$workdir/preview.log"

# A Visual request reuses a normally opened editor process.
"$binary" --visual "$first" > "$workdir/normal-activate.log" 2>&1
grep -q '"activated":true' "$workdir/normal-activate.log"
kill -0 "$preview_pid"

"$binary" --visual "$second" > "$workdir/activate.log" 2>&1
grep -q '"activated":true' "$workdir/activate.log"
grep -q "$(basename "$second")" "$workdir/activate.log"
kill -0 "$preview_pid"

for _ in $(seq 1 100); do
  grep -q "$(basename "$second")" "$workdir/preview.log" && break
  sleep 0.02
done
grep -q "$(basename "$second")" "$workdir/preview.log"

cp "$second" "$workdir/before-append.md"
"$binary" append "$second" --section="Second preview" --kind=bullet \
  > "$workdir/append.log" 2>&1
grep -q '"activated":true' "$workdir/append.log"
kill -0 "$preview_pid"
cmp "$workdir/before-append.md" "$second"
node "$source_root/tests/preview_append_focus.mjs" \
  "http://127.0.0.1:$QTWEBENGINE_REMOTE_DEBUGGING" "Second preview"
