#!/usr/bin/env python3
"""
Take a raw fontconvert.c output header and rewrite it to match this
codebase's existing GFXfont header style, as seen in
AuroraOS/src/apps/assets/fonts/*.h:

  #pragma once
  #include "gfxfont.h"   // from GFX Library for Arduino (this project)

  const uint8_t <Name>Bitmaps[] PROGMEM = { ... };

  const GFXglyph <Name>Glyphs[] PROGMEM = { ... };

  const GFXfont <Name> PROGMEM = {
      (uint8_t *)<Name>Bitmaps, (GFXglyph *)<Name>Glyphs,
      0x.., 0x.., NN};

  // Approx. NNNN bytes

fontconvert.c derives its own internal identifier from the input filename
(e.g. "SpaceGrotesk_Bold28pt7b") -- this script just finds-and-replaces
that internal identifier with our desired struct name everywhere it
appears (bitmaps array, glyph array, font struct), and reformats the
surrounding boilerplate to match the existing headers byte-for-byte in
style (spacing of pragma/include block).
"""
import re
import sys

def convert(raw_text, struct_name):
    # Find the auto-generated base identifier from "const uint8_t <ID>Bitmaps[]"
    m = re.search(r"const uint8_t (\w+)Bitmaps\[\] PROGMEM", raw_text)
    if not m:
        raise ValueError("could not find Bitmaps array declaration")
    old_name = m.group(1)

    # Replace every occurrence of the old identifier with the new one.
    text = raw_text.replace(old_name + "Bitmaps", struct_name + "Bitmaps")
    text = text.replace(old_name + "Glyphs", struct_name + "Glyphs")
    # The bare font struct name appears as "const GFXfont <old_name> PROGMEM"
    text = re.sub(
        rf"const GFXfont {re.escape(old_name)} PROGMEM",
        f"const GFXfont {struct_name} PROGMEM",
        text,
    )
    text = re.sub(
        rf"\(uint8_t\s*\*\){re.escape(old_name)}Bitmaps",
        f"(uint8_t *){struct_name}Bitmaps",
        text,
    )
    text = re.sub(
        rf"\(GFXglyph\s*\*\){re.escape(old_name)}Glyphs",
        f"(GFXglyph *){struct_name}Glyphs",
        text,
    )

    header = '#pragma once\n#include "gfxfont.h"   // from GFX Library for Arduino (this project)\n\n'
    return header + text


if __name__ == "__main__":
    raw_path, struct_name, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(raw_path) as f:
        raw = f.read()
    out = convert(raw, struct_name)
    with open(out_path, "w") as f:
        f.write(out)
    print(f"wrote {out_path}")
