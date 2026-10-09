#!/usr/bin/env python3
"""Glyph shapes for the Write app's handwriting recogniser.

One source of truth for two things:
  1. the built-in templates compiled into the firmware
     (`python3 tools/write_glyphs.py header` -> src/apps/write/write_templates.h)
  2. the synthetic finger draws used to test it
     (`python3 tools/write_glyphs.py tests` -> test/write_*.ewt)

Shapes are hand-authored uppercase letters and digits on a grid where the glyph
is 100 units tall (y grows DOWN, like the screen) and as wide as it looks.
Each character has 1-4 variants (different shapes people actually write).
The recogniser ($P point cloud) ignores stroke order and direction, so variants
only need to differ in SHAPE; the tests still draw strokes in shuffled orders
and directions to exercise the multi-stroke input path.

Edit gestures (space / backspace / enter) are NOT templates: the firmware
detects them geometrically (a single straight stroke), see write_recog.cpp.
"""
import math, os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# ---------------------------------------------------------------- primitives
def arc(cx, cy, rx, ry, a0, a1, step_deg=15):
    """Elliptical arc, angles in degrees (0 = right, 90 = DOWN). a0 -> a1."""
    n = max(2, int(abs(a1 - a0) / step_deg) + 1)
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


def seq(*parts):
    """Join polylines/points into one stroke (drops duplicated join points)."""
    out = []
    for p in parts:
        pts = [p] if isinstance(p, tuple) else list(p)
        for q in pts:
            if not out or math.dist(out[-1], q) > 0.5:
                out.append(q)
    return out


def L(*pts):
    return list(pts)


# ---------------------------------------------------------------- the shapes
# GLYPHS[ch] = list of variants; variant = list of strokes; stroke = [(x, y)...]
G = {}

