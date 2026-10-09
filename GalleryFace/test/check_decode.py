#!/usr/bin/env python3
"""Cross-check the firmware's JPEG decode against Pillow, painting by painting.

Reads the JPEG bytes straight out of src/paintings_data.h (what ships in
flash) and the pixels host_test decoded with JPEGDEC. Fails if any painting
differs by more than a small IDCT rounding tolerance.
  check_decode.py <outdir> [--control]   (--control: expect failure on a tampered dump)
"""
import io, re, sys
from pathlib import Path
import numpy as np
from PIL import Image

out = Path(sys.argv[1]); control = "--control" in sys.argv
src = (Path(__file__).resolve().parent.parent / "src/paintings_data.h").read_text()
start = src.index("PAINTING_JPEG[] = {") + 19
body = src[start: src.index("};", start)]
blob = np.array(body.replace("\n", "").strip().rstrip(",").split(","), dtype=np.uint8).tobytes()
idx = [tuple(int(x) for x in m) for m in re.findall(r"\{(\d+), (\d+), (\d+), \"", src)]
dec = np.fromfile(out / "decoded.bin", dtype="<u2")
if control: dec = dec.copy(); dec[len(dec) // 2: len(dec) // 2 + 240 * 50] = 0x4208   # fake one flat tile
pos, worst, bad = 0, 0.0, []
for i, (off, ln, h) in enumerate(idx):
    im = np.asarray(Image.open(io.BytesIO(blob[off:off + ln])).convert("RGB"), dtype=np.int32)
    assert im.shape[:2] == (h, 240), (i, im.shape)
    px = dec[pos:pos + 240 * h].reshape(h, 240).astype(np.int32); pos += 240 * h
    r = (px >> 11) * 255 // 31; g = ((px >> 5) & 63) * 255 // 63; b = (px & 31) * 255 // 31
    me = np.stack([r, g, b], -1)
    # Per-pixel luma + 8x8 block colour. Raw per-pixel RGB is NOT used:
    # JPEGDEC upsamples 4:2:0 chroma without Pillow's smoothing, which gives
    # ~7/255 R/B error on colour edges of a correct decode (measured
    # 2026-09-25: luma 1.3, block 1.7 on those paintings).
    luma = lambda a: (a * [0.299, 0.587, 0.114]).sum(-1)
    hb = h // 8 * 8
    blk = lambda a: a[:hb].reshape(hb // 8, 8, 30, 8, 3).mean((1, 3))
    err = max(np.abs(luma(me) - luma(im)).mean(), np.abs(blk(me) - blk(im)).mean())
    worst = max(worst, err)
    if err > 4: bad.append((i, round(float(err), 1)))
assert pos == len(dec), "dump size mismatch"
print(f"{len(idx)} paintings compared, worst error (max of luma MAE, 8x8 block MAE) {worst:.2f}/255, {len(bad)} bad {bad[:5]}")
sys.exit(1 if bad else 0)
