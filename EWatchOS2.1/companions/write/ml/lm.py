"""English context for the Write app: a letter trigram prior and a word
dictionary, plus a simulator that 'writes' words with real letters from the
UJIpenchars2 test writers (unseen in training) to measure what context buys.

  lm.py sim                       accuracy: raw CNN / +trigram / +dictionary
  lm.py header OUT.h [NWORDS]     emit write_lm_data.h for the firmware

The decoder here (decode_letter, correct_word) is the reference for
write_lm.cpp; keep them in step.
"""
import math, sys, re
import numpy as np
from common import CLASSES

A = "abcdefghijklmnopqrstuvwxyz"
B = 26                       # boundary symbol index (start/end of word)
LAM = 0.5                    # weight of the trigram prior
DIGIT_LOGP = math.log(0.01)  # prior for a digit inside letters
DIGIT_START = math.log(0.25) # prior for a digit starting a word (numbers)
NUM_LETTER = math.log(0.01)  # prior for a letter inside a number
TRI_FLOOR = math.log(0.002)  # lowest letter-trigram log-prob
ALPHA = 0.25                 # weight of word log-frequency in correction
DIGIT_WORD_MARGIN = 7.0      # extra margin to turn a 2-digit string into a word ("10" stays 10)
DIGIT_WORD_MARGIN3 = 2.0     # ... a string of 3+ digits ("701" -> YOU)
MARGIN = 2.0                 # dictionary word must beat the typed string by this (nats)


def load_words(n=None):
    out = []
    for ln in open("data/lm/count_1w.txt"):
        w, c = ln.split("\t")
        if re.fullmatch(r"[a-z]+", w) and (len(w) > 1 or w in ("a", "i")):
            out.append((w, int(c)))
        if n and len(out) >= n:
            break
    return out


def trigram(words):
    """-> logp[27,27,26] = log P(letter | prev2, prev1), boundary = 26."""
    c3 = np.full((27, 27, 26), 0.1)
    c2 = np.full((27, 26), 0.1)
    for w, n in words:
        s = [B, B] + [A.index(ch) for ch in w]
        wt = n ** 0.5                         # damp "the", "of" so rarer words still count
        for i in range(2, len(s)):
            c3[s[i - 2], s[i - 1], s[i]] += wt
            c2[s[i - 1], s[i]] += wt
    p2 = c2 / c2.sum(1, keepdims=True)
    tot = c3.sum(2, keepdims=True)
    lam = tot / (tot + 50.0)                  # back off to the bigram where data is thin
    p3 = lam * c3 / tot + (1 - lam) * p2[None]
    return np.maximum(np.log(p3), TRI_FLOOR)   # a rare pair must not beat a confident CNN


def quant(logp):
    """log-probs -> uint8 in units of 1/8 nat below 0 (0 = p 1, 255 = tiny)."""
    return np.clip(np.round(-logp * 8), 0, 255).astype(np.uint8)


def dequant(q):
    return -q.astype(np.float32) / 8.0


class Ctx:
    def __init__(self, nwords=20000):
        self.words = load_words(nwords)
        self.tri = dequant(quant(trigram(load_words(60000))))
        tot = sum(c for _, c in self.words)
        lf = np.array([math.log(c / tot) for _, c in self.words])
        self.wlogf = dequant(quant(lf - lf.max()))           # same quantisation as the firmware
        self.bylen = {}
        for (w, _), f in zip(self.words, self.wlogf):
            self.bylen.setdefault(len(w), []).append((w, f))

    def decode_letter(self, logp, prev):
        """logp: 36 CNN log-probs; prev: text of the current word so far (lower)."""
        letters_only = all(ch in A for ch in prev)
        p1 = A.index(prev[-1]) if prev and prev[-1] in A else B
        p0 = A.index(prev[-2]) if len(prev) > 1 and prev[-2] in A else B
        s = logp.copy()
        if letters_only:
            s[:26] += LAM * self.tri[p0, p1]
            s[26:] += DIGIT_START if not prev else DIGIT_LOGP
        elif all(ch.isdigit() for ch in prev):               # in a number: digits
            s[:26] += NUM_LETTER
        return CLASSES[int(np.argmax(s))]

    def correct_word(self, typed, logps):
        """typed: decoded word (upper), logps: per-letter CNN log-probs. -> word."""
        L = len(typed)
        # Letters with a stray digit ("1APPY") are still words; an all-digit
        # string needs overwhelming evidence (a y/o/u read as 7/0/1).
        if L < 2 or L not in self.bylen:
            return typed
        idx = np.array([[CLASSES.index(ch.upper()) for ch in w] for w, _ in self.bylen[L]])
        M = np.stack(logps)                                   # (L,36)
        sc = M[np.arange(L)[None], idx].sum(1) + ALPHA * np.array([f for _, f in self.bylen[L]])
        best = int(np.argmax(sc))
        t = [CLASSES.index(ch) for ch in typed]
        typed_sc = M[np.arange(L), t].sum() + ALPHA * self.oov(typed)
        if typed.lower() == self.bylen[L][best][0]:
            return typed
        if typed.isdigit():
            extra = DIGIT_WORD_MARGIN if L <= 2 else DIGIT_WORD_MARGIN3
            return self.bylen[L][best][0].upper() if sc[best] > typed_sc + MARGIN + extra else typed
        if not typed.isalpha():               # a stray digit: the best word, no margin
            return self.bylen[L][best][0].upper() if sc[best] > typed_sc else typed
        return self.bylen[L][best][0].upper() if sc[best] > typed_sc + MARGIN else typed

    def oov(self, w):
        """log-frequency stand-in for a word not chosen from the list: its own
        entry if listed, else the floor."""
        for ww, f in self.bylen.get(len(w), []):
            if ww == w.lower():
                return f
        return -255 / 8.0


