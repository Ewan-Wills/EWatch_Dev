"""Host replica of the on-watch phrase test: the exact touch strokes from
words.json -> C CNN (cnn_bench -p, the firmware source) -> the LM decoder
(lm.py, bit-identical to write_lm.cpp) -> final text, scored like drive_words.

  phrase_host.py data/words.json [CNN_BENCH]
"""
import json, subprocess, sys
import numpy as np
import lm
from common import CLASSES

BENCH = "/Users/ewills/Documents/EWatch/EWatch_Firmware/EWatchOS2.1/test/write/host/cnn_bench"


def blocks(it):
    """steps -> one list of strokes per phrase character (split at 'wait 650')."""
    out, cur, st = [], [], None
    for s in it["steps"]:
        t = s.split()
        if t[0] == "stroke":
            for part in " ".join(t[2:]).split("|"):
                v = [int(z) for z in part.split()]
                cur.append(list(zip(v[0::2], v[1::2])))
            continue
        if t[:2] == ["touch", "down"]:
            st = [(int(t[2]), int(t[3]))]
        elif t[:2] == ["touch", "move"] and st is not None:
            st.append((int(t[2]), int(t[3])))
        elif t[:2] == ["touch", "up"] and st is not None:
            cur.append(st); st = None
        elif s == "wait 650":
            out.append(cur); cur = []
    return out


def run(path, bench=BENCH, verbose=True):
    items = json.load(open(path))
    lines, where = [], []
    for k, it in enumerate(items):
        for ch, strokes in zip(it["phrase"], blocks(it)):
            if ch == " ":
                continue
            toks = [f"{x} {y} {i}" for i, s in enumerate(strokes) for x, y in s]
            lines.append(f"{ch.upper()} {len(toks)} {' '.join(toks)}")
    open("/tmp/claude-501/phr.txt", "w").write("\n".join(lines) + "\n")
    out = subprocess.run([bench, "/tmp/claude-501/phr.txt", "-p"], capture_output=True, text=True).stdout.split("\n")
    P = [np.log(np.maximum(np.array([float(v) for v in l.split()[1:]]), 1e-6)) for l in out if l.strip()]
    ctx = lm.Ctx()
    i = 0; exact = 0; wr = wt = 0; raw_words = 0
    for it in items:
        text, word, lps = "", "", []
        raw = ""
        for ch in it["phrase"]:
            if ch == " ":
                if len(word) >= 2:
                    fixed = ctx.correct_word(word, lps)
                    text = text[: len(text) - len(word)] + fixed.lower()
                text += " "; raw += " "; word, lps = "", []
                continue
            c = ctx.decode_letter(P[i], word.lower())
            raw += CLASSES[int(np.argmax(P[i]))].lower()
            word += c; lps.append(P[i]); text += c.lower(); i += 1
        if len(word) >= 2:          # (the watch corrects on space only; shown for the final word too)
            fixed = ctx.correct_word(word, lps)
            text = text[: len(text) - len(word)] + fixed.lower()
        ok = text == it["phrase"]
        exact += ok
        wr += sum(a == b for a, b in zip(it["phrase"].split(), text.split())); wt += len(it["phrase"].split())
        raw_words += sum(a == b for a, b in zip(it["phrase"].split(), raw.split()))
        if verbose:
            print(f"{'OK ' if ok else '   '} {it['phrase']!r:24} -> {text!r:24} (CNN alone {raw!r})")
    print(f"phrases exact {exact}/{len(items)}  words {wr}/{wt} = {wr/wt:.3f}  (CNN alone {raw_words/wt:.3f})")
    return wr / wt


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else BENCH)
