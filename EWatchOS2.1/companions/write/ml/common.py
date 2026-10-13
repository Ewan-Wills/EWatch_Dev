"""Shared bits: class list, UJI parser, and the stroke rasteriser.

The rasteriser here is the reference for the C one in the firmware
(write_cnn.cpp: wcnn::rasterise). Keep the two identical: same box fit,
same brush, same formula.
"""
import numpy as np

CLASSES = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
CIDX = {c: i for i, c in enumerate(CLASSES)}
G = 28          # image side
FIT = 20.0      # longest side of the glyph box, in pixels
RADIUS = 1.1    # brush radius (pixels) used on the watch


def label_of(ch):
    """UJI/EMNIST character -> class index, or None (accents, punctuation)."""
    u = ch.upper()
    return CIDX.get(u) if len(u) == 1 else None


def parse_uji(path):
    """-> list of (char, writer, strokes); strokes = [np.array (n,2) float]."""
    out = []
    lines = open(path, encoding="latin-1").read().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith("WORD"):
            parts = ln.split()
            ch, wid = parts[1], parts[2]
            ns = int(lines[i + 1].split()[1])
            strokes = []
            for k in range(ns):
                toks = lines[i + 2 + k].split("#")[1].split()
                a = np.array(toks, dtype=np.float32).reshape(-1, 2)
                strokes.append(a)
            out.append((ch, wid, strokes))
            i += 2 + ns
        else:
            i += 1
    return out


def rasterise(strokes, radius=RADIUS):
    """Strokes (list of (n,2) arrays, any units, y down) -> 28x28 float32 in [0,1].

    Box fit: the glyph's bounding box is scaled uniformly so its longest side
    is FIT px, then centred in the 28x28 grid. Brush: pixel value
    clamp(radius + 0.5 - d, 0, 1), d = distance from the pixel centre to the
    nearest stroke segment (a single point counts as a zero-length segment).
    """
    pts = np.concatenate(strokes, 0)
    mn, mx = pts.min(0), pts.max(0)
    size = max(mx[0] - mn[0], mx[1] - mn[1], 1e-6)
    s = FIT / size
    cx, cy = (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2
    segs = []
    for st in strokes:
        q = (st - [cx, cy]) * s + G / 2.0
        if len(q) == 1:
            segs.append(np.concatenate([q, q], 1))
        else:
            segs.append(np.concatenate([q[:-1], q[1:]], 1))
    seg = np.concatenate(segs, 0)                       # (m,4) x0 y0 x1 y1
    yy, xx = np.mgrid[0:G, 0:G].astype(np.float32) + 0.5
    P = np.stack([xx.ravel(), yy.ravel()], 1)           # (784,2)
    a = seg[:, :2][None]; b = seg[:, 2:][None]
    ab = b - a
    L2 = (ab ** 2).sum(-1)
    t = ((P[:, None] - a) * ab).sum(-1) / np.maximum(L2, 1e-9)
    t = np.clip(t, 0, 1)
    proj = a + t[..., None] * ab
    d = np.sqrt(((P[:, None] - proj) ** 2).sum(-1)).min(1)
    img = np.clip(radius + 0.5 - d, 0, 1).reshape(G, G)
    return img.astype(np.float32)
