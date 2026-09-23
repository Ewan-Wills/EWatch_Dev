# EWatch ClaudeFace font converter

Builds the 5 `CF_*` GFXfont headers in
`AuroraOS/src/apps/claude/fonts/` from the OFL TTF sources in
`Claude Knowledge Base/blender/projects/ewatch-cloud/fonts/`.

## Tooling

- `fontconvert.c` — unmodified copy of Adafruit-GFX-Library's
  `fontconvert.c` (found under
  `Home Assistant/Alarm Clock/.pio/libdeps/esp32dev/Adafruit GFX Library/fontconvert/`),
  compiled against Homebrew FreeType (`/opt/homebrew/opt/freetype`).
- `gfxfont.h` — copy of this project's actual
  `AuroraOS/.pio/libdeps/ewatch/GFX Library for Arduino/src/gfxfont.h`
  (uint16_t `bitmapOffset`), used both to build fontconvert and for the
  compile check.
- `measure_size.py` — iterates FreeType point sizes at the same DPI (141)
  and rasterization mode (`FT_LOAD_TARGET_MONO`) fontconvert.c uses, to
  find the integer point size whose rendered probe-glyph height lands in
  a target px range.
- `rename_and_style.py` — rewrites fontconvert.c's raw output (which
  derives an internal identifier from the input filename) to use our
  `CF_*` struct names and match the existing codebase header style
  (`#pragma once` + `#include "gfxfont.h"` preamble, seen in
  `AuroraOS/src/apps/assets/fonts/*.h`).
- `render_preview.py` — parses a *generated* header's own C source
  (regex over the Bitmaps/Glyphs arrays and GFXfont struct) and
  rasterizes sample text from that data, to verify the header itself
  (not a re-render of the source TTF).
- `compile_check.cpp` — defines `PROGMEM` as a no-op and includes the
  real `gfxfont.h` + all 5 generated headers; checked with
  `xcrun clang++ -std=c++14 -fsyntax-only`.

## Build steps (reproducible)

