#!/bin/bash
# Cross-check: frames produced by the REAL TypeScript bridge encoder must
# parse in the firmware's C parser and decode to the same field values.
#
#   test/xcheck/run.sh
#
# 1. tsc compiles ClaudeFace/bridge/src/protocol.gen.ts into a temp dir (the
#    bridge's own dist/ is never touched); gen_frames.js emits frames + the
#    summaries xcheck must print.
# 2. xcheck.cpp + src/cf_logic.cpp (clang++ -std=c++11) parses them; its
#    output must equal the expected summaries line for line.
# 3. Prove the check can fail: a corrupted frame must make xcheck exit
#    non-zero, and a tampered expectation must make the comparison fail.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"                 # ClaudeFace/firmware/
BRIDGE="$(cd "$ROOT/../bridge" && pwd)"
W="${TMPDIR:-/tmp}/cf_xcheck.$$"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
FAILS=0

[ -x "$BRIDGE/node_modules/.bin/tsc" ] || { echo "missing $BRIDGE/node_modules (npm install in the bridge)"; exit 2; }
(cd "$BRIDGE" && node_modules/.bin/tsc --target ES2022 --module commonjs --skipLibCheck \
   --outDir "$W/js" src/protocol.gen.ts) || { echo "FAIL: tsc"; exit 1; }
node "$HERE/gen_frames.js" "$W/js/protocol.gen.js" "$W/frames.txt" "$W/expect.txt" || { echo "FAIL: gen_frames"; exit 1; }
xcrun clang++ -std=c++11 -Wall -Wextra -Werror -O1 -I"$ROOT/src" \
  "$HERE/xcheck.cpp" "$ROOT/src/cf_logic.cpp" -o "$W/xcheck" || { echo "FAIL: build"; exit 1; }

"$W/xcheck" < "$W/frames.txt" > "$W/got.txt"; RC=$?
N=$(wc -l < "$W/expect.txt" | tr -d ' ')
if [ "$RC" -ne 0 ]; then echo "FAIL: xcheck rc=$RC"; FAILS=$((FAILS+1)); fi
if diff "$W/expect.txt" "$W/got.txt" > "$W/diff.txt"; then
  echo "ok: $N bridge frames decode in the C parser with matching fields"
else
  echo "FAIL: decoded fields differ:"; cat "$W/diff.txt"; FAILS=$((FAILS+1))
fi
[ "$N" -ge 10 ] || { echo "FAIL: only $N frames (dead generator?)"; FAILS=$((FAILS+1)); }

echo "== prove the check can fail"
# corrupted frame: flip one base64 char in the middle of the second frame
perl -pe 'if ($. == 2) { substr($_, 20, 1) = (substr($_, 20, 1) eq "A" ? "B" : "A") }' \
  "$W/frames.txt" > "$W/frames_bad.txt"
if "$W/xcheck" < "$W/frames_bad.txt" > /dev/null 2>&1; then
  echo "FAIL: corrupted frame accepted"; FAILS=$((FAILS+1))
else
  echo "ok: corrupted frame rejected"
fi
# tampered expectation
sed '1s/seq=1 /seq=2 /' "$W/expect.txt" > "$W/expect_bad.txt"
if cmp -s "$W/expect.txt" "$W/expect_bad.txt"; then
  echo "FAIL: tamper did not apply"; FAILS=$((FAILS+1))
elif diff -q "$W/expect_bad.txt" "$W/got.txt" > /dev/null; then
  echo "FAIL: tampered expectation still matched"; FAILS=$((FAILS+1))
else
  echo "ok: tampered expectation detected"
fi

echo
if [ "$FAILS" -eq 0 ]; then echo "XCHECK PASSED"; else echo "$FAILS FAILURE(S)"; fi
exit "$FAILS"
