#!/usr/bin/env python3
"""Measure where the hands point in the host-rendered frames.

    check_hands.py <outdir>

For every t_HHMMSS.png it finds the blue (hand) pixels that differ from
dial.png and takes the mean direction of those in the minute hand's outer
part (only the minute hand is that long) and of those in the hour hand's
belly (minute hand masked out by its measured direction). Both must match the
time in the file name. Independent of tank_draw.cpp: it knows only the hand
lengths and that hands are blue.
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CX, CY = 120.0, 140.0
TOL_MIN, TOL_HOUR = 0.8, 1.0            # degrees
NEED_MIN, NEED_HOUR = 90, 60            # frames that must actually be measured


def circ_mean(deg):
    r = np.radians(deg)
    return math.degrees(math.atan2(np.sin(r).mean(), np.cos(r).mean())) % 360


def diff(a, b):
    return abs((a - b + 180) % 360 - 180)


def main():
    out = Path(sys.argv[1])
    dial = np.asarray(Image.open(out / "dial.png").convert("RGB"), dtype=np.int32)
    y, x = np.mgrid[0:dial.shape[0], 0:dial.shape[1]] + 0.5
    rad = np.hypot(x - CX, y - CY)
    ang = np.degrees(np.arctan2(x - CX, -(y - CY))) % 360

    bad, n_min, n_hour, worst_min, worst_hour = [], 0, 0, 0.0, 0.0
    files = sorted(out.glob("t_*.png"))
    for f in files:
        hh, mm, ss = int(f.stem[2:4]), int(f.stem[4:6]), int(f.stem[6:8])
        want_min = (mm + ss / 60) * 6 % 360
        want_hour = ((hh % 12) + mm / 60 + ss / 3600) * 30 % 360
        img = np.asarray(Image.open(f).convert("RGB"), dtype=np.int32)
        hand = (np.abs(img - dial).sum(axis=2) > 0) & (img[:, :, 2] - img[:, :, 0] > 40)
        if hand.sum() < 150:
            bad.append((f.name, "no hands drawn (%d blue px)" % hand.sum()))
            continue
        tip = hand & (rad > 48) & (rad <= 67)       # the hour hand ends at r=45
        if tip.sum() < 10:
            bad.append((f.name, "no minute hand beyond r=48"))
            continue
        got_min = circ_mean(ang[tip])
        n_min += 1
        worst_min = max(worst_min, diff(got_min, want_min))
        if diff(got_min, want_min) > TOL_MIN:
            bad.append((f.name, "minute hand at %.1f, want %.1f" % (got_min, want_min)))
        # Hour hand: skip frames where the two hands are too close to separate.
        off_min = (np.abs((ang - got_min + 180) % 360 - 180) > 14)
        belly = hand & (rad >= 28) & (rad <= 44) & off_min
        if belly.sum() < 20:
            continue
        got_hour = circ_mean(ang[belly])
        if diff(got_hour, got_min) < 30:
            continue
        n_hour += 1
        worst_hour = max(worst_hour, diff(got_hour, want_hour))
        if diff(got_hour, want_hour) > TOL_HOUR:
            bad.append((f.name, "hour hand at %.1f, want %.1f" % (got_hour, want_hour)))

    print("hands: %d frames, minute measured in %d (worst %.2f deg), hour in %d (worst %.2f deg)"
          % (len(files), n_min, worst_min, n_hour, worst_hour))
    if n_min < NEED_MIN or n_hour < NEED_HOUR:
        bad.append(("coverage", "measured too few frames (need %d minute, %d hour)" % (NEED_MIN, NEED_HOUR)))
    if bad:
        print("HANDS MISMATCH: %d bad %s" % (len(bad), bad[:6]))
        sys.exit(1)
    print("hands ok")


if __name__ == "__main__":
    main()
