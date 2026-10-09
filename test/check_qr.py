#!/usr/bin/env python3
"""Decode the rendered QR cards with OpenCV and compare to the exact URLs.
  check_qr.py <outdir> [--control]   (--control: swapped expectations, must fail)"""
import sys
from pathlib import Path
import cv2, zxingcpp
from PIL import Image
out = Path(sys.argv[1])
want = {"4_qr.png": "https://maps.app.goo.gl/LDtHCnFEXopYkxdy5", "5_qr.png": "https://clairewills.co.uk"}
if "--control" in sys.argv: want = dict(zip(want, reversed(list(want.values()))))
bad = 0
for f, url in want.items():
    # Two independent decoders: zxing-cpp on the exact render, OpenCV on a
    # phone-camera-ish copy (3x up, blurred). OpenCV can assert-crash on an
    # unreadable code; that counts as a failed decode.
    r = zxingcpp.read_barcodes(Image.open(out / f))
    got = r[0].text if r else None
    img = cv2.imread(str(out / f))
    big = cv2.GaussianBlur(cv2.resize(img, None, fx=3, fy=3, interpolation=cv2.INTER_NEAREST), (5, 5), 0)
    try: got2, *_ = cv2.QRCodeDetector().detectAndDecode(big)
    except cv2.error as e: got2 = f"<opencv error>"
    ok = got == url and got2 == url
    bad += not ok
    print(f"{f}: decoded {got!r} / blurred {got2!r} -> {'ok' if ok else 'MISMATCH, want ' + url}")
sys.exit(1 if bad else 0)
