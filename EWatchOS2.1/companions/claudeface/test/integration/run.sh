#!/bin/sh
# Claude Face integration host tests. Builds the suite (clang++ -std=c++11),
# runs it, then proves it can fail: each deliberately broken variant must
# exit non-zero, otherwise this script fails.
set -u
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/cf_integration_tests
mkdir -p "$OUT"
M=../../../../src/apps/modules/claudeface
SRC="test_integration.cpp $M/cf_logic.cpp"
FLAGS="-std=c++11 -Wall -Wextra -Werror -O1 -pthread -I$M"

clang++ $FLAGS $SRC -o "$OUT/t_ok" || { echo "BUILD FAILED"; exit 1; }
"$OUT/t_ok" || { echo "SUITE FAILED"; exit 1; }

rc=0
for brk in CF_TEST_BREAK_SNAPSHOT CF_TEST_BREAK_POLICY CF_TEST_BREAK_ORIENT CF_TEST_BREAK_DIR CF_TEST_BREAK_BUTTON; do
  clang++ $FLAGS -D$brk $SRC -o "$OUT/t_$brk" 2>/dev/null || { echo "break build $brk failed"; rc=1; continue; }
  if "$OUT/t_$brk" >/dev/null 2>&1; then
    echo "CHECKER BROKEN: suite passed with -D$brk"; rc=1
  else
    echo "ok: -D$brk makes the suite fail"
  fi
done
exit $rc
