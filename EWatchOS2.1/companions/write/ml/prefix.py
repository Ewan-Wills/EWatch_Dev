"""Which CNN predictions come from an unfinished letter? For every multi-stroke
UJIpenchars2 sample, classify its first j strokes (j < n) as well as the whole
glyph, placed and resampled as on the watch. Prints per class: how often a
confident prediction (p >= th) is a partial glyph."""
import subprocess, sys
import numpy as np
from common import parse_uji, label_of, CLASSES
from watch_tests import place

BENCH = sys.argv[1] if len(sys.argv) > 1 else "/tmp/claude-501/cnn_bench_ho"
WRITERS = sys.argv[2] if len(sys.argv) > 2 else "trn"
r = np.random.default_rng(3)
rows, kinds = [], []
for ch, w, strokes in parse_uji("data/uji/ujipenchars2.txt"):
    if label_of(ch) is None or not w.startswith(WRITERS):
        continue
    q = place(strokes, r)
    for j in range(1, len(q) + 1):
        toks = [f"{x} {y} {i}" for i, s in enumerate(q[:j]) for x, y in s]
        rows.append(f"{ch.upper()} {len(toks)} {' '.join(toks)}"); kinds.append(j < len(q))
open("/tmp/claude-501/prefix.txt", "w").write("\n".join(rows) + "\n")
out = [l.split() for l in subprocess.run([BENCH, "/tmp/claude-501/prefix.txt", "-p"], capture_output=True, text=True).stdout.splitlines() if l.strip()]
P = np.array([[float(v) for v in o[1:]] for o in out]); part = np.array(kinds)
np.save(f"data/prefix_{WRITERS}.npy", np.concatenate([P, part[:, None]], 1))
top = P.argmax(1); pt = P.max(1)
print(f"glyphs: {(~part).sum()} complete, {part.sum()} partial")
for th in (0.8, 0.9):
    print(f"--- p >= {th}: class: partial/complete")
    line = []
    for k in range(36):
        m = (top == k) & (pt >= th)
        a, b = (m & part).sum(), (m & ~part).sum()
        if a: line.append(f"{CLASSES[k]}:{a}/{b}")
    print("  ".join(line))
