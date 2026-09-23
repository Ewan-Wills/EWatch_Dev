#!/usr/bin/env python3
"""
Verification renderer: parses a GENERATED GFXfont .h file's own C source
(the Bitmaps array, the Glyphs array, and the GFXfont struct) with regexes,
reconstructs the glyph bitmaps exactly as the Adafruit_GFX drawChar()
routine would (continuous MSB-first bitstream, no row padding), and
rasterizes a sample string to a PNG.

This intentionally does NOT go back to the source TTF -- the point is to
prove the *generated header data* is self-consistent and correct, since
that's what will actually ship to the device.
"""
import re
import sys
from PIL import Image, ImageDraw

def parse_header(path):
    text = open(path).read()

    # --- Bitmaps array ---
    m = re.search(r"const uint8_t \w+Bitmaps\[\] PROGMEM = \{(.*?)\};", text, re.S)
    if not m:
        raise ValueError("Bitmaps array not found")
    bitmap_bytes = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]

    # --- Glyphs array ---
    m = re.search(r"const GFXglyph \w+Glyphs\[\] PROGMEM = \{(.*?)\};", text, re.S)
    if not m:
        raise ValueError("Glyphs array not found")
    glyph_body = m.group(1)
    glyphs = []
    for gm in re.finditer(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
        glyph_body,
    ):
        bitmapOffset, width, height, xAdvance, xOffset, yOffset = map(int, gm.groups())
        glyphs.append(dict(
            bitmapOffset=bitmapOffset, width=width, height=height,
            xAdvance=xAdvance, xOffset=xOffset, yOffset=yOffset,
        ))

    # --- GFXfont struct (first, last, yAdvance) ---
    m = re.search(
        r"const GFXfont \w+ PROGMEM = \{.*?(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*\};",
        text, re.S,
    )
    if not m:
        raise ValueError("GFXfont struct not found")
    first = int(m.group(1), 16)
    last = int(m.group(2), 16)
    yAdvance = int(m.group(3))

    if len(glyphs) != (last - first + 1):
        raise ValueError(f"glyph count {len(glyphs)} != range {last-first+1}")

    return dict(bitmap=bitmap_bytes, glyphs=glyphs, first=first, last=last, yAdvance=yAdvance)


def get_glyph_bit(bitmap_bytes, bitmapOffset, width, height, x, y):
    """Continuous MSB-first bitstream, no per-row padding (Adafruit GFX convention)."""
    bit_index = y * width + x
    byte_index = bitmapOffset + (bit_index // 8)
    bit_in_byte = 7 - (bit_index % 8)
    if byte_index >= len(bitmap_bytes):
        return 0
    return (bitmap_bytes[byte_index] >> bit_in_byte) & 1


def render_text(font, text, scale=6, margin=20, fg=(20, 20, 24), bg=(255, 255, 255)):
    glyphs = font["glyphs"]
    first = font["first"]

    # First pass: compute pixel bounding box of the rendered string, cursor on baseline y=0
    cursor_x = 0
    min_x, max_x, min_y, max_y = 0, 0, 0, 0
    for ch in text:
        code = ord(ch)
        if code < first or code > font["last"]:
            continue
        g = glyphs[code - first]
        if g["width"] > 0 and g["height"] > 0:
            gx0 = cursor_x + g["xOffset"]
            gx1 = gx0 + g["width"]
            gy0 = g["yOffset"]
            gy1 = gy0 + g["height"]
            min_x, max_x = min(min_x, gx0), max(max_x, gx1)
            min_y, max_y = min(min_y, gy0), max(max_y, gy1)
        cursor_x += g["xAdvance"]
    max_x = max(max_x, cursor_x)

    px_w = (max_x - min_x) + 2 * margin
    px_h = (max_y - min_y) + 2 * margin
    img_w, img_h = px_w * scale, px_h * scale
    img = Image.new("RGB", (img_w, img_h), bg)
    draw = ImageDraw.Draw(img)

    origin_x = margin - min_x
    origin_y = margin - min_y

    cursor_x = 0
    for ch in text:
        code = ord(ch)
        if code < first or code > font["last"]:
            continue
        g = glyphs[code - first]
        for gy in range(g["height"]):
            for gx in range(g["width"]):
                bit = get_glyph_bit(font["bitmap"], g["bitmapOffset"], g["width"], g["height"], gx, gy)
                if bit:
                    px = (cursor_x + g["xOffset"] + gx + origin_x) * scale
                    py = (g["yOffset"] + gy + origin_y) * scale
                    draw.rectangle([px, py, px + scale - 1, py + scale - 1], fill=fg)
        cursor_x += g["xAdvance"]

    # Draw a baseline marker in light red for visual sanity check
    baseline_y = (origin_y) * scale
    draw.line([(0, baseline_y), (img_w, baseline_y)], fill=(255, 180, 180), width=1)

    return img


if __name__ == "__main__":
    header_path, text, out_png = sys.argv[1], sys.argv[2], sys.argv[3]
    font = parse_header(header_path)
    print(f"Parsed {header_path}: {len(font['glyphs'])} glyphs, "
          f"range 0x{font['first']:02X}-0x{font['last']:02X}, yAdvance={font['yAdvance']}, "
          f"bitmap bytes={len(font['bitmap'])}")
    img = render_text(font, text)
    img.save(out_png)
    print(f"wrote {out_png} ({img.width}x{img.height})")
