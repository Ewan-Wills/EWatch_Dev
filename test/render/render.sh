#!/bin/bash
# Render every Claude Face state to PNG using the real Arduino_GFX sources and
# the real firmware drawing module, in LANDSCAPE: viewer PNGs (280x240) plus
# one native-panel PNG, with the rotation / region checks.
# Output: test/render/out/*.png (git-ignored)
#   ./render.sh            (from anywhere)
# Test hooks (used by test/claude/run.sh to prove the checks can fail):
#   CF_DRAW_SRC=<file>     draw module to build instead of the real one
#   CF_EXTRA_FLAGS=<flags> extra compiler flags (e.g. -DCF_TEST_BREAK_RECT)
#   CF_RENDER_OUT=<dir>    output directory (default: out/, wiped first)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"                    # ClaudeFace/firmware/
G="$ROOT/.pio/libdeps/ewatch/GFX Library for Arduino/src"
[ -d "$G" ] || { echo "missing $G (run pio run / pio pkg install once to fetch libdeps)"; exit 2; }
BIN="${TMPDIR:-/tmp}/cf_render_claude"
OUTDIR="${CF_RENDER_OUT:-$HERE/out}"
DRAW="${CF_DRAW_SRC:-$ROOT/src/claude_face_draw.cpp}"
mkdir -p "$OUTDIR"
rm -f "$OUTDIR/"*.png
xcrun clang++ -std=c++14 -O1 -Wall -Wno-unused-function ${CF_EXTRA_FLAGS:-} \
  -I"$HERE/stub" -I"$G" -I"$ROOT/src" \
  "$G/Arduino_GFX.cpp" "$G/Arduino_G.cpp" "$G/canvas/Arduino_Canvas.cpp" "$HERE/png.cpp" \
  "$DRAW" "$HERE/render_claude.cpp" \
  -lz -o "$BIN"
cd "$HERE" && "$BIN" "$OUTDIR"