def sim(n=3000, seed=0):
    from watch_tests import tst
    data = tst()
    P = np.array([[float(v) for v in l.split()[1:]] for l in open("data/tst_probs.txt")])
    LP = np.log(np.maximum(P, 1e-6))
    by = {}
    for i, (lab, orig, w, _) in enumerate(data):
        by.setdefault((w.split("-")[0], orig), []).append(i)
    writers = sorted({k[0] for k in by})
    ctx = Ctx()
    r = np.random.default_rng(seed)
    pool = ctx.words[:5000]
    pw = np.array([c for _, c in pool], float) ** 0.5; pw /= pw.sum()
    nums = [str(v) for v in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20, 24, 30, 42, 99, 100, 365, 2024, 2026)]
    res_num = {k: [0, 0, 0, 0] for k in ("raw", "tri", "dict")}
    oov = ["xavi", "zork", "qwop", "brexit", "ewan", "ewatch", "kfc", "jpeg", "nyx", "vlog", "zuck", "yeet"]
    res = {k: [0, 0, 0, 0] for k in ("raw", "tri", "dict")}   # chars ok, chars, words ok, words
    res_oov = {k: [0, 0, 0, 0] for k in res}
    for t in range(n):
        is_oov = t % 10 == 0
        is_num = t % 10 == 5
        word = oov[r.integers(len(oov))] if is_oov else nums[r.integers(len(nums))] if is_num else pool[r.choice(len(pool), p=pw)][0]
        wr = writers[r.integers(len(writers))]
        upper = r.random() < 0.2
        lps = []
        for ch in word:
            key = (wr, ch.upper() if upper and ch.isalpha() else ch)
            lps.append(LP[by[key][r.integers(len(by[key]))]])
        raw = "".join(CLASSES[int(np.argmax(l))] for l in lps)
        tri = ""
        for l in lps:
            tri += ctx.decode_letter(l, tri.lower())
        dic = ctx.correct_word(tri, lps)
        R = res_oov if is_oov else res_num if is_num else res
        for k, out in (("raw", raw), ("tri", tri), ("dict", dic)):
            R[k][0] += sum(a == b for a, b in zip(out, word.upper())); R[k][1] += len(word)
            R[k][2] += out == word.upper(); R[k][3] += 1
    for name, R in (("dictionary words", res), ("out-of-vocabulary words", res_oov), ("numbers", res_num)):
        print(name)
        for k, (a, b, c, d) in R.items():
            print(f"  {k:5s} char acc {a/b:.4f}  word acc {c/d:.4f}  (n={d})")


def header(path, nwords=20000):
    ctx_words = load_words(nwords)
    tri = quant(trigram(load_words(60000)))
    tot = sum(c for _, c in ctx_words)
    lf = np.array([math.log(c / tot) for _, c in ctx_words])
    q = quant(lf - lf.max())
    # words grouped by length, each length block sorted by frequency;
    # blob = for each word: letters (a-z) then one byte log-freq.
    bylen = {}
    for (w, _), f in zip(ctx_words, q):
        bylen.setdefault(len(w), []).append((w, f))
    maxlen = max(bylen)
    blob = bytearray(); offs = []; counts = []
    for L in range(maxlen + 1):
        offs.append(len(blob)); counts.append(len(bylen.get(L, [])))
        for w, f in bylen.get(L, []):
            blob += w.encode() + bytes([int(f)])
    with open(path, "w") as f:
        f.write(f"// GENERATED by ml-write/lm.py - do not edit. Norvig count_1w, top {nwords} words.\n")
        f.write("#pragma once\n#include <stdint.h>\nnamespace wlm_d {\n")
        f.write(f"constexpr int kMaxLen = {maxlen};\n")
        f.write(f"constexpr float kLam = {LAM}f, kDigitLogp = {DIGIT_LOGP:.6f}f, kDigitStart = {DIGIT_START:.6f}f, kAlpha = {ALPHA}f, kMargin = {MARGIN}f, kDigitWordMargin = {DIGIT_WORD_MARGIN}f, kDigitWordMargin3 = {DIGIT_WORD_MARGIN3}f;\n")
        f.write("// trigram: [prev2][prev1][letter], prev 26 = word boundary; -log p * 8\n")
        f.write(f"static const uint8_t kTri[27*27*26] = {{{','.join(map(str, tri.ravel()))}}};\n")
        f.write(f"static const uint32_t kOff[{maxlen + 1}] = {{{','.join(map(str, offs))}}};\n")
        f.write(f"static const uint16_t kCount[{maxlen + 1}] = {{{','.join(map(str, counts))}}};\n")
        f.write(f"static const uint8_t kWords[{len(blob)}] = {{{','.join(map(str, blob))}}};\n")
        f.write("}  // namespace wlm_d\n")
    print(f"wrote {path}: {len(blob)} word bytes + {tri.size} trigram bytes")


if __name__ == "__main__":
    if sys.argv[1] == "sim":
        sim()
    elif sys.argv[1] == "header":
        header(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 20000)
