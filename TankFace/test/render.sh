#!/bin/bash
# Host checks for TankFace (no watch needed):
#   1. hands: in 96 rendered times across 24 h, the minute and hour hands
#      point where the time says (measured from the pixels);
#   2. dial: all 60 minute ticks sit on their true minute rays, five-minute
#      marks are the wide ones;
#   3. PNGs in test/out/ for eyeballing (1_1010.png ...).
# Every checker first runs against broken input to prove it can fail.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
OUT="$HERE/out"; mkdir -p "$OUT"; rm -f "$OUT"/*
BIN="${TMPDIR:-/tmp}/tankface_host"
xcrun clang++ -std=c++14 -O2 -Wall -I"$ROOT/src" "$ROOT/src/tank_draw.cpp" "$HERE/host_test.cpp" -lz -o "$BIN"
PY=(uv run -q --with pillow --with numpy python3)
# Output is captured, never piped into grep -q (pipefail + SIGPIPE = false result).
echo "== control: hand checker must fail when the hour hand ignores the minutes"
"$BIN" "$OUT" --break-hour >/dev/null
CTL="$("${PY[@]}" "$HERE/check_hands.py" "$OUT" 2>&1 || true)"
if [[ "$CTL" == *"HANDS MISMATCH"* && "$CTL" == *"hour hand at"* ]]; then echo "ok, it failed"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
echo "== control: hand checker must fail when no hands are drawn"
rm -f "$OUT"/*; "$BIN" "$OUT" --no-hands >/dev/null
CTL="$("${PY[@]}" "$HERE/check_hands.py" "$OUT" 2>&1 || true)"
if [[ "$CTL" == *"HANDS MISMATCH"* && "$CTL" == *"no hands drawn"* ]]; then echo "ok, it failed"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
echo "== real run"
rm -f "$OUT"/*; "$BIN" "$OUT"
"${PY[@]}" "$HERE/check_hands.py" "$OUT"
echo "== control: dial checker must fail half a minute off"
CTL="$("${PY[@]}" "$HERE/check_dial.py" "$OUT" --control 2>&1 || true)"
if [[ "$CTL" == *"DIAL MISMATCH"* ]]; then echo "ok, it failed"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
"${PY[@]}" "$HERE/check_dial.py" "$OUT"
rm -f "$OUT"/t_*.png
echo "ALL HOST CHECKS PASSED"
