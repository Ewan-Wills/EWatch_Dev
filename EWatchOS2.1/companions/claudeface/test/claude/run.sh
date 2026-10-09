#!/bin/bash
# Host unit tests for the Claude Face pure logic.
#
#   test/claude/run.sh
#
# 1. Python packs frames from ClaudeFace/protocol/protocol.json (struct.pack,
#    independent of the generated C header and the TS encoder).
# 2. test_main parses them with the firmware's cf_logic.cpp, runs the haptic
#    state machine / turn-timer / formatting tests, and encodes EVENT/HELLO
#    frames that Python decodes and verifies.
# 3. The checker is proven able to fail: a tampered vector file, a tampered
#    checkout line and five source mutations of cf_logic.cpp must each FAIL.
# 4. The drawing module's region checks (render.sh) run if libdeps exist.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"             # ClaudeFace/firmware/
SRC="$(cd "$HERE/../../../../src/apps/modules/claudeface" && pwd)"   # the module's face code
W="${TMPDIR:-/tmp}/cf_host_tests.$$"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
CXX="xcrun clang++"
FAILS=0
MIN_CHECKS=200

build() {  # build <logic.cpp> <out>
  $CXX -std=c++14 -O1 -Wall -Wextra -Werror -I"$SRC" "$1" "$HERE/test_main.cpp" -o "$2"
}

echo "== firmware-dialect compile of cf_logic.cpp (-std=gnu++11 -Wall -Wextra -Werror)"
if ! $CXX -std=gnu++11 -Wall -Wextra -Werror -fsyntax-only -I"$SRC" "$SRC/cf_logic.cpp"; then
  echo "FAIL: cf_logic.cpp not clean as C++11"; FAILS=$((FAILS+1))
fi

echo "== vectors"
python3 "$HERE/mkframes.py" vectors "$W/vectors.txt" || { echo "FAIL: mkframes"; exit 1; }

echo "== unit tests"
build "$SRC/cf_logic.cpp" "$W/t" || { echo "FAIL: build"; exit 1; }
OUT="$("$W/t" "$W/vectors.txt" "$W/checkout.txt")"; RC=$?
echo "$OUT" | grep -E "^FAIL|host tests:"
N=$(echo "$OUT" | sed -nE 's/^host tests: ([0-9]+) checks.*/\1/p')
if [ "$RC" -ne 0 ]; then echo "FAIL: unit tests rc=$RC"; FAILS=$((FAILS+1)); fi
if [ -z "$N" ] || [ "$N" -lt "$MIN_CHECKS" ]; then
  echo "FAIL: only '${N:-0}' checks ran (expected >= $MIN_CHECKS) — dead harness?"; FAILS=$((FAILS+1))
fi

echo "== watch->host frames decoded by python"
python3 "$HERE/mkframes.py" checkout "$W/checkout.txt" || { echo "FAIL: checkout"; FAILS=$((FAILS+1)); }

