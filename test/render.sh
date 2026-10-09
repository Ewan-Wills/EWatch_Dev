#!/bin/bash
# Host checks for HeadFace (no watch needed):
#   1. frame timing over two whole loops (incl. the wrap), a slow decoder makes
#      the head wait instead of skipping, swipes land on the next layer's turn;
#   2. every frame decodes with the firmware's JPEGDEC path and matches
#      Pillow's decode of the same bytes;
#   3. the clock: a minute tick only touches the last digit's cell;
#   4. PNGs of clock-over-head frames in test/out/ for eyeballing.
# Every checker first runs against broken input to prove it can fail.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
J="$ROOT/.pio/libdeps/ewatch/JPEGDEC/src"
[ -d "$J" ] || { echo "run 'pio run' once to fetch libdeps"; exit 2; }
OUT="$HERE/out"; mkdir -p "$OUT"; rm -f "$OUT"/*
BIN="${TMPDIR:-/tmp}/headface_host"
xcrun clang++ -std=c++14 -O2 -w -I"$J" -I"$ROOT/src" \
  "$J/JPEGDEC.cpp" "$ROOT/src/head.cpp" "$ROOT/src/clock_draw.cpp" "$HERE/host_test.cpp" -o "$BIN"
PY=(uv run -q --with pillow --with numpy python3)
# Output is captured, never piped into grep -q (pipefail + SIGPIPE = false result).
echo "== control: timing checker must fail on a 1 ms wrong period"
CTL="$("$BIN" "$OUT" --control-timing || true)"
if [[ "$CTL" == *"frame timing wrong at"* ]]; then echo "ok, it failed"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
echo "== real run"
"$BIN" "$OUT"
echo "== control: decode checker must fail on a half-black frame"
CTL="$("${PY[@]}" "$HERE/check_frames.py" "$OUT" --control 2>&1 || true)"
if [[ "$CTL" == *" 1 bad [(200,"* ]]; then echo "ok, it failed on exactly the tampered frame"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
"${PY[@]}" "$HERE/check_frames.py" "$OUT"
rm -f "$OUT/decoded.bin"
echo "ALL HOST CHECKS PASSED"