G['A'] = [
    [L((0, 100), (30, 0), (60, 100)), L((13, 60), (47, 60))],
    [L((0, 100), (30, 0), (60, 100))],                                   # Graffiti-style, no bar
    [seq(L((0, 100), (21, 8)), arc(30, 12, 10, 10, -150, -30), L((39, 8), (60, 100))), L((12, 62), (50, 62))],  # round apex
]
G['B'] = [
    [L((0, 0), (0, 100)), seq(L((0, 0), (20, 0)), arc(20, 24, 26, 24, -90, 90), L((20, 48), (0, 48)),
                              L((0, 48), (24, 48)), arc(24, 74, 30, 26, -90, 90), L((24, 100), (0, 100)))],
    [L((0, 0), (0, 100)), seq(arc(4, 24, 36, 24, -90, 90), arc(4, 74, 42, 26, -90, 90))],
    [L((0, 0), (0, 100)), seq(arc(4, 25, 30, 25, -90, 90)), seq(arc(4, 75, 38, 25, -90, 90))],  # bumps as separate strokes
]
G['C'] = [
    [arc(36, 50, 36, 50, -45, -315)],
    [arc(40, 50, 40, 50, -60, -300)],
]
G['D'] = [
    [L((0, 0), (0, 100)), arc(4, 50, 52, 50, -90, 90)],
    [L((0, 0), (0, 100)), L((0, 0), (30, 4), (52, 28), (56, 50), (52, 72), (30, 96), (0, 100))],
]
G['E'] = [
    [L((0, 0), (0, 100)), L((0, 0), (52, 0)), L((0, 50), (40, 50)), L((0, 100), (52, 100))],
    [L((52, 0), (0, 0), (0, 100), (52, 100)), L((0, 50), (40, 50))],
    [seq(arc(30, 25, 28, 25, -30, -270), arc(32, 75, 32, 25, -90, -330))],   # curly "Ɛ"
]
G['F'] = [
    [L((0, 0), (0, 100)), L((0, 0), (52, 0)), L((0, 48), (40, 48))],
    [L((52, 0), (0, 0), (0, 100)), L((0, 52), (34, 52))],
]
G['G'] = [
    [seq(arc(36, 50, 36, 50, -45, -360), L((72, 50), (36, 50)))],
    [seq(arc(36, 50, 36, 50, -45, -330), L((67, 75), (69, 54))), L((46, 54), (70, 54))],
    [seq(arc(36, 50, 36, 50, -40, -340), L((70, 66), (70, 46), (40, 46)))],
]
G['H'] = [
    [L((0, 0), (0, 100)), L((60, 0), (60, 100)), L((0, 50), (60, 50))],
]
G['I'] = [
    [L((0, 0), (0, 100))],
    [L((5, 0), (35, 0)), L((20, 0), (20, 100)), L((5, 100), (35, 100))],
]
G['J'] = [
    [seq(L((44, 0), (44, 72)), arc(22, 72, 22, 26, 0, 180))],
    [L((10, 0), (60, 0)), seq(L((44, 0), (44, 72)), arc(22, 72, 22, 26, 0, 180))],
    [seq(L((50, 0), (48, 80)), arc(28, 80, 20, 20, 0, 160))],            # short hook
]
G['K'] = [
    [L((0, 0), (0, 100)), L((55, 0), (0, 55), (55, 100))],
    [L((0, 0), (0, 100)), L((55, 0), (2, 58)), L((20, 42), (58, 100))],
]
G['L'] = [
    [L((0, 0), (0, 100), (52, 100))],
]
G['M'] = [
    [L((0, 100), (0, 0), (36, 64), (72, 0), (72, 100))],
    [L((0, 100), (8, 0), (36, 100), (64, 0), (72, 100))],
    [L((0, 100), (10, 0), (36, 50), (62, 0), (72, 100))],               # splayed legs
]
G['N'] = [
    [L((0, 100), (0, 0), (56, 100), (56, 0))],
]
G['O'] = [
    [arc(38, 50, 38, 50, -90, -450)],
    [arc(40, 50, 40, 50, -60, -430)],
]
G['P'] = [
    [L((0, 0), (0, 100)), seq(L((0, 0), (14, 0)), arc(14, 26, 38, 26, -90, 90), L((14, 52), (0, 52)))],
    [L((0, 0), (0, 100)), arc(4, 26, 46, 26, -90, 90)],
]
G['Q'] = [
    [arc(38, 50, 38, 50, -90, -450), L((44, 70), (78, 104))],
    [arc(38, 50, 38, 50, -90, -450), L((54, 82), (74, 102))],
]
G['R'] = [
    [L((0, 0), (0, 100)), seq(L((0, 0), (14, 0)), arc(14, 26, 36, 26, -90, 90), L((14, 52), (0, 52)), L((0, 52), (16, 52), (54, 100)))],
    [L((0, 0), (0, 100)), seq(arc(4, 26, 44, 26, -90, 90), L((4, 52), (52, 100)))],
]
G['S'] = [
    [seq(arc(30, 25, 30, 25, -20, -270), arc(30, 75, 30, 25, -90, 160))],
    [seq(arc(32, 24, 28, 24, -10, -250), L((22, 46), (40, 54)), arc(30, 76, 30, 24, -70, 170))],
]
G['T'] = [
    [L((0, 0), (64, 0)), L((32, 0), (32, 100))],
    [L((0, 2), (60, 0)), L((28, 0), (30, 100))],
]
G['U'] = [
    [seq(L((0, 0), (0, 64)), arc(30, 64, 30, 36, 180, 0), L((60, 64), (60, 0)))],
    [seq(L((0, 0), (0, 64)), arc(30, 64, 30, 36, 180, 0), L((60, 64), (60, 0))), L((60, 0), (60, 100))],
    [seq(L((0, 0), (4, 70)), arc(30, 70, 26, 30, 180, 0), L((56, 70), (60, 0)))],  # deeper, narrower
]
G['V'] = [
    [L((0, 0), (32, 100), (64, 0))],
]
G['W'] = [
    [L((0, 0), (18, 100), (36, 30), (54, 100), (72, 0))],
    [L((0, 0), (16, 100), (36, 0), (56, 100), (72, 0))],
    [seq(L((0, 0), (8, 80)), arc(20, 80, 12, 20, 180, 0), L((32, 80), (36, 30), (40, 80)), arc(52, 80, 12, 20, 180, 0), L((64, 80), (72, 0)))],  # rounded
]
G['X'] = [
    [L((0, 0), (60, 100)), L((60, 0), (0, 100))],
]
G['Y'] = [
    [L((0, 0), (30, 48)), L((60, 0), (30, 48), (30, 100))],
    [L((0, 0), (30, 50)), L((60, 0), (4, 100))],
]
G['Z'] = [
    [L((0, 0), (60, 0), (0, 100), (60, 100))],
    [L((0, 0), (60, 0), (0, 100), (60, 100)), L((12, 50), (48, 50))],
    [L((4, 4), (58, 0), (6, 96), (62, 100))],                                     # loose
]
G['0'] = [
    [arc(26, 50, 26, 50, -90, -450)],
    [arc(26, 50, 26, 50, -90, -450), L((48, 10), (4, 90))],
    [arc(30, 50, 30, 50, -60, -420)],                                     # a bit wider
]
G['1'] = [
    [L((8, 24), (30, 0), (30, 100))],
    [L((4, 30), (30, 0), (30, 100))],                                      # steeper flag
]
G['2'] = [
    [seq(arc(30, 28, 30, 28, -160, 20), L((58, 38), (0, 100), (62, 100)))],
    [seq(arc(30, 28, 30, 28, -170, 10), L((59, 33), (0, 100)), L((0, 100), (62, 100)))],
]
G['3'] = [
    [seq(arc(28, 25, 30, 25, -150, 90), arc(28, 75, 32, 25, -90, 150))],
    [seq(L((0, 0), (56, 0), (24, 42)), arc(28, 70, 32, 28, -100, 150))],
]
G['4'] = [
    [L((46, 100), (46, 0), (0, 66), (62, 66))],
    [L((4, 0), (0, 58), (62, 58)), L((46, 0), (46, 100))],
    [L((40, 0), (0, 64), (62, 64)), L((44, 30), (44, 100))],             # stem drawn separately
]
G['5'] = [
    [seq(L((56, 0), (6, 0), (2, 44)), arc(28, 70, 32, 30, -150, 150))],
    [seq(L((6, 0), (2, 44)), arc(28, 70, 32, 30, -150, 150)), L((6, 0), (56, 0))],
]
G['6'] = [
    [seq(L((50, 0), (24, 18), (6, 44)), arc(30, 70, 30, 30, 180, -180))],
    [seq(arc(52, 50, 52, 50, -100, -180), arc(30, 72, 30, 28, 180, -180))],
]
G['7'] = [
    [L((0, 0), (60, 0), (16, 100))],
    [L((0, 0), (60, 0), (16, 100)), L((14, 52), (52, 52))],
    [seq(L((0, 0), (60, 0)), L((60, 0), (40, 40), (28, 70), (22, 100)))],   # curved stem
]
G['8'] = [
    [seq(arc(30, 24, 24, 24, -10, -350), L((48, 40), (12, 58)), arc(30, 76, 26, 24, -140, 200), L((10, 60), (12, 42)))],
    [arc(30, 25, 24, 25, 90, 450), arc(30, 74, 28, 26, -90, 270)],
    [arc(30, 22, 18, 22, 90, 450), arc(30, 70, 30, 30, -90, 270)],       # small top loop
]
G['9'] = [
    [seq(arc(30, 28, 28, 28, 0, -360), L((58, 28), (54, 100)))],
    [seq(arc(30, 28, 28, 28, 0, -360), L((58, 28), (58, 100)))],
]

CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
assert set(G) == set(CHARS), set(CHARS) ^ set(G)


def strokes_q(v):
    """Integer-quantised strokes (0..127 after shifting), densified so no two
    consecutive points are more than ~8 units apart (resampling needs the
    true path, not just the corners - straight lines are exact anyway)."""
    out = []
    for s in v:
        q = [(int(round(x)) + 10, int(round(y)) + 10) for x, y in s]
        dd = []
        for p in q:
            if not dd or p != dd[-1]:
                dd.append(p)
        if len(dd) == 1:
            dd.append(dd[0])
        out.append(dd)
    return out


# ---------------------------------------------------------------- header
def write_header(path):
    blob, ntpl = [], 0
    for ch in CHARS:
        for v in G[ch]:
            st = strokes_q(v)
            blob += [ord(ch), len(st)]
            for s in st:
                assert len(s) < 256
                blob.append(len(s))
                for x, y in s:
                    assert 0 <= x < 256 and 0 <= y < 256
                    blob += [x, y]
            ntpl += 1
    lines = ["// GENERATED by tools/write_glyphs.py header -- do not edit by hand.",
             "// Built-in handwriting templates for the Write app (uppercase A-Z, 0-9).",
             "// Format, repeated kWriteTplCount times:",
             "//   char, nStrokes, { nPts, x0, y0, x1, y1, ... } * nStrokes",
             "// Coordinates: glyph 100 units tall, y down, offset +10 (0..~110).",
             "#pragma once", "#include <stdint.h>", "",
             f"static const uint16_t kWriteTplCount = {ntpl};",
             f"static const uint8_t kWriteTplBlob[{len(blob)}] = {{"]
    for i in range(0, len(blob), 20):
        lines.append("  " + ", ".join(str(b) for b in blob[i:i + 20]) + ",")
    lines.append("};")
    open(path, "w").write("\n".join(lines) + "\n")
    print(f"wrote {path}: {ntpl} templates, {len(blob)} bytes")


