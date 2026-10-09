"""Build labelled motion-language sequences: [BOS] tokens [SEP] <label>.

IMU classes: synthetic traces -> the exact on-device tokenizer + segmenter.
A positive trace must produce exactly ONE segment (else it is dropped and
counted); every segment of a "none" trace is a "none" example.
Touch phrases are generated directly as touch-token strings.
"""
import json
import multiprocessing as mp

import numpy as np

from . import synth, tokenizer as T, vocab as V

TOUCH_POS = {
    "swipe_uu": [[V.SW_U, V.SW_U]],
    "swipe_lrl": [[V.SW_L, V.SW_R, V.SW_L]],
    "swipe_dd": [[V.SW_D, V.SW_D]],
    "tap_tap_r": [[V.TAP, V.TAP, V.SW_R], [V.DTAP, V.SW_R]],
}
TOUCH_TOKS = list(range(V.TOUCH0, V.TOUCH0 + 7))


def touch_label(seq):
    for k, alts in TOUCH_POS.items():
        if list(seq) in alts:
            return k
    return "none"


def gen_touch(rng, name):
    if name != "none":
        alts = TOUCH_POS[name]
        return list(alts[rng.integers(len(alts))])
    while True:
        if rng.random() < 0.5:
            seq = [int(t) for t in rng.choice(TOUCH_TOKS, rng.integers(1, 7))]
        else:   # near miss: mutate a positive by one edit
            alts = [a for v in TOUCH_POS.values() for a in v]
            seq = list(alts[rng.integers(len(alts))])
            op = rng.integers(3)
            i = int(rng.integers(len(seq) + (1 if op == 0 else 0)))
            if op == 0:
                seq.insert(i, int(rng.choice(TOUCH_TOKS)))
            elif op == 1 and len(seq) > 1:
                del seq[min(i, len(seq) - 1)]
            else:
                seq[min(i, len(seq) - 1)] = int(rng.choice(TOUCH_TOKS))
        if touch_label(seq) == "none":
            return seq


def _imu_job(args):
    name, n_target, seed = args
    rng = np.random.default_rng(seed)
    out, dropped, tries = [], 0, 0
    while len(out) < n_target:
        tries += 1
        tr = synth.gen(name, rng)
        segs = T.trace_segments(tr)
        if name == "none":
            out.extend(segs[: n_target - len(out)])
        elif len(segs) == 1:
            out.append(segs[0])
        else:
            dropped += 1
    return name, out, dropped, tries


def build(n_per_class, n_none, n_touch, seed, procs=8):
    jobs = []
    for i, name in enumerate(V.IMU_LABELS):
        n = n_none if name == "none" else n_per_class
        chunk = max(1, n // procs)
        k = 0
        while k < n:
            m = min(chunk, n - k)
            jobs.append((name, m, seed * 1000003 + i * 9973 + k))
            k += m
    data, stats = [], {}
    with mp.Pool(procs) as pool:
        for name, segs, dropped, tries in pool.imap_unordered(_imu_job, jobs):
            data.extend((s, name) for s in segs)
            st = stats.setdefault(name, [0, 0])
            st[0] += dropped
            st[1] += tries
    rng = np.random.default_rng(seed + 7)
    for name in V.TOUCH_LABELS:
        n = n_touch * (2 if name == "none" else 1)
        data.extend((gen_touch(rng, name), name) for _ in range(n))
    rng.shuffle(data)
    return data, stats


def to_ids(seg, label):
    return T.model_input(seg) + [V.label_tok(label)]


def save_jsonl(path, data):
    with open(path, "w") as f:
        for seg, lab in data:
            f.write(json.dumps({"toks": [int(t) for t in seg], "label": lab}) + "\n")


def load_jsonl(path):
    out = []
    with open(path) as f:
        for line in f:
            if line.strip():
                d = json.loads(line)
                out.append((d["toks"], d["label"]))
    return out
