#!/usr/bin/env python3
"""Cross-check the firmware's JPEG decode against Pillow, frame by frame, and
turn the host test's raw RGB565 dumps into PNGs (+ a contact sheet).

Reads the JPEG bytes straight out of src/frames_data.h (what ships in flash)
and the pixels host_test decoded with JPEGDEC. Fails if any frame differs by
more than a small IDCT rounding tolerance (see GalleryFace/test/check_decode.py
for why luma + 8x8 block means, not raw RGB).
  check_frames.py <outdir> [--control]   (--control: expect failure on a tampered dump)
"""
import io, re, sys
from pathlib import Path
import numpy as np
from PIL import Image

W, H = 240, 280
out = Path(sys.argv[1]); control = "--control" in sys.argv
src = (Path(__file__).resolve().parent.parent / "src/frames_data.h").read_text()
start = src.index("FRAME_JPEG[] = {") + 16
body = src[start: src.index("};", start)]
blob = np.array(body.replace("\n", "").strip().rstrip(",").split(","), dtype=np.uint8).tobytes()
idx = [tuple(int(x) for x in m) for m in re.findall(r"\{(\d+), (\d+), (\d+)\},", src)]
dec = np.fromfile(out / "decoded.bin", dtype="<u2")
assert len(dec) == len(idx) * W * H, "dump size mismatch"
if control:                                   # fake one frame (#200) going black halfway down
    dec = dec.copy(); dec[200 * W * H + 140 * W: 201 * W * H] = 0

def rgb(px):
    px = px.astype(np.int32)
    return np.stack([(px >> 11) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1)

luma = lambda a: (a * [0.299, 0.587, 0.114]).sum(-1)
blk = lambda a: a.reshape(H // 8, 8, W // 8, 8, 3).mean((1, 3))
worst, bad = 0.0, []
for i, (off, ln, layer) in enumerate(idx):
    ref = np.asarray(Image.open(io.BytesIO(blob[off:off + ln])).convert("RGB"), dtype=np.int32)
    assert ref.shape[:2] == (H, W), (i, ref.shape)
    me = rgb(dec[i * W * H:(i + 1) * W * H].reshape(H, W))
    err = max(np.abs(luma(me) - luma(ref)).mean(), np.abs(blk(me) - blk(ref)).mean())
    worst = max(worst, err)
    if err > 4: bad.append((i, round(float(err), 1)))
print(f"{len(idx)} frames compared, worst error (max of luma MAE, 8x8 block MAE) {worst:.2f}/255, {len(bad)} bad {bad[:5]}")

if not control:
    shots = sorted(out.glob("*.raw"))
    ims = []
    for r in shots:
        im = Image.fromarray(rgb(np.fromfile(r, dtype="<u2").reshape(H, W)).astype(np.uint8))
        im.save(r.with_suffix(".png")); r.unlink(); ims.append(im)
    if ims:
        cols = 4; rows = (len(ims) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (W + 8), rows * (H + 8)), (40, 40, 40))
        for k, im in enumerate(ims):
            sheet.paste(im, ((k % cols) * (W + 8) + 4, (k // cols) * (H + 8) + 4))
        sheet.save(out / "sheet.png")
sys.exit(1 if bad else 0)
