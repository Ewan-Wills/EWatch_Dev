# GalleryFace

Stand-alone EWatch v2 firmware: the AuroraOS **Analog** dial (same tick ring,
56/84/96 px hands, Aurora-blue second hand) over a **crossfading slideshow of
Claire Wills paintings**, all hard-coded into flash as JPEGs, plus two QR cards.

| Page | Shows |
|---|---|
| 0 | paintings with the dial; **tap** or **button** toggles the dial off = screensaver (paintings only) |
| 1 (swipe right) | QR: gallery location, https://maps.app.goo.gl/LDtHCnFEXopYkxdy5 |
| 2 (swipe right again) | QR: https://clairewills.co.uk |

Swipe left goes back a page; a short button press on a QR page returns to
page 0. Hold the button 2 s to power off.

- 288 paintings (every original on clairewills.co.uk in the local DB copy of
  2026-09-16; prints/cards skipped as copies), 240 px wide, max 360 px tall,
  baseline JPEG q70, 3.68 MB. Firmware 4.06 MB of the 4.13 MB app partition.
- Each painting holds 7 s and crossfades over 1.5 s into the next; taller than
  the screen = slow top-to-bottom pan, shorter = centred on black. Core 0
  decodes the next painting into PSRAM while the current one shows (two
  172 KB slots).
- Title of the painting shown at the bottom (upload filenames like `00_ce`
  are hidden). Time from the RV-3028 RTC (set it with AuroraOS). Starts at a
  random painting each boot.

## Build / flash
```bash
pio run -t upload && pio device monitor
```
The monitor prints `page= dial= fps= busy=ms/frame decodeFail=` every 5 s and
one line per touch gesture.

## Refresh the paintings
```bash
uv run --with pillow tools/build_paintings.py [path/to/db.sqlite] [--budget BYTES]
```
Picks the highest JPEG quality that fits `--budget` (default 3.74 MB). If the
build then overflows flash, lower the budget.

## Host checks (no watch needed)
```bash
test/render.sh
```
Decodes every painting with the firmware's JPEGDEC path and compares it with
Pillow; checks the exact crossfade frames (start, middle, end), painting order
incl. the wrap, and that a fade waits for the decoder; decodes both QR cards
with zxing-cpp and OpenCV and compares the exact URLs; writes PNGs to
`test/out/`. Every checker is first run against broken input to prove it can
fail.

## Files
- `src/main.cpp` hardware, tasks, frame loop
- `src/slides.*` slideshow, decode slots, crossfade (host-testable)
- `src/qr_draw.*` QR card (host-testable)
- `src/face_draw.*` the dial (host-testable)
- `src/paintings_data.h` generated, do not edit
- `partitions_maxapp.csv` one 4 MB factory app, no OTA/filesystem
