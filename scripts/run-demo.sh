#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${MUSTERMARK_DEMO_BUILD_DIR:-/tmp/mustermark-demo-build}
mode=${1:-}
if [[ -n $mode && $mode != --smoke && $mode != --autoplay ]]; then
  echo 'Usage: scripts/run-demo.sh [--smoke|--autoplay]' >&2
  exit 64
fi
cmake -S "$repo" -B "$build_dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release > /tmp/mustermark-demo-configure.log
cmake --build "$build_dir" --target mustermark_demo -j2 > /tmp/mustermark-demo-compile.log
if [[ $mode == --smoke ]]; then
  exec env QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=generic QT_QUICK_BACKEND=software \
    QT_STYLE_OVERRIDE=Basic "$build_dir/mustermark_demo" --fast --autoplay --windowed
fi
hyprctl dispatch 'hl.dsp.focus({ workspace = "2" })' >/dev/null
if [[ $mode == --autoplay ]]; then
  exec "$build_dir/mustermark_demo" --autoplay
fi
exec "$build_dir/mustermark_demo"