# ---------------------------------------------------------------- synthetic draws
# Drawing area on the watch (must match write_view.cpp).
DRAW_X0, DRAW_X1, DRAW_Y0, DRAW_Y1 = 8, 232, 100, 270


def _resample_path(s, spacing):
    """Points along a polyline every `spacing` units (keeps both ends)."""
    out = [s[0]]
    acc = 0.0
    for a, b in zip(s, s[1:]):
        d = math.dist(a, b)
        t = spacing - acc
        while t <= d:
            out.append((a[0] + (b[0] - a[0]) * t / d, a[1] + (b[1] - a[1]) * t / d))
            t += spacing
        acc = (acc + d) % spacing if d > 0 else acc
    if math.dist(out[-1], s[-1]) > 1:
        out.append(s[-1])
    return out


def perturb(variant, rng, strength=1.0, center=False):
    """A finger draw of `variant`: list of strokes of integer screen points.
    Distortions: size, position, aspect, slant (shear), rotation, smooth wobble,
    jitter, stroke order shuffle, stroke direction flips, and joining strokes
    whose ends are close into one continuous stroke (pen not lifted)."""
    k = strength
    strokes = [list(s) for s in variant]
    # stroke order / direction (the recogniser must not care)
    rng.shuffle(strokes)
    strokes = [s[::-1] if rng.random() < 0.4 else s for s in strokes]
    # join consecutive strokes whose end/start are close (one continuous stroke)
    if len(strokes) > 1 and rng.random() < 0.35 * k:
        j = [strokes[0]]
        for s in strokes[1:]:
            if math.dist(j[-1][-1], s[0]) < 30:
                j[-1] = j[-1] + s
            else:
                j.append(s)
        strokes = j
    xs = [p[0] for s in strokes for p in s]
    ys = [p[1] for s in strokes for p in s]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    h = rng.uniform(95, 150)                       # glyph height on screen, px
    sc = h / 100.0
    asp = 1 + rng.uniform(-0.15, 0.15) * k
    shear = rng.uniform(-0.2, 0.2) * k
    rot = math.radians(rng.uniform(-8, 8) * k)
    cr, sr = math.cos(rot), math.sin(rot)
    wob = []
    for _ in strokes:
        wob.append((rng.uniform(0, 3) * k, rng.uniform(0.5, 2.0), rng.uniform(0, 6.28),
                    rng.uniform(0, 3) * k, rng.uniform(0.5, 2.0), rng.uniform(0, 6.28)))
    out = []
    for si, s in enumerate(strokes):
        dense = _resample_path(s, 2.0)
        n = len(dense)
        pts = []
        for i, (x, y) in enumerate(dense):
            t = i / max(1, n - 1)
            ax, fx, px, ay, fy, py = wob[si]
            x = (x - cx) * asp + ax * math.sin(6.28 * fx * t + px)
            y = (y - cy) + ay * math.sin(6.28 * fy * t + py)
            x = x + shear * y
            x, y = x * cr - y * sr, x * sr + y * cr
            pts.append((x * sc, y * sc))
        out.append(pts)
    # place inside the drawing area
    xs = [p[0] for s in out for p in s]
    ys = [p[1] for s in out for p in s]
    w, hh = max(xs) - min(xs), max(ys) - min(ys)
    room_x = (DRAW_X1 - DRAW_X0 - 16) - w
    room_y = (DRAW_Y1 - DRAW_Y0 - 16) - hh
    if room_x < 0 or room_y < 0:                   # too big: shrink to fit
        f = min((DRAW_X1 - DRAW_X0 - 16) / max(w, 1), (DRAW_Y1 - DRAW_Y0 - 16) / max(hh, 1))
        out = [[(x * f, y * f) for x, y in s] for s in out]
        xs = [p[0] for s in out for p in s]; ys = [p[1] for s in out for p in s]
        w, hh = max(xs) - min(xs), max(ys) - min(ys)
        room_x = (DRAW_X1 - DRAW_X0 - 16) - w
        room_y = (DRAW_Y1 - DRAW_Y0 - 16) - hh
    fx, fy = (0.5, 0.5) if center else (rng.random(), rng.random())
    ox = DRAW_X0 + 8 + fx * max(0, room_x) - min(xs)
    oy = DRAW_Y0 + 8 + fy * max(0, room_y) - min(ys)
    final = []
    for s in out:
        # finger samples: one touch step every ~10-18 px of travel
        samp = _resample_path(s, rng.uniform(10, 18))
        final.append([(int(round(x + ox + rng.uniform(-1.5, 1.5) * k)),
                       int(round(y + oy + rng.uniform(-1.5, 1.5) * k))) for x, y in samp])
    return final


