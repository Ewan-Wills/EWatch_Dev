#!/usr/bin/env python3
"""
Iterate FreeType point sizes (same DPI/rasterization convention as
Adafruit fontconvert.c: DPI=141, FT_LOAD_TARGET_MONO) to find the integer
point size whose rendered bitmap height for a given probe character lands
within a target pixel range. Mirrors fontconvert.c's FT_Set_Char_Size call
so the chosen size produces the same rasterization when later passed to
fontconvert_bin.
"""
import sys
import freetype

DPI = 141

def measure(face_path, point_size, probe_char):
    face = freetype.Face(face_path)
    face.set_char_size(point_size << 6, 0, DPI, 0)
    face.load_char(probe_char, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    bmp = face.glyph.bitmap
    return bmp.rows, bmp.width

def find_size(face_path, probe_char, lo, hi, start=20):
    """Search integer point sizes for one whose probe_char bitmap height
    falls within [lo, hi] px. Returns (size, height)."""
    best = None
    for size in range(4, 200):
        rows, width = measure(face_path, size, probe_char)
        if lo <= rows <= hi:
            best = (size, rows, width)
            # keep searching a bit to see the full valid range but return first match near start
    return best

if __name__ == "__main__":
    face_path, probe_arg, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    # probe_arg may be a literal single character or a 0x.. codepoint
    if probe_arg.lower().startswith("0x"):
        probe_char = chr(int(probe_arg, 16))
    else:
        probe_char = probe_arg
    print(f"{'size':>5} {'rows':>5} {'width':>5}")
    matches = []
    for size in range(4, 100):
        rows, width = measure(face_path, size, probe_char)
        marker = ""
        if lo <= rows <= hi:
            marker = "  <-- in range"
            matches.append(size)
        print(f"{size:5d} {rows:5d} {width:5d}{marker}")
    print("Matching sizes:", matches)
