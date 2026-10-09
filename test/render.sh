#!/bin/bash
# Host checks for GalleryFace (no watch needed):
#   1. every painting decodes with the firmware's JPEGDEC path and matches
#      Pillow's decode of the same bytes;
#   2. crossfades: exact frames at fade start/middle/end, paintings advance in
#      order incl. the wrap, a fade waits for the decoder;
#   3. both QR cards decode (OpenCV) to exactly the right URL;
#   4. PNGs in test/out/ for eyeballing.
# Every checker first runs against broken input to prove it can fail.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"
L="$ROOT/.pio/libdeps/ewatch"; G="$L/GFX Library for Arduino/src"; J="$L/JPEGDEC/src"; Q="$L/QRCode/src"
[ -d "$G" ] && [ -d "$J" ] && [ -d "$Q" ] || { echo "run 'pio run' once to fetch libdeps"; exit 2; }
OUT="$HERE/out"; mkdir -p "$OUT"; rm -f "$OUT"/*
BIN="${TMPDIR:-/tmp}/galleryface_host"
xcrun clang -c -O2 -w "$Q/qrcode.c" -o "${BIN}_qrcode.o"
xcrun clang++ -std=c++14 -O2 -w -I"$HERE/host/stub" -I"$HERE/host" -I"$G" -I"$J" -I"$Q" -I"$ROOT/src" \
  "$G/Arduino_GFX.cpp" "$G/Arduino_G.cpp" "$HERE/host/png.cpp" "$J/JPEGDEC.cpp" "${BIN}_qrcode.o" \
  "$ROOT/src/slides.cpp" "$ROOT/src/face_draw.cpp" "$ROOT/src/qr_draw.cpp" "$HERE/host/host_test.cpp" \
  -lz -o "$BIN"
PY=(uv run -q --with pillow --with numpy --with opencv-python-headless --with zxing-cpp python3)
# Output is captured, never piped into grep -q (pipefail + SIGPIPE = false result).
echo "== control: fade checker must fail on a corrupted blend"
CTL="$("$BIN" "$OUT" --break-blend || true)"
if [[ "$CTL" == *"mid-fade frame is not a 50/50 blend"* ]]; then echo "ok, it failed"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
echo "== real run"
"$BIN" "$OUT"
echo "== control: decode checker must fail on a flat tile"
CTL="$("${PY[@]}" "$HERE/check_decode.py" "$OUT" --control 2>&1 || true)"
if [[ "$CTL" == *" 1 bad [("* ]]; then echo "ok, it failed on exactly the tampered painting"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
"${PY[@]}" "$HERE/check_decode.py" "$OUT"
echo "== control: QR checker must fail on swapped URLs"
CTL="$("${PY[@]}" "$HERE/check_qr.py" "$OUT" --control 2>&1 || true)"
if [[ "$CTL" == *"MISMATCH"* && "$CTL" == *"decoded 'https://"* ]]; then echo "ok, it failed (and it did decode)"; else echo "$CTL"; echo "CONTROL DID NOT FAIL"; exit 1; fi
"${PY[@]}" "$HERE/check_qr.py" "$OUT"
rm -f "$OUT/decoded.bin"
echo "ALL HOST CHECKS PASSED"