def draw_steps(strokes):
    st = []
    for s in strokes:
        st.append(f"touch down {s[0][0]} {s[0][1]}")
        for x, y in s[1:]:
            st.append(f"touch move {x} {y}")
        st.append("touch up")
    return st


def line_steps(x0, y0, x1, y1, n=8):
    return draw_steps([[(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n)) for i in range(n + 1)]])


GAP_WAIT = 900   # ms after the last stroke: > the 600 ms end-of-glyph pause


def write_tests(seed=7, per_char=6):
    rng = random.Random(seed)
    tdir = os.path.join(ROOT, "test")
    os.makedirs(tdir, exist_ok=True)

    # 1. accuracy sweep: every char, `per_char` perturbed draws (random variant)
    exp = []
    st = ["# GENERATED by tools/write_glyphs.py tests -- synthetic accuracy sweep",
          "# Every A-Z/0-9 drawn %d times from perturbed templates. Labels in write_sweep.expected." % per_char,
          "# Run offline: ewsimrec ewwrite --snap /tmp/x test/write_sweep.ewt -v 2>log; tools/write_score.py log",
          "goto Write", "wait 400", "tap 210 72", "wait 200"]
    order = [c for c in CHARS for _ in range(per_char)]
    rng.shuffle(order)
    for c in order:
        v = rng.choice(G[c])
        st.append("tap 210 72")        # CLR: no previous char, so no letter/digit context
        st += draw_steps(perturb(v, rng))
        st.append(f"wait {GAP_WAIT}")
        exp.append(c)
    open(os.path.join(tdir, "write_sweep.ewt"), "w").write("\n".join(st) + "\n")
    open(os.path.join(tdir, "write_sweep.expected"), "w").write("".join(exp) + "\n")

    # 2. hardware/sim smoke test with assertions (ewt-compatible)
    r2 = random.Random(seed + 1)
    s = ["# Write app smoke test (ewt on the watch, or ewsimrec offline). GENERATED by tools/write_glyphs.py",
         "goto Write", "wait 400", "expect screen Write",
         "# clear any saved text", "tap 210 72", "expect log WRITE:.*clear",
         "# draw H E L L O (A-Z templates, perturbed)"]
    for c in "HELLO":
        s += draw_steps(perturb(G[c][0], r2, 0.6))
        s.append(f"expect log WRITE:.*char={c} 1500")
    s += ["# space: straight stroke left -> right"] + line_steps(40, 200, 200, 204) + ["expect log WRITE:.*edit=space 1500"]
    for c in "42":
        s += draw_steps(perturb(G[c][0], r2, 0.6))
        s.append(f"expect log WRITE:.*char={c} 1500")
    s += ["# backspace: straight stroke right -> left (deletes the 2)"] + line_steps(200, 220, 40, 216) + ["expect log WRITE:.*edit=backspace 1500",
          "expect log WRITE:.*text=HELLO.4\\s*$ 500"]
    s += ["# enter: straight stroke top-right -> bottom-left"] + line_steps(180, 120, 70, 240) + ["expect log WRITE:.*edit=enter 1500"]
    s += ["# multi-stroke T with the strokes in reverse order",
          ] + draw_steps([[(120, 130), (120, 160), (121, 190), (121, 220), (122, 250)], [(80, 128), (110, 128), (140, 129), (165, 130)]]) + [
          "expect log WRITE:.*char=T 1500",
          "# tap the 2nd candidate chip: corrects the last letter + learns the glyph",
          "tap 79 72", "expect log WRITE:.*fix", "expect log WRITE:.*learn",
          "# the same drawing again now comes back as the taught character",
          ] + draw_steps([[(120, 130), (120, 160), (121, 190), (121, 220), (122, 250)], [(80, 128), (110, 128), (140, 129), (165, 130)]]) + [
          "expect log WRITE:.*char=7.*user=1 1500",
          "# swipe right must NOT leave the app (drawing an L-like stroke would otherwise exit)",
          "swipe r", "wait 800", "expect screen Write",
          "expect noerr",
          "# side button = back to the launcher",
          "btn short", "wait 300", "expect screen AppList"]
    open(os.path.join(tdir, "write.ewt"), "w").write("\n".join(s) + "\n")

    # 3. checker must be able to fail: draw an A, demand a B
    r3 = random.Random(seed + 2)
    s = ["# MUST FAIL (exit 1): proves `expect log WRITE:.*char=X` can fail",
         "goto Write", "wait 400", "tap 210 72"]
    s += draw_steps(perturb(G['A'][0], r3, 0.3)) + ["expect log WRITE:.*char=B 1500"]
    open(os.path.join(tdir, "write_mustfail.ewt"), "w").write("\n".join(s) + "\n")

    # 4. demo: write HELLO slowly for the MP4
    r4 = random.Random(seed + 3)
    s = ["# Demo footage: HELLO written on the watch", "goto Write", "wait 500", "tap 210 72", "wait 400"]
    for c in "HELLO":
        s += draw_steps(perturb(G[c][0], r4, 0.4, center=True))
        s.append("wait 1100")
    s += ["wait 1500"]
    open(os.path.join(tdir, "write_hello.ewt"), "w").write("\n".join(s) + "\n")
    print(f"wrote test/write_sweep.ewt ({len(order)} draws), write.ewt, write_mustfail.ewt, write_hello.ewt")


