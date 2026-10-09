#!/usr/bin/env python3
"""
encode_media.py — bake images / videos into PROGMEM headers for the EWatch
media app.

Output format is raw RGB565 (the panel's native format) so the firmware
streams pixels straight from flash to SPI with zero decode CPU.

Usage
-----
    # an image (default max size 240x280, will preserve aspect)
    python tools/encode_media.py image input.png  --name logo
    python tools/encode_media.py image photo.jpg  --name photo --max-w 240 --max-h 280

    # a video (default 120x140 at 12 fps; needs ffmpeg on PATH)
    python tools/encode_media.py video clip.mp4   --name demo
    python tools/encode_media.py video clip.mp4   --name demo --w 96 --h 96 --fps 15

Then add the entry to src/apps/assets/manifest.h, e.g.:

    #include "logo.h"
    ...
    { "Logo", MediaKind::Image,
      media_logo::W, media_logo::H,
      media_logo::FPS, media_logo::FRAMES, media_logo::pixels },

Sizing guidance
---------------
The panel is 240x280. Each pixel costs 2 bytes of flash:
    image  240x280  ~= 134 KB
    video  120x140 @ 12 fps for 5 s = 60 frames  ~= 2.0 MB
    video   80x100 @ 15 fps for 5 s = 75 frames  ~= 1.2 MB
Default partition table on the ESP32-S3FH4R2 leaves ~1.25 MB of app flash;
keep total media well under that or switch to a larger app partition.

Requirements
------------
    pip install pillow
    ffmpeg on PATH (only needed for video).
"""

import argparse
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Missing dependency: install with `pip install pillow`")

REPO_ROOT  = Path(__file__).resolve().parent.parent
ASSETS_DIR = REPO_ROOT / "src" / "apps" / "assets"


def to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def emit_array(f, values, per_line=12, indent="    "):
    line = []
    for i, v in enumerate(values):
        line.append(f"0x{v:04X}")
        if (i + 1) % per_line == 0:
            f.write(indent + ", ".join(line) + ",\n")
            line.clear()
    if line:
        f.write(indent + ", ".join(line) + ",\n")


def write_header(out_path: Path, name: str, w: int, h: int,
                 fps: int, frames: int, pixel_iter, source: str):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w") as f:
        f.write(f"// Auto-generated from {source} by tools/encode_media.py.\n")
        f.write("// Format: raw RGB565, native panel byte order. Do not hand-edit.\n")
        f.write("#pragma once\n")
        f.write("#include <pgmspace.h>\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"namespace media_{name} {{\n")
        f.write(f"  constexpr int16_t  W      = {w};\n")
        f.write(f"  constexpr int16_t  H      = {h};\n")
        f.write(f"  constexpr uint16_t FPS    = {fps};\n")
        f.write(f"  constexpr uint16_t FRAMES = {frames};\n")
        f.write(f"  const uint16_t pixels[W * H * FRAMES] PROGMEM = {{\n")
        emit_array(f, pixel_iter)
        f.write("  };\n")
        f.write("}\n")


def encode_image(input_path: str, name: str, max_w: int, max_h: int):
    img = Image.open(input_path).convert("RGB")
    img.thumbnail((max_w, max_h), Image.LANCZOS)
    w, h = img.size
    pixels = (to_rgb565(*p) for p in img.getdata())
    out = ASSETS_DIR / f"{name}.h"
    write_header(out, name, w, h, fps=0, frames=1,
                 pixel_iter=pixels, source=input_path)
    print(f"wrote {out.relative_to(REPO_ROOT)}  "
          f"({w}x{h}, {w*h*2:,} bytes)")


def encode_video(input_path: str, name: str, w: int, h: int, fps: int):
    # Use ffmpeg to decode + scale + reframerate to a raw RGB stream.
    cmd = [
        "ffmpeg", "-loglevel", "error",
        "-i", input_path,
        "-vf", f"scale={w}:{h}:flags=lanczos,fps={fps}",
        "-pix_fmt", "rgb24",
        "-f", "rawvideo",
        "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, check=True)
    except FileNotFoundError:
        sys.exit("ffmpeg not found on PATH (needed for video).")
    except subprocess.CalledProcessError as e:
        sys.exit(f"ffmpeg failed: {e.stderr.decode(errors='replace')}")

    raw = proc.stdout
    bpf = w * h * 3
    if len(raw) < bpf:
        sys.exit("ffmpeg produced no frames")
    n = len(raw) // bpf

    def gen():
        for fi in range(n):
            base = fi * bpf
            for i in range(w * h):
                p = base + i * 3
                yield to_rgb565(raw[p], raw[p + 1], raw[p + 2])

    out = ASSETS_DIR / f"{name}.h"
    write_header(out, name, w, h, fps=fps, frames=n,
                 pixel_iter=gen(), source=input_path)
    print(f"wrote {out.relative_to(REPO_ROOT)}  "
          f"({n} frames @ {fps} fps, {w}x{h}, {n*w*h*2:,} bytes)")


def main():
    ap = argparse.ArgumentParser(
        description="Bake images / videos into RGB565 PROGMEM headers.")
    sub = ap.add_subparsers(dest="kind", required=True)

    p_img = sub.add_parser("image", help="encode a still image")
    p_img.add_argument("input")
    p_img.add_argument("--name",   required=True,
                       help="C++ identifier suffix (no spaces)")
    p_img.add_argument("--max-w",  type=int, default=240)
    p_img.add_argument("--max-h",  type=int, default=280)

    p_vid = sub.add_parser("video", help="encode a video (needs ffmpeg)")
    p_vid.add_argument("input")
    p_vid.add_argument("--name",   required=True,
                       help="C++ identifier suffix (no spaces)")
    p_vid.add_argument("--w",      type=int, default=120)
    p_vid.add_argument("--h",      type=int, default=140)
    p_vid.add_argument("--fps",    type=int, default=12)

    a = ap.parse_args()
    if a.kind == "image":
        encode_image(a.input, a.name, a.max_w, a.max_h)
    else:
        encode_video(a.input, a.name, a.w, a.h, a.fps)


if __name__ == "__main__":
    main()
