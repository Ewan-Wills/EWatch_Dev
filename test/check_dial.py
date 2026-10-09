#!/usr/bin/env python3
"""Check the baked dial's minute track against true minute angles.

    check_dial.py <outdir> [--control]

Reads dial.png (the dial as the firmware draws it). For each of the 60
minutes the ray at 6 degrees x minute must cross a dark tick in the middle of
the track, and the ray half-way to the next minute must cross clear dial.
Five-minute marks must be wider than the others. So a hand at a minute's
angle points at that minute's tick. --control shifts every angle by half a
minute and has to fail.
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CX, CY = 120.0, 140.0
MID = (65.5, 85.5)                       # middle of the track (between its two lines)


def on_track(theta, along=0.0):
    """Pixel where the ray meets the middle of the track, moved `along` it."""
    s, c = math.sin(math.radians(theta)), math.cos(math.radians(theta))
    top = MID[1] / max(abs(c), 1e-9) < MID[0] / max(abs(s), 1e-9)
    r = MID[1] / abs(c) if top else MID[0] / abs(s)
    x, y = CX + r * s, CY - r * c
    return (int(x + along), int(y)) if top else (int(x), int(y + along))


def main():
    out = Path(sys.argv[1])
    shift = 3.0 if "--control" in sys.argv else 0.0
    lum = np.asarray(Image.open(out / "dial.png").convert("L"), dtype=np.int32)
    px = lambda p: lum[p[1], p[0]]
    bad = []
    for m in range(60):
        a = m * 6.0 + shift
        if px(on_track(a)) > 110:
            bad.append("minute %d: no tick on its ray (lum %d)" % (m, px(on_track(a))))
        if px(on_track(a + 3.0)) < 170:
            bad.append("minute %d.5: dial not clear between ticks (lum %d)" % (m, px(on_track(a + 3.0))))
        wide = px(on_track(a, 1.5)) < 150 and px(on_track(a, -1.5)) < 150
        if wide != (m % 5 == 0):
            bad.append("minute %d: %s mark" % (m, "wide" if wide else "narrow"))
    if bad:
        print("DIAL MISMATCH: %d bad %s" % (len(bad), bad[:5]))
        sys.exit(1)
    print("dial ok: 60 ticks on their rays, 12 wide five-minute marks")


if __name__ == "__main__":
    main()