```bash
# 0. venv for fontTools / freetype-py / Pillow
python3 -m venv /private/tmp/claude-501/fontconv
/private/tmp/claude-501/fontconv/bin/pip install fonttools freetype-py Pillow

# 1. Build fontconvert against Homebrew FreeType
xcrun clang -O2 -o fontconvert_bin fontconvert.c \
  -I/opt/homebrew/opt/freetype/include/freetype2 \
  -L/opt/homebrew/opt/freetype/lib -lfreetype
# (gfxfont.h must exist one directory above fontconvert.c, since it does
#  #include "../gfxfont.h" -- see the tools/gfxfont.h copy alongside this dir)

# 2. SpaceGrotesk-Regular.ttf is a variable font (wght axis, 300-700).
#    Instantiate the two static weights needed:
/private/tmp/claude-501/fontconv/bin/python3 - <<'EOF'
from fontTools.varLib import instancer
from fontTools.ttLib import TTFont
SRC = ".../ewatch-cloud/fonts/SpaceGrotesk-Regular.ttf"
for wght, name in [(700, "Bold"), (500, "Medium")]:
    f = TTFont(SRC)
    instancer.instantiateVariableFont(f, {"wght": wght}).save(f"instances/SpaceGrotesk-{name}.ttf")
EOF

# 3. Find point sizes hitting the target measured px heights
python3 measure_size.py instances/SpaceGrotesk-Bold.ttf 0 38 42      # -> size 28 (40px) - CF_SpaceGroteskBold40 (digit '0')
python3 measure_size.py instances/SpaceGrotesk-Bold.ttf 0 20 22      # -> size 15 (21px) - CF_SpaceGroteskBold22 (digit '0')
python3 measure_size.py instances/SpaceGrotesk-Medium.ttf E 11 12   # -> size 9  (12px) - CF_SpaceGroteskMed12 (cap 'E')
python3 measure_size.py instances/SpaceGrotesk-Medium.ttf E 8 9     # -> size 6  (8px)  - CF_SpaceGroteskMed9 (cap 'E')
python3 measure_size.py JetBrainsMono-Medium.ttf E 9 10             # -> size 7  (10px) - CF_JetBrainsMono10 (cap 'E')

# 4. Run fontconvert for each font (first/last are decimal ASCII codes:
#    0x20=32, 0x3A=58, 0x7E=126)
./fontconvert_bin instances/SpaceGrotesk-Bold.ttf   28 32 58  > generated/CF_SpaceGroteskBold40_raw.h
./fontconvert_bin instances/SpaceGrotesk-Bold.ttf   15 32 126 > generated/CF_SpaceGroteskBold22_raw.h
./fontconvert_bin instances/SpaceGrotesk-Medium.ttf 9  32 126 > generated/CF_SpaceGroteskMed12_raw.h
./fontconvert_bin instances/SpaceGrotesk-Medium.ttf 6  32 126 > generated/CF_SpaceGroteskMed9_raw.h
./fontconvert_bin JetBrainsMono-Medium.ttf          7  32 126 > generated/CF_JetBrainsMono10_raw.h

# 5. Rename fontconvert's auto-derived identifiers to our CF_* struct
#    names and add the project's #pragma once / #include preamble
OUTDIR=.../AuroraOS/src/apps/claude/fonts
python3 rename_and_style.py generated/CF_SpaceGroteskBold40_raw.h CF_SpaceGroteskBold40 "$OUTDIR/CF_SpaceGroteskBold40.h"
python3 rename_and_style.py generated/CF_SpaceGroteskBold22_raw.h CF_SpaceGroteskBold22 "$OUTDIR/CF_SpaceGroteskBold22.h"
python3 rename_and_style.py generated/CF_SpaceGroteskMed12_raw.h  CF_SpaceGroteskMed12  "$OUTDIR/CF_SpaceGroteskMed12.h"
python3 rename_and_style.py generated/CF_SpaceGroteskMed9_raw.h   CF_SpaceGroteskMed9   "$OUTDIR/CF_SpaceGroteskMed9.h"
python3 rename_and_style.py generated/CF_JetBrainsMono10_raw.h    CF_JetBrainsMono10    "$OUTDIR/CF_JetBrainsMono10.h"

# 6. Copy OFL license files into the output dir
cp .../ewatch-cloud/fonts/OFL-SpaceGrotesk.txt  "$OUTDIR/"
cp .../ewatch-cloud/fonts/OFL-JetBrainsMono.txt "$OUTDIR/"

# 7. Verification: render sample text FROM the generated headers
python3 render_preview.py "$OUTDIR/CF_SpaceGroteskBold40.h" "12:47"            previews/CF_SpaceGroteskBold40.png
python3 render_preview.py "$OUTDIR/CF_SpaceGroteskBold22.h" "NEEDS YOU"        previews/CF_SpaceGroteskBold22.png
python3 render_preview.py "$OUTDIR/CF_SpaceGroteskMed12.h"  "Bash - npm test"  previews/CF_SpaceGroteskMed12.png
python3 render_preview.py "$OUTDIR/CF_SpaceGroteskMed9.h"   "5h resets 2h14"   previews/CF_SpaceGroteskMed9.png
python3 render_preview.py "$OUTDIR/CF_JetBrainsMono10.h"    "Bash - npm test"  previews/CF_JetBrainsMono10.png

# 8. Verification: host compile-check (syntax only)
xcrun clang++ -std=c++14 -fsyntax-only compile_check.cpp -I. -I"$OUTDIR"
```

## Notes

- `SpaceGrotesk-Regular.ttf` is a **variable font** (wght axis 300-700).
  Bold (700) was instantiated for the digit fonts, Medium (500) for the
  label fonts, per the task spec.
- `JetBrainsMono-Medium.ttf` is a static font, used directly.
- The em dash (U+2014, "—") is outside GFXfont's 0x20-0x7E ASCII range
  and is intentionally NOT special-cased here; the firmware draws it as
  a separately-drawn short horizontal bar.
