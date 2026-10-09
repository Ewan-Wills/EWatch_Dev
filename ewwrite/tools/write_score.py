#!/usr/bin/env python3
"""Score a Write accuracy sweep from the watch console log.

  ewsimrec ewwrite --snap /tmp/s test/write_sweep.ewt -v 2> sweep.log
  python3 tools/write_score.py sweep.log [test/write_sweep.expected]

Pairs every recognition line (`WRITE: char=` / `edit=` / `reject`) with the
expected label, in order. Fails (exit 1) if the number of recognitions differs
from the number of draws: a dead or double-firing recogniser can't score.
"""
import re, sys, os, collections

HERE = os.path.dirname(os.path.abspath(__file__))
log = open(sys.argv[1], errors="replace").read().splitlines()
exp_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "test", "write_sweep.expected")
expected = open(exp_path).read().strip()

got = []
for l in log:
    m = re.search(r"WRITE: (char=(\S)|edit=(\w+)|reject)", l)
    if not m:
        continue
    if m.group(2):
        cands = re.search(r"cands=(\S+)", l).group(1)
        got.append((m.group(2), cands))
    elif m.group(3):
        got.append(("<" + m.group(3) + ">", ""))
    else:
        got.append(("<reject>", re.search(r"cands=(\S*)", l).group(1)))

print(f"draws={len(expected)} recognitions={len(got)}")
if len(got) != len(expected):
    print("FAIL: recognition count != draw count (dead recogniser, merged or split glyphs)")
    sys.exit(1)

per = collections.defaultdict(lambda: [0, 0])
conf = collections.Counter()
top3 = 0
for e, (g, cands) in zip(expected, got):
    per[e][1] += 1
    if g == e:
        per[e][0] += 1
    else:
        conf[f"{e}->{g}"] += 1
    if e in [c.split(":")[0] for c in cands.split(",") if c]:
        top3 += 1
ok = sum(v[0] for v in per.values())
print(f"top1={ok}/{len(expected)} = {100*ok/len(expected):.1f}%   top3={top3}/{len(expected)} = {100*top3/len(expected):.1f}%")
print("per-char: " + " ".join(f"{k}={v[0]}/{v[1]}" for k, v in sorted(per.items())))
print("confusions: " + (" ".join(f"{k}:{n}" for k, n in conf.most_common()) or "none"))
sys.exit(0)
