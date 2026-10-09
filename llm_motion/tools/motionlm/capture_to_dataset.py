"""Turn serial logs from capture mode into training data for real-wrist fine-tuning.

On the watch:   motion cap flick_out      (or the REC chip in the Motion app)
                ... do the gesture ~20 times, pausing ~0.5 s between ...
                motion cap twist_l ... ; motion cap none (ordinary wrist movement) ; motion cap off
Log the serial port to a file (e.g. `ewt tail 600 -g MCAP > caps.log`, or any
terminal logger), then:

  python -m motionlm.capture_to_dataset caps.log [more.log ...] -o data/real.jsonl
  python -m motionlm.train --init out/motion_lm.pt --steps 1500 --lr 1e-3 --real data/real.jsonl

Each MCAP line is one segment the watch's segmenter cut, with the label you
set and the model's own prediction at the time. MCAPRAW lines carry the raw
samples ((len+1) windows: one priming window); they are re-tokenized here with
tokenizer.py and compared to the tokens the watch logged, so a tokenizer
change can be replayed over old recordings (--retokenize).
Caveat: a capture label applies to EVERY segment while it is set. Keep the
wrist still between repetitions, or a stray movement is recorded as that
gesture; review with --show and drop bad rows.
"""
import argparse
import json
import re

from . import tokenizer as T, vocab as V

CAP = re.compile(r"MCAP n=(\d+) label=(\S+) pred=(\S+) p=([\d.]+) src=(\S+) toks=([\d,]*)")
RAW = re.compile(r"MCAPRAW n=(\d+) part=(\d+)/(\d+) (\S*)")


def parse(paths):
    caps, raws = {}, {}
    for p in paths:
        with open(p, errors="replace") as f:
            for line in f:
                m = CAP.search(line)
                if m:
                    n = int(m.group(1))
                    caps[n] = {"n": n, "label": m.group(2), "pred": m.group(3), "p": float(m.group(4)),
                               "src": m.group(5), "toks": [int(t) for t in m.group(6).split(",") if t]}
                    continue
                m = RAW.search(line)
                if m:
                    n, part, parts = int(m.group(1)), int(m.group(2)), int(m.group(3))
                    smp = [tuple(int(v) for v in s.split(",")) for s in m.group(4).split(";") if s]
                    raws.setdefault(n, {})[part] = (parts, smp)
    out = []
    for n, c in sorted(caps.items()):
        r = raws.get(n)
        if r:
            parts = next(iter(r.values()))[0]
            if all(i in r for i in range(1, parts + 1)):
                c["raw"] = [s for i in range(1, parts + 1) for s in r[i][1]]
        out.append(c)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("-o", "--out", default="data/real.jsonl")
    ap.add_argument("--retokenize", action="store_true", help="use tokens re-derived from raw samples")
    ap.add_argument("--show", action="store_true")
    a = ap.parse_args()
    caps = parse(a.logs)
    agree = mism = noraw = 0
    rows = []
    for c in caps:
        if c["label"] not in V.LABELS:
            continue
        toks = c["toks"]
        if c["src"] == "imu":
            if "raw" not in c:
                noraw += 1
            else:
                rt = T.tokenize_trace(c["raw"])[1:]          # drop the priming window
                rt = rt[-len(toks):] if len(rt) >= len(toks) else rt
                if rt == toks:
                    agree += 1
                else:
                    mism += 1
                if a.retokenize:
                    toks = rt
        rows.append({"toks": toks, "label": c["label"], "src": c["src"], "watch_pred": c["pred"],
                     "watch_p": c["p"], "raw": c.get("raw")})
        if a.show:
            print("%4d %-13s pred=%-13s p=%.2f %s" % (c["n"], c["label"], c["pred"], c["p"],
                                                    " ".join(V.tok_name(t) for t in toks)))
    with open(a.out, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    by = {}
    for r in rows:
        by[r["label"]] = by.get(r["label"], 0) + 1
    print("wrote %d segments to %s  per label: %s" % (len(rows), a.out, by))
    print("raw re-tokenization: %d agree, %d differ, %d without raw" % (agree, mism, noraw))
    acc = sum(r["watch_pred"] == r["label"] for r in rows) / max(1, len(rows))
    print("the watch's own live accuracy on these captures: %.3f (real-wrist accuracy of the current model)" % acc)


if __name__ == "__main__":
    main()
