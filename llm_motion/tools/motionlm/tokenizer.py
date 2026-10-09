"""IMU window tokenizer + segmenter. Pure integer maths so the Python and the
C implementation (src/apps/motion/motion_tok.c) agree bit-for-bit; checked by
tools/motionlm/crosscheck.py.

Input: raw MMA8451 samples (14-bit, 4096 counts = 1 g, +-2 g full scale) at
50 Hz (the EWatchOS2.1 I/O task's 20 ms loop). One token per WIN samples.
"""
from .vocab import BOS, SEP, STILL0, MOVE0, JOLT0, TOUCH0, LABEL0

G = 4096
WIN = 3                    # samples per token (60 ms at 50 Hz)
# Thresholds. d* are on window SUMS (3 samples), e* on per-sample L1 diffs.
STILL_D = 1229             # |mean change| < 0.10 g
BIG_D = 5530               # |mean change| >= 0.45 g -> "big" move
STILL_E = 1434             # sum of |sample diffs| (3 diffs x 3 axes) < 0.35 g
JOLT_E = 4915              # >= 1.2 g of jerk inside one window
JOLT_E2 = 10240            # >= 2.5 g -> strong jolt
# Segmenter
PRE = 2                    # still tokens kept as context before motion
END_STILL = 5              # this many still tokens in a row end a segment (300 ms)
MAX_LEN = 40               # cap on segment length (tokens, incl. pre-roll)
SEQ_LEN = 48               # model context: BOS + 40 + SEP + label fits


def is_still(t):
    return STILL0 <= t < MOVE0


def _argmax_abs(v):
    best, bi = -1, 0
    for i in range(3):
        a = abs(v[i])
        if a > best:
            best, bi = a, i
    return bi


class WindowTokenizer:
    def __init__(self):
        self.reset()

    def reset(self):
        self.buf = []
        self.prev_sum = None
        self.prev_last = None

    def push(self, x, y, z):
        """Add one sample; returns a token when a window completes, else None."""
        self.buf.append((int(x), int(y), int(z)))
        if len(self.buf) < WIN:
            return None
        w = self.buf
        self.buf = []
        s = [w[0][k] + w[1][k] + w[2][k] for k in range(3)]
        last = self.prev_last if self.prev_last is not None else w[0]
        e = 0
        eax = [0, 0, 0]
        for i in range(WIN):
            for k in range(3):
                dd = abs(w[i][k] - last[k])
                e += dd
                eax[k] += dd
            last = w[i]
        if self.prev_sum is None:
            d = [0, 0, 0]
        else:
            d = [s[k] - self.prev_sum[k] for k in range(3)]
        self.prev_sum = s
        self.prev_last = w[WIN - 1]

        oax = _argmax_abs(s)
        orient = oax * 2 + (1 if s[oax] < 0 else 0)
        dax = _argmax_abs(d)
        dinf = abs(d[dax])
        if dinf < STILL_D and e < STILL_E:
            return STILL0 + orient
        if e >= JOLT_E and 6 * e > 5 * dinf:
            jax = _argmax_abs(eax)
            return JOLT0 + jax * 2 + (1 if e >= JOLT_E2 else 0)
        direction = dax * 2 + (1 if d[dax] < 0 else 0)
        mag = 1 if dinf >= BIG_D else 0
        return MOVE0 + (direction * 2 + mag) * 6 + orient


class Segmenter:
    """Cuts the token stream into candidate gestures (see NOTES.md)."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.active = False
        self.pre = []
        self.seq = []
        self.still_run = 0

    def push(self, t):
        """Returns ('start', seq) when a segment opens (seq = pre-roll + t),
        ('tok', t) while it runs, ('end', seq) when it closes, else None."""
        if not self.active:
            if is_still(t):
                self.pre.append(t)
                if len(self.pre) > PRE:
                    self.pre.pop(0)
                return None
            self.active = True
            self.seq = self.pre + [t]
            self.pre = []
            self.still_run = 0
            return ("start", list(self.seq))
        self.seq.append(t)
        self.still_run = self.still_run + 1 if is_still(t) else 0
        if self.still_run >= END_STILL or len(self.seq) >= MAX_LEN:
            seq = self.seq
            self.active = False
            self.seq = []
            self.still_run = 0
            return ("end", seq)
        return ("tok", t)


def tokenize_trace(samples):
    tk = WindowTokenizer()
    out = []
    for x, y, z in samples:
        t = tk.push(x, y, z)
        if t is not None:
            out.append(t)
    return out


def segments(tokens):
    sg = Segmenter()
    out = []
    for t in tokens:
        r = sg.push(t)
        if r and r[0] == "end":
            out.append(r[1])
    return out


def trace_segments(samples):
    return segments(tokenize_trace(samples))


def model_input(seg):
    """Token ids fed to the LM; the label is predicted after SEP."""
    return [BOS] + list(seg) + [SEP]
