"""Minimal ewbridge client without pygame or the App framework: any program can
put pixels on the watch and get its input. Draws a clock and a dot where the
watch is touched; the side button buzzes.

    bin/ewbridge up
    .venv/bin/python examples/client_example.py
"""
import os
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from ewbridge.client import Client  # noqa: E402

c = Client("clock-example", "Clock example")
touch = None
t_end = time.time() + float(sys.argv[1]) if len(sys.argv) > 1 else None
while not c.closed and (t_end is None or time.time() < t_end):
    for ev in c.poll():
        if ev["type"] == "touch":
            touch = None if ev["phase"] == "up" else (ev["x"], ev["y"])
        elif ev["type"] == "button" and ev["phase"] == "short":
            c.haptic(60, 220)
    w, h = c.size
    img = Image.new("RGB", (w, h), (12, 18, 30))
    d = ImageDraw.Draw(img)
    d.text((16, 16), time.strftime("%H:%M:%S"), fill=(255, 255, 255))
    d.text((16, 36), "touch me", fill=(120, 160, 220))
    if touch:
        d.ellipse([touch[0] - 12, touch[1] - 12, touch[0] + 12, touch[1] + 12], fill=(255, 120, 40))
    c.frame(np.asarray(img))
    time.sleep(1 / 30)
