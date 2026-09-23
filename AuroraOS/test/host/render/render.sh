#!/bin/bash
# Render every Claude Face state to PNG using the real Arduino_GFX sources and
# the real firmware drawing module. Output: test/host/render/out/*.png
#   ./render.sh            (from anywhere)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"                 # AuroraOS/
G="$ROOT/.pio/libdeps/ewatch/GFX Library for Arduino/src"
[ -d "$G" ] || { echo "missing $G (run pio once to fetch libdeps)"; exit 2; }
BIN="${TMPDIR:-/tmp}/cf_render_claude"
mkdir -p "$HERE/out"
xcrun clang++ -std=c++14 -O1 -Wall -Wno-unused-function \
  -I"$HERE/stub" -I"$G" -I"$ROOT/src/apps/claude" \
  "$G/Arduino_GFX.cpp" "$G/Arduino_G.cpp" "$HERE/png.cpp" \
  "$ROOT/src/apps/claude/claude_face_draw.cpp" "$HERE/render_claude.cpp" \
  -lz -o "$BIN"
cd "$HERE" && "$BIN" out