echo "== prove the checkers can fail"
expect_fail() {  # expect_fail <label> <cmd...>
  local label="$1"; shift
  if "$@" > "$W/ef.log" 2>&1; then
    echo "FAIL: '$label' should have failed but passed"; FAILS=$((FAILS+1))
  else
    echo "ok: '$label' fails as it must ($(grep -cE '^FAIL|CHECKOUT FAIL' "$W/ef.log") failing checks)"
  fi
}
# tampered expectation: first OK vector relabelled BAD
sed '1s/^OK/BAD/' "$W/vectors.txt" > "$W/vectors_bad.txt"
expect_fail "tampered vector" "$W/t" "$W/vectors_bad.txt" "$W/co2.txt"
# tampered checkout: expected seq changed
sed '1s/seq=513/seq=514/' "$W/checkout.txt" > "$W/checkout_bad.txt"
expect_fail "tampered checkout" python3 "$HERE/mkframes.py" checkout "$W/checkout_bad.txt"
# source mutations
mutate() {  # mutate <label> <perl-subst>
  local label="$1" expr="$2"
  perl -0pe "$expr" "$SRC/cf_logic.cpp" > "$W/mut.cpp"
  if cmp -s "$W/mut.cpp" "$SRC/cf_logic.cpp"; then
    echo "FAIL: mutation '$label' did not apply (stale pattern)"; FAILS=$((FAILS+1)); return
  fi
  if ! $CXX -std=c++14 -O1 -w -I"$SRC" "$W/mut.cpp" "$HERE/test_main.cpp" -o "$W/tm"; then
    echo "FAIL: mutation '$label' did not compile"; FAILS=$((FAILS+1)); return
  fi
  expect_fail "mutation: $label" "$W/tm" "$W/vectors.txt" "$W/co3.txt"
}
mutate "skip CRC check"        's/if \(cfCrc16\(buf, plen\) != crc\) return CF_PARSE_BAD;//'
mutate "DONE at >= 60 s"       's/p_last_turn_s > CF_DONE_MIN_TURN_S/p_last_turn_s >= CF_DONE_MIN_TURN_S/'
mutate "6 BLOCKED fires"       's/s\.blkFires < CF_BLOCKED_MAX_FIRES/s.blkFires <= CF_BLOCKED_MAX_FIRES/'
mutate "no quiet hours"        's/\|\| cfInQuietHours\(localHour, cfg\.quietStart, cfg\.quietEnd\)//'
mutate "size before version"   's/if \(buf\[0\] != CF_PROTO_VERSION\) return CF_PARSE_VERSION;\n  if \(plen != want\) return CF_PARSE_BAD;/if (plen != want) return CF_PARSE_BAD;\n  if (buf[0] != CF_PROTO_VERSION) return CF_PARSE_VERSION;/'

if [ -d "$ROOT/.pio/libdeps/ewatch/GFX Library for Arduino/src" ]; then
  echo "== drawing module: render + region soundness checks"
  "$ROOT/test/render/render.sh" > "$W/render.log" 2>&1; RRC=$?
  tail -3 "$W/render.log"
  [ "$RRC" -eq 0 ] || { echo "FAIL: render checks rc=$RRC"; FAILS=$((FAILS+1)); }
  echo "== prove the render/region checks can fail"
  render_expect_fail() {  # render_expect_fail <label> <draw.cpp> <extra flags>
    if CF_DRAW_SRC="$2" CF_EXTRA_FLAGS="$3" CF_RENDER_OUT="$W/rout" \
         "$ROOT/test/render/render.sh" > "$W/rf.log" 2>&1; then
      echo "FAIL: render '$1' should have failed but passed"; FAILS=$((FAILS+1))
    elif ! grep -qE "FAIL" "$W/rf.log"; then
      echo "FAIL: render '$1' failed without a check firing (build error?)"; FAILS=$((FAILS+1))
    else
      echo "ok: render '$1' fails as it must ($(grep -cE 'FAIL' "$W/rf.log") failing checks)"
    fi
  }
  perl -0pe 's/r\.w = CF_COL_X; r\.h = CF_FOOTER_Y;/r.w = CF_COL_X - 1; r.h = CF_FOOTER_Y;/' \
    "$SRC/claude_face_draw.cpp" > "$W/draw_mut.cpp"
  if cmp -s "$W/draw_mut.cpp" "$SRC/claude_face_draw.cpp"; then
    echo "FAIL: render mutation did not apply (stale pattern)"; FAILS=$((FAILS+1))
  else
    render_expect_fail "HERO rect 1 px narrow" "$W/draw_mut.cpp" "-I$SRC"
  fi
  render_expect_fail "panel rect mapping 1 row off" "$SRC/claude_face_draw.cpp" "-DCF_TEST_BREAK_RECT"
fi

echo
if [ "$FAILS" -eq 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "$FAILS FAILURE(S)"; fi
exit "$FAILS"
