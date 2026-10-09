#!/usr/bin/env python3
"""Bake the TankFace dial into src/dial_data.h (240x280 RGB565, dithered).

    uv run --with pillow --with numpy tools/build_dial.py

The dial is a rectangular Tank-style dial: radial Roman numerals (IIII at
four), a chemin-de-fer minute track inside them, a silvered ground with a
faint sunray in the centre. Everything static is drawn here at 4x and
box-filtered down, so the watch only has to draw the hands.

Geometry shared with src/tank_draw.cpp and test/check_dial.py: screen centre
(120,140); an angle is clockwise from 12; a "rect ray" is the distance from
the centre to a centred rectangle along that angle. Numerals and ticks sit at
their true hour/minute angles, so a hand at angle A points at mark A.
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, SS = 240, 280, 4
CX, CY = W / 2, H / 2

NUM_OUT = (107.0, 127.0)   # half-size of the numerals' outer edge
NUM_IN = (73.0, 93.0)      # ... and of their feet
TRACK_OUT = (69.0, 89.0)   # minute track, outer line
TRACK_IN = (62.0, 82.0)    # minute track, inner line (edge of the centre field)

DEG_PER_UNIT = 5.2         # one numeral stroke cell
THICK, THIN, SERIF = 0.46, 0.14, 0.055
NUMERALS = ["XII", "I", "II", "III", "IIII", "V", "VI", "VII", "VIII", "IX", "X", "XI"]

GROUND = np.array([240.0, 236.0, 224.0])
INK = np.array([24.0, 21.0, 19.0])
SIGNATURE = "EWATCH"
FONTS = ["/System/Library/Fonts/Supplemental/Baskerville.ttc",
         "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
         "/System/Library/Fonts/Times.ttc"]


def ray(theta, half):
    s, c = abs(math.sin(math.radians(theta))), abs(math.cos(math.radians(theta)))
    return min(half[0] / s if s > 1e-9 else 1e9, half[1] / c if c > 1e-9 else 1e9)


def polar(theta, r):
    t = math.radians(theta)
    return (CX + r * math.sin(t), CY - r * math.cos(t))


def band(theta, v):
    """v = 0 on the numerals' feet (towards the centre), 1 on their tops."""
    return polar(theta, ray(theta, NUM_IN) + v * (ray(theta, NUM_OUT) - ray(theta, NUM_IN)))


# ---- numerals: flat polygons in (stroke cells, v), then bent onto the dial --
def quad_v(x_top, x_bot, t):          # a stroke from the top to the foot
    return [(x_top - t / 2, 1), (x_top + t / 2, 1), (x_bot + t / 2, 0), (x_bot - t / 2, 0)]


def serif(x, v_top, half=0.42):
    return [(x - half, v_top), (x + half, v_top), (x + half, v_top - SERIF), (x - half, v_top - SERIF)]


def glyph(ch):
    """-> (advance, [polygons]). Thick strokes run top-left to bottom-right."""
    if ch == "I":
        return 1.0, [quad_v(0.5, 0.5, THICK), serif(0.5, 1), serif(0.5, SERIF)]
    if ch == "V":
        return 2.0, [quad_v(0.44, 1.0, THICK), quad_v(1.56, 1.0, THIN), serif(0.44, 1), serif(1.56, 1, 0.36)]
    if ch == "X":
        return 2.0, [quad_v(0.44, 1.56, THICK), quad_v(1.56, 0.44, THIN),
                     serif(0.44, 1), serif(1.56, 1, 0.36), serif(0.44, SERIF, 0.36), serif(1.56, SERIF)]
    raise ValueError(ch)


def subdivide(poly, step=0.04):
    out = []
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        n = max(1, int(math.ceil(max(abs(x1 - x0), abs(y1 - y0)) / step)))
        out += [(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n) for i in range(n)]
    return out


def numeral_polys(hour_index):
    polys, x = [], 0.0
    for ch in NUMERALS[hour_index]:
        adv, ps = glyph(ch)
        polys += [[(px + x, pv) for px, pv in p] for p in ps]
        x += adv
    centre = hour_index * 30.0
    half_span = x * DEG_PER_UNIT / 2
    # A numeral must not run round a corner of the band: squeeze it if needed.
    corner = math.degrees(math.atan2(NUM_IN[0], NUM_IN[1]))
    for c in (corner, 180 - corner, 180 + corner, 360 - corner):
        d = abs(centre - c)
        if d < 30:
            half_span = min(half_span, d - 0.9)
    out = []
    for p in polys:
        out.append([band(centre + (px / x - 0.5) * 2 * half_span, pv) for px, pv in subdivide(p)])
    return out


