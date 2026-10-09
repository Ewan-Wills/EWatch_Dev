"""Cross-check the Python pipeline against the FIRMWARE C code, and measure
end-to-end (trace-level) accuracy through the C path.

  python -m motionlm.crosscheck [--traces 300] [--break-checker]

1. builds tools/host/host_eval (firmware motion_tok.c + motion_lm.c +
   motion_model_data.c, clang -O2 -DMOTION_HOST)
2. tokens: Python tokenizer vs C tokenizer on random traces (must be identical)
3. segments + labels: PyTorch model vs C runtime on every segment (label equal,
   |p diff| small)
4. trace-level accuracy through the C path on fresh synthetic traces (seed 4242,
   unseen in training): segmentation + LM + threshold, the way the watch runs it
5. touch phrases through the C path
6. host speed (us/forward on this Mac)
--break-checker perturbs the Python tokenizer threshold first: the run MUST
then report token mismatches (proves the comparison can fail).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
import torch
import torch.nn.functional as F

from . import synth, tokenizer as T, vocab as V, dataset as D
from .model import Cfg, MotionLM
from .train import OUT, PROJ, TOOLS

SRC = os.path.join(PROJ, "src", "apps", "motion")
BIN = os.path.join(TOOLS, "host", "host_eval")


def build():
    srcs = [os.path.join(SRC, f) for f in ("motion_tok.c", "motion_lm.c", "motion_model_data.c")]
    srcs.append(os.path.join(TOOLS, "host", "host_eval.c"))
    cmd = ["xcrun", "clang", "-O2", "-DMOTION_HOST", "-I", SRC, "-o", BIN] + srcs + ["-lm"]
    subprocess.check_call(cmd)


def run_c(lines):
    p = subprocess.run([BIN], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
    return p.stdout.splitlines(), p.stderr.strip()


def parse_trace_output(out):
    """Group S lines per trace (each trace ends with an E line)."""
    res, segs = [], []
    for l in out:
        if l.startswith("S "):
            _, lab, p, toks = l.split(" ", 3)
            segs.append((lab, float(p), [int(t) for t in toks.split(",")]))
        elif l.startswith("E"):
            toks = l[2:].strip()
            res.append((segs, [int(t) for t in toks.split(",")] if toks else []))
            segs = []
    return res


def load_model():
    ck = torch.load(os.path.join(OUT, "motion_lm.pt"))
    m = MotionLM(Cfg(**ck["cfg"]))
    m.load_state_dict(ck["model"])
    m.eval()
    return m


@torch.no_grad()
def torch_label(model, seg):
    ids = torch.tensor([T.model_input(seg)])
    lg = model(ids)[0, -1, V.LABEL0:V.VOCAB]
    p = F.softmax(lg, -1)
    j = int(torch.argmax(p))
    return V.LABELS[j], float(p[j])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--traces", type=int, default=300, help="traces per class for the C-path accuracy")
    ap.add_argument("--thr", type=float, default=0.6)
    ap.add_argument("--break-checker", action="store_true")
    a = ap.parse_args()
    if a.break_checker:
        T.STILL_E += 50
        print("!! checker self-test: Python STILL_E perturbed by +50 counts; mismatches EXPECTED")
    build()
    model = load_model()
    rep = {}

    # ---- 1/2: token + segment + label agreement on random traces
    rng = np.random.default_rng(99)
    traces = []
    for name in V.IMU_LABELS:
        for _ in range(60):
            traces.append(synth.gen(name, rng))
    for _ in range(40):   # adversarial: wild random samples incl. clipping extremes
        n = int(rng.integers(30, 300))
        traces.append(np.clip(rng.normal(0, rng.uniform(100, 9000), (n, 3)), -8192, 8191).astype(int))
    lines = ["t %d %s" % (len(tr), " ".join("%d %d %d" % tuple(s) for s in tr)) for tr in traces]
    out, info = run_c(lines)
    print("C runtime:", info)
    parsed = parse_trace_output(out)
    assert len(parsed) == len(traces), (len(parsed), len(traces))
    tok_mismatch = seg_mismatch = lab_mismatch = 0
    pdiff = []
    nseg = 0
    for tr, (csegs, ctoks) in zip(traces, parsed):
        ptoks = T.tokenize_trace(tr)
        if ptoks != ctoks:
            tok_mismatch += 1
        psegs = T.segments(ptoks)
        if [s for _, _, s in csegs] != psegs:
            seg_mismatch += 1
            continue
        for lab, p, seg in csegs:
            nseg += 1
            tl, tp = torch_label(model, seg)
            if tl != lab:
                lab_mismatch += 1
            pdiff.append(abs(tp - p))
    rep["agreement"] = {"traces": len(traces), "token_mismatch_traces": tok_mismatch,
                        "segment_mismatch_traces": seg_mismatch, "segments": nseg,
                        "label_mismatch": lab_mismatch,
                        "max_abs_p_diff": float(max(pdiff)) if pdiff else None}
    print("agreement:", rep["agreement"])

    # ---- 3: trace-level accuracy through the C path
    rng = np.random.default_rng(4242)
    names, traces = [], []
    for name in V.IMU_LABELS:
        n = a.traces * (3 if name == "none" else 1)
        for _ in range(n):
            names.append(name)
            traces.append(synth.gen(name, rng))
    lines = ["t %d %s" % (len(tr), " ".join("%d %d %d" % tuple(s) for s in tr)) for tr in traces]
    out, _ = run_c(lines)
    parsed = parse_trace_output(out)
    per = {n: [0, 0] for n in V.IMU_LABELS}
    conf = {}
    for name, (csegs, _) in zip(names, parsed):
        fired = [lab for lab, p, _ in csegs if lab != "none" and p >= a.thr]
        if name == "none":
            ok = not fired
            outcome = fired[0] if fired else "none"
        else:
            ok = name in fired and all(f == name for f in fired)
            outcome = name if ok else (fired[0] if fired else ("none" if csegs else "no_segment"))
        per[name][0] += ok
        per[name][1] += 1
        conf.setdefault(name, {}).setdefault(outcome, 0)
        conf[name][outcome] += 1
    pos = sum(per[n][0] for n in V.IMU_LABELS[1:]) / sum(per[n][1] for n in V.IMU_LABELS[1:])
    rep["trace_level_c_path"] = {
        "seed": 4242, "threshold": a.thr,
        "gesture_trace_success": pos,
        "none_trace_no_trigger": per["none"][0] / per["none"][1],
        "per_class": {n: per[n][0] / per[n][1] for n in V.IMU_LABELS},
        "outcomes": conf,
    }
    print("trace-level (C path): gestures %.4f, none-without-trigger %.4f" % (
        pos, per["none"][0] / per["none"][1]))
    for n in V.IMU_LABELS:
        print("  %-13s %.3f  %s" % (n, per[n][0] / per[n][1], conf[n]))

    # ---- 4: touch phrases
    rng = np.random.default_rng(4343)
    items = [(D.gen_touch(rng, n), n) for n in V.TOUCH_LABELS for _ in range(200)]
    out, _ = run_c(["q %d %s" % (len(s), " ".join(map(str, s))) for s, _ in items])
    ok = 0
    for (s, n), l in zip(items, out):
        _, lab, p = l.split()
        final = lab if float(p) >= a.thr else "none"
        ok += final == n
    rep["touch_c_path_accuracy"] = ok / len(items)
    print("touch phrases (C path): %.4f of %d" % (ok / len(items), len(items)))

    # ---- 5: speed on this Mac
    out, _ = run_c(["b 200000"])
    rep["host_us_per_forward_mac"] = float(out[0].split()[1])
    print("host: %.2f us per forward (Apple M4 Pro, clang -O2, one core)" % rep["host_us_per_forward_mac"])

    if not a.break_checker:
        json.dump(rep, open(os.path.join(OUT, "crosscheck.json"), "w"), indent=1)
    bad = tok_mismatch or seg_mismatch or lab_mismatch
    print("CROSSCHECK", "FAIL" if bad else "PASS")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
