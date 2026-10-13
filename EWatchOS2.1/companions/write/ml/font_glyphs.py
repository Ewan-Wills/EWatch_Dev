"""Synthetic training glyphs from installed fonts (style coverage: double-storey
a/g, serif 1, crossed 7/Z, script capitals...). Each glyph is rendered large,
skeletonised to a 1-px centre line, and redrawn with the watch's own brush and
box fit (common.rasterise), with finger-style jitter. Letters keep their case
label for the 62-class map but train as the 36 merged classes.

  font_glyphs.py OUT.npz [REPS]
"""
import glob, sys
import numpy as np
from multiprocessing import Pool
from PIL import Image, ImageDraw, ImageFont
from skimage.morphology import skeletonize
from common import CLASSES, rasterise

CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
SKIP = ("Emoji", "Symbol", "Wingding", "Webding", "Zapf", "Bodoni Ornaments", "LastResort",
        "Apple Braille", "Keyboard", "Mono", "Courier", "Menlo", "Monaco", "Andale", "Stencil",
        "Smallcaps", "SC", "Engraved", "Bodoni 72 Smallcaps")


def fonts():
    out = []
    for f in sorted(glob.glob("/System/Library/Fonts/*.tt?") + glob.glob("/System/Library/Fonts/Supplemental/*.tt?")
                    + glob.glob("/Library/Fonts/*.tt?")):
        if any(s in f for s in SKIP):
            continue
        out.append(f)
    return out


def glyph_skeleton(font, ch):
    img = Image.new("L", (160, 160), 0)
    d = ImageDraw.Draw(img)
    d.text((30, 10), ch, font=font, fill=255)
    a = np.asarray(img) > 128
    if a.sum() < 30:
        return None
    sk = skeletonize(a)
    ys, xs = np.nonzero(sk)
    if len(xs) < 8:
        return None
    return np.stack([xs, ys], 1).astype(np.float32)


def ok_font(path):
    """Latin glyphs present and distinct (not a .notdef box for everything)."""
    try:
        font = ImageFont.truetype(path, 100)
    except Exception:
        return None
    sk = [glyph_skeleton(font, c) for c in "AaGg7"]
    if any(s is None for s in sk):
        return None
    sizes = {len(s) for s in sk}
    return path if len(sizes) > 2 else None


def render(job):
    path, reps, seed = job
    font = ImageFont.truetype(path, 100)
    r = np.random.default_rng(seed)
    X, Y = [], []
    for ch in CHARS:
        pts = glyph_skeleton(font, ch)
        if pts is None:
            continue
        lab = CLASSES.index(ch.upper())
        c = (pts.min(0) + pts.max(0)) / 2
        for _ in range(reps):
            th = np.deg2rad(r.uniform(-10, 10)); sh = r.uniform(-0.25, 0.25); asp = np.exp(r.uniform(-0.15, 0.15))
            M = np.array([[np.cos(th), -np.sin(th)], [np.sin(th), np.cos(th)]]) @ np.array([[1, sh], [0, 1]]) @ np.diag([asp, 1 / asp])
            q = (pts - c) @ M.T
            # skeleton pixels as zero-length segments: the brush covers the line
            img = rasterise([p[None] for p in q[r.random(len(q)) < 0.6]] or [q[:1]], r.uniform(0.75, 1.7))
            X.append((img * 255).astype(np.uint8)); Y.append(lab)
    return X, Y


def main():
    out = sys.argv[1]
    reps = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    with Pool(8) as p:
        good = [f for f in p.map(ok_font, fonts()) if f]
        print("fonts", len(good))
        res = p.map(render, [(f, reps, i) for i, f in enumerate(good)])
    X = np.stack([x for xs, _ in res for x in xs]); Y = np.array([y for _, ys in res for y in ys])
    np.savez_compressed(out, x=X, y=Y)
    print(out, X.shape)
    Image.fromarray(np.concatenate(list(X[::max(1, len(X) // 60)][:60]), 1)).resize((60 * 56, 56), 0).save("data/font_preview.png")


if __name__ == "__main__":
    main()