# ---- minute track -----------------------------------------------------------
def track_tick(minute):
    """The tick for a minute: a bar across the track where the ray meets it."""
    theta = minute * 6.0
    mid = ((TRACK_IN[0] + TRACK_OUT[0]) / 2, (TRACK_IN[1] + TRACK_OUT[1]) / 2)
    x, y = polar(theta, ray(theta, mid))
    w = 1.9 if minute % 5 == 0 else 0.5            # half-width along the track
    s, c = abs(math.sin(math.radians(theta))), abs(math.cos(math.radians(theta)))
    if mid[1] / max(c, 1e-9) < mid[0] / max(s, 1e-9):   # top or bottom edge
        sy = -1 if y < CY else 1
        return [(x - w, CY + sy * TRACK_IN[1]), (x + w, CY + sy * TRACK_IN[1]),
                (x + w, CY + sy * TRACK_OUT[1]), (x - w, CY + sy * TRACK_OUT[1])]
    sx = -1 if x < CX else 1
    return [(CX + sx * TRACK_IN[0], y - w), (CX + sx * TRACK_IN[0], y + w),
            (CX + sx * TRACK_OUT[0], y + w), (CX + sx * TRACK_OUT[0], y - w)]


def ink_mask():
    img = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(img)

    def poly(p):
        d.polygon([(x * SS, y * SS) for x, y in p], fill=255)

    for h in range(12):
        for p in numeral_polys(h):
            poly(p)
    for half in (TRACK_IN, TRACK_OUT):
        d.rectangle([(CX - half[0]) * SS - 2, (CY - half[1]) * SS - 2,
                     (CX + half[0]) * SS + 1, (CY + half[1]) * SS + 1], outline=255, width=SS)
    for m in range(60):
        poly(track_tick(m))

    font = next((ImageFont.truetype(f, 15 * SS) for f in FONTS if Path(f).exists()), None)
    if font is None:
        sys.exit("no serif font found for the signature (see FONTS)")
    gap = 2.6 * SS
    widths = [d.textlength(ch, font=font) for ch in SIGNATURE]
    x = CX * SS - (sum(widths) + gap * (len(SIGNATURE) - 1)) / 2
    for ch, w in zip(SIGNATURE, widths):
        d.text((x, 93 * SS), ch, font=font, fill=255, anchor="ls")
        x += w + gap
    return np.asarray(img.reduce(SS), dtype=np.float64) / 255.0


def ground():
    rng = np.random.default_rng(1917)                  # the Tank's year; fixed = reproducible
    y, x = np.mgrid[0:H, 0:W] + 0.5
    dx, dy = x - CX, y - CY
    lum = 1.0 - 0.075 * ((dx / CX) ** 2 + (dy / CY) ** 2) ** 1.2      # vignette
    lum += 0.018 * (1.0 - (x + y) / (W + H))                           # light from the top left
    lum += rng.normal(0.0, 0.006, (H, W))                              # silvered grain
    r = np.hypot(dx, dy)
    centre_field = (np.abs(dx) < TRACK_IN[0]) & (np.abs(dy) < TRACK_IN[1])
    rays = np.cos(np.arctan2(dx, -dy) * 48) * np.clip((r - 10) / 30, 0, 1)
    lum += np.where(centre_field, 0.013 * rays, 0.0)                   # faint sunray guilloche
    return GROUND[None, None, :] * lum[:, :, None]


def main():
    a = ink_mask()[:, :, None]
    rgb = np.clip(ground() * (1 - a) + INK[None, None, :] * a, 0, 255)
    rng = np.random.default_rng(565)
    q = lambda ch, levels: np.clip(np.floor(rgb[:, :, ch] / 255.0 * levels + rng.random((H, W))), 0, levels).astype(np.uint16)
    px = (q(0, 31) << 11) | (q(1, 63) << 5) | q(2, 31)

    out = Path(__file__).resolve().parent.parent / "src" / "dial_data.h"
    lines = ["// Generated by tools/build_dial.py - do not edit.",
             "// The TankFace dial, 240x280 RGB565, row-major from the top left.",
             "#pragma once", "#include <stdint.h>", "",
             "static const int kDialW = %d, kDialH = %d;" % (W, H),
             "static const uint16_t kDialPixels[%d] = {" % (W * H)]
    flat = px.reshape(-1)
    for i in range(0, len(flat), 16):
        lines.append("  " + ",".join("0x%04X" % v for v in flat[i:i + 16]) + ",")
    lines.append("};")
    out.write_text("\n".join(lines) + "\n")
    print("wrote %s (%d px, %d B in flash)" % (out, W * H, W * H * 2))


if __name__ == "__main__":
    main()