def write_bench_data(path, seed=11, per_char=60, strength=1.0):
    """Synthetic draws for the host bench: '#c' line then one line per stroke."""
    rng = random.Random(seed)
    with open(path, "w") as f:
        for c in CHARS:
            for i in range(per_char):
                v = rng.choice(G[c])
                f.write(f"#{c}\n")
                for s in perturb(v, rng, strength):
                    f.write(" ".join(f"{x},{y}" for x, y in s) + "\n")
    print(f"wrote {path}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "all"
    if cmd in ("header", "all"):
        write_header(os.path.join(ROOT, "src/apps/write/write_templates.h"))
    if cmd in ("tests", "all"):
        write_tests()
    if cmd == "bench":
        write_bench_data(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 11,
                         int(sys.argv[4]) if len(sys.argv) > 4 else 60,
                         float(sys.argv[5]) if len(sys.argv) > 5 else 1.0)


def blob_of(items):
    """items: [(ch, variant)] -> (bytes, count) in the firmware blob format."""
    blob = []
    for ch, v in items:
        st = strokes_q(v)
        blob += [ord(ch), len(st)]
        for s in st:
            blob.append(len(s))
            for x, y in s:
                blob += [x, y]
    return bytes(blob), len(items)


def leave_one_out(bench, workdir, per=40, seed=5):
    """For every char with >= 2 variants and every variant k: templates = all
    but (ch, k); test draws come only from (ch, k). Measures how well the
    recogniser copes with a shape it has never seen (closer to a real writer)."""
    import subprocess
    rng = random.Random(seed)
    tot = ok = 0
    per_char = collections.defaultdict(lambda: [0, 0])
    conf = collections.Counter()
    for ch in CHARS:
        if len(G[ch]) < 2:
            continue
        for k, held in enumerate(G[ch]):
            items = [(c, v) for c in CHARS for i, v in enumerate(G[c]) if not (c == ch and i == k)]
            b, n = blob_of(items)
            bp = os.path.join(workdir, "loo.bin"); open(bp, "wb").write(b)
            dp = os.path.join(workdir, "loo.txt")
            with open(dp, "w") as f:
                for _ in range(per):
                    f.write(f"#{ch}\n")
                    for s in perturb(held, rng):
                        f.write(" ".join(f"{x},{y}" for x, y in s) + "\n")
            out = subprocess.run([bench, dp, "--tpl", bp, str(n)], capture_output=True, text=True).stdout
            m = re.search(r"per-char: \S=(\d+)/(\d+)", out)
            a, t = int(m.group(1)), int(m.group(2))
            ok += a; tot += t
            per_char[f"{ch}{k + 1}"] = [a, t]
            for cm in re.finditer(r" (\S->\S+):(\d+)", out.split("confusions:")[1]):
                conf[cm.group(1)] += int(cm.group(2))
    print(f"leave-one-variant-out: top1={ok}/{tot} = {100 * ok / tot:.1f}%")
    print("per held-out variant: " + " ".join(f"{k}={v[0]}/{v[1]}" for k, v in per_char.items()))
    print("confusions: " + " ".join(f"{k}:{n}" for k, n in conf.most_common(20)))


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "loo":
    import collections, re
    leave_one_out(sys.argv[2], sys.argv[3])
