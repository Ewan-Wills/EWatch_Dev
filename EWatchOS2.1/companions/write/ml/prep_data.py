"""Build training/test arrays (uint8 28x28) for the Write CNN.

  data/train.npz : EMNIST byclass (case merged, capped per class) + UJI trn
                   writers, each rendered many times with finger-style jitter
  data/test_uji.npz   : UJI tst writers (20 people never seen in training),
                        rendered once, no augmentation  <- the honest number
  data/test_emnist.npz: EMNIST byclass test, case merged, 20k sample
"""
import gzip, os, sys
import numpy as np
from multiprocessing import Pool
from PIL import Image
from common import CLASSES, G, FIT, parse_uji, rasterise, label_of

rng = np.random.default_rng(1)
EM = "data/emnist/gzip"
# EMNIST byclass mapping: 0-9 digits, 10-35 A-Z, 36-61 a-z
EM_MAP = list("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") + list("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
C62 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz"   # 36 merged + 26 lower
EM62 = list("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")


def idx(path):
    with gzip.open(path) as f:
        d = f.read()
    if d[2] == 8 and d[3] == 3:
        n, r, c = np.frombuffer(d[4:16], ">u4")
        return np.frombuffer(d[16:], np.uint8).reshape(n, r, c)
    n = np.frombuffer(d[4:8], ">u4")[0]
    return np.frombuffer(d[8:], np.uint8)


def refit(img):
    """EMNIST image -> the watch's box fit (longest side FIT px, centred)."""
    a = img.T.astype(np.float32) / 255.0          # EMNIST is stored transposed
    ys, xs = np.nonzero(a > 0.25)
    if len(xs) == 0:
        return None
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    crop = a[y0:y1, x0:x1]
    h, w = crop.shape
    s = FIT / max(h, w)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    im = Image.fromarray((crop * 255).astype(np.uint8)).resize((nw, nh), Image.BILINEAR)
    out = np.zeros((G, G), np.uint8)
    ox, oy = (G - nw) // 2, (G - nh) // 2
    out[oy:oy + nh, ox:ox + nw] = np.asarray(im)
    return out


def emnist(split, cap, seed):
    X = idx(f"{EM}/emnist-byclass-{split}-images-idx3-ubyte.gz")
    Y = idx(f"{EM}/emnist-byclass-{split}-labels-idx1-ubyte.gz")
    lab = np.array([CLASSES.index(EM_MAP[y]) for y in Y])
    lab62 = np.array([C62.index(EM62[y]) for y in Y])
    r = np.random.default_rng(seed)
    keep = []
    for c in range(len(CLASSES)):
        ii = np.nonzero(lab == c)[0]
        r.shuffle(ii)
        keep.extend(ii[:cap])
    keep = np.array(keep)
    with Pool(12) as p:
        ims = p.map(refit, [X[i] for i in keep], chunksize=2000)
    ok = [i for i, m in enumerate(ims) if m is not None]
    return np.stack([ims[i] for i in ok]), lab[keep][ok], lab62[keep][ok]


def augment(strokes, r):
    """Finger-style variation of one pen sample (see NOTES)."""
    pts = np.concatenate(strokes, 0)
    c = (pts.min(0) + pts.max(0)) / 2
    h = max(np.ptp(pts[:, 0]), np.ptp(pts[:, 1]), 1.0)
    th = np.deg2rad(r.uniform(-12, 12))
    sh = r.uniform(-0.3, 0.3)
    asp = np.exp(r.uniform(-0.2, 0.2))
    M = np.array([[np.cos(th), -np.sin(th)], [np.sin(th), np.cos(th)]]) @ \
        np.array([[1, sh], [0, 1]]) @ np.diag([asp, 1 / asp])
    out = []
    for st in strokes:
        q = (st - c) @ M.T
        q = q + r.normal(0, 0.05 * h, 2) * (len(strokes) > 1)      # stroke placement
        # finger sampling: keep every k-th point (chords), always the ends
        k = r.integers(1, 4)
        if len(q) > 2 and k > 1:
            q = np.concatenate([q[:-1:k], q[-1:]], 0)
        q = q + r.normal(0, 0.008 * h, q.shape)                    # jitter
        out.append(q)
    return out


def uji_render(job):
    strokes, seed, aug = job
    r = np.random.default_rng(seed)
    if aug:
        return (rasterise(augment(strokes, r), r.uniform(0.75, 1.7)) * 255).astype(np.uint8)
    return (rasterise(strokes) * 255).astype(np.uint8)


ALL = os.environ.get("ALLWRITERS") == "1"      # final model: train on all 60 writers
OUT = "data/train_all.npz" if ALL else "data/train.npz"


def main():
    reps = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    uji = [(label_of(ch), w, s, C62.index(ch)) for ch, w, s in parse_uji("data/uji/ujipenchars2.txt") if label_of(ch) is not None]
    trn = [u for u in uji if u[1].startswith("trn") or ALL]
    tst = [u for u in uji if u[1].startswith("tst")]
    print("uji trn", len(trn), "tst", len(tst))
    with Pool(12) as p:
        jobs = [(s, i * 1000 + k, True) for i, (_, _, s, _) in enumerate(trn) for k in range(reps)]
        Xu = np.stack(p.map(uji_render, jobs, chunksize=500))
        Yu = np.repeat([l for l, _, _, _ in trn], reps)
        Yu62 = np.repeat([l62 for _, _, _, l62 in trn], reps)
        Xt = np.stack(p.map(uji_render, [(s, 0, False) for _, _, s, _ in tst]))
    Yt = np.array([l for l, _, _, _ in tst])
    np.savez_compressed("data/test_uji.npz", x=Xt, y=Yt, w=np.array([w for _, w, _, _ in tst]))
    Xe, Ye, Ye62 = emnist("train", 6000, 2)
    print("emnist train", Xe.shape)
    np.savez_compressed(OUT, x=np.concatenate([Xe, Xu]), y=np.concatenate([Ye, Yu]),
                        y62=np.concatenate([Ye62, Yu62]),
                        src=np.concatenate([np.zeros(len(Ye), np.uint8), np.ones(len(Yu), np.uint8)]))
    Xet, Yet, _ = emnist("test", 600, 3)
    np.savez_compressed("data/test_emnist.npz", x=Xet, y=Yet)
    print("done", Xe.shape, Xu.shape, Xt.shape, Xet.shape)
    # preview strip
    pick = np.concatenate([Xe[:20], Xu[::reps][:20], Xt[:20]])
    Image.fromarray(np.concatenate(list(pick), 1)).resize((60 * 56, 56), 0).save("data/preview.png")


if __name__ == "__main__":
    main()
