# Draw module

Finger painting with colours, sizes, eraser, undo, clear and Send (pick an
eWatch Cloud contact, send the drawing as a 240x240 RGB565 image).
Build with `EW_APPS="... draw"` (adds `-DEW_APP_DRAW=1` and this folder).

## Layout and controls
- Rows 0..239: the drawing (a 240x240 square, exactly what Send uploads).
- Rows 240..279: a slim toolbar pill, six 37 px slots:
  colour swatch | brush size | eraser | undo | clear | send.
  The bar is always shown under the canvas. It never overlaps the drawing, so
  it doesn't need to collapse while drawing, and the canvas stays square for
  Send.
- Colour: a popover with 12 swatches (black, white, grey, sienna + the Aurora
  red, orange, yellow, green, teal, blue, purple, pink). Picking one also turns
  the eraser off.
- Size: 4 brushes, radius 2 / 4 / 7 / 11 px (eraser 5 / 8 / 12 / 16). The bar
  icon shows the dot in the current colour.
- Eraser: toggle (blue disc while on). It paints the current page colour.
- Undo: removes the last stroke or clear. Dimmed when there is nothing to undo.
- Clear: a confirm card: Black page / White page / Cancel. A clear is an undo
  step too.
- Send: refuses an empty page ("Draw something first") and an unlinked watch
  ("Link eWatch Cloud in Settings"). Otherwise it opens `cloudPickContact`.
  On pick, the picture is recomposed into the frame canvas and handed to
  `cloudSendImage(id, fb, 240, 240, nullptr)`. The service copies it.
- A tap on the canvas while a popover is open only closes the popover.
  Buttons fire on release and only if the finger is still on the same slot.
- Side button = back to AppList. Swipe-right is paint, not back (controller.cpp).
- An empty page shows a quiet "Draw with your finger" hint, which the first
  stroke removes.
- The drawing lives in module statics (PSRAM), so it survives leaving the app
  while the watch stays powered. It isn't saved to flash.

## How drawing works
- **Undo by replay.** `sBase` (240x240 RGB565, PSRAM) holds everything older
  than the undo window. `sOps[17]` holds the last 16 edits (strokes and clears),
  each with a point range in `sPts` (2-byte points, 12288 of them, 24 KB PSRAM).
  Picture = base + replay(ops). Past 16 edits, or when the point pool fills, the
  oldest op is folded into the base.
- **Strokes.** Touch samples are joined with quadratic curves through the sample
  midpoints. A round brush (precomputed span table) is stamped every r/2 px.
  The live stroke is stamped straight into the frame canvas, and only the dirty
  area goes to the panel through `pushRect`. That area is split into boxes of
  about 40 px, so one long diagonal sample doesn't push its whole bounding box.
  No full flush happens per touch sample. `desiredFrameMs` is 16 while
  drawing, otherwise 50.
- Replay uses the same piece functions as live drawing, so undo is
  pixel-identical. The app proves this itself: after every edit it logs the
  FNV-1a sum of the drawing area, and on undo it compares the recomposed sum
  with the sum recorded at that step (`same=1`).
- **Undo of a stroke** recomposes and pushes only that stroke's box. Undo of a
  clear, entering the app and the return from the picker do a full flush.

## Memory
- PSRAM: 115,200 B base + 24,576 B point pool = 139,776 B, allocated on the
  first enter and kept. The send buffer is the frame canvas itself (no copy
  here; the cloud service copies it).
- Internal RAM: about 0.65 KB of statics (ops 136 B, span table 289 B,
  per-step sums 72 B, misc). No heap use per stroke.
- Flash: the module adds FreeSans12pt7b (about 2.7 KB, already used by Write)
  for the hint.

## Logs and console (tests)
- `DRAW: enter` / `stroke n= r= color= frames= avg_us= max_us=` (per-frame cost
  of live ink incl. the SPI push) / `op=stroke|undo|clear|enter ops= pts= bg=
  sum= same=` / `color=` / `size=` / `eraser=` / `clear bg=` /
  `send: pick contact` / `send: to= w=240 h=240 sum= same= queued=`.
- `draw state` on the console prints `DRAWSTATE ops=.. pts=.. ...`.

## Cloud stub (delete at merge)
`cloud_msg_stub.cpp` defines every `cloud_msg.h` function as a weak symbol,
marked `// STUB: deleted when merged with core/cloud_msg.cpp`. Linked as
"Ewan", contacts "Me" (self) and "Ewan". The picker calls back at once with
"Ewan". `cloudSendImage` logs `DRAW: stub send to= w= h= bytes= sum=` and sends
nothing. The real `core/cloud_msg.cpp` overrides it at link time, or define
`EW_CLOUD_MSG_IMPL`. Delete the file at merge. Also update the one stub-only
line in `test/draw/draw.ewt` (`stub\ssend`).

## Verified (2026-10-13, offline only; the watch was not touched)
- Watch builds (`ewt build -e ewatch_v20`, compile only):
  `EW_APPS="screenshare doom claudeface write draw"` OK (flash 78.9 %), and
  `EW_APPS="draw"` OK (66.8 %). Checked with `nm` that `DrawView::render` and the
  stub's `cloudSendImage` are in each ELF.
- ewsim_host: `test/draw/draw.ewt` PASS with `EW_APPS=draw` and with
  `EW_APPS="write draw"`. It covers two colours, a size change, a left-to-right
  stroke plus an injected chip SwipeRight (stays in Draw), undo with `same=1`,
  eraser, clear Cancel / White, undo of the clear, Send -> stub picker -> stub
  send with the same sum as the screen, `expect noerr`, and side button -> AppList.
- `test/draw/draw_mustfail.ewt` FAILs as it should, at the inverted undo check
  (`same=0`).
- Checker checks:
  - With the controller hook disabled, draw.ewt fails at `expect screen Draw`
    after the swipe. ewsim doesn't turn touch moves into chip gestures, hence
    the `gest 4` step.
  - `expect log` regexes must not contain spaces. The step is split on
    whitespace, so a regex with spaces silently checks only its first word.
    Use `\s`.
  - Sim objects can go stale after `git checkout` within the same second as a
    build. Delete the `.o` or touch the file.
- Live-ink cost per touch sample in ewsim (SPI charged at 60 MHz; a full frame
  is 17.9 ms):

  | stroke | avg | max |
  |---|---|---|
  | r=2 | 0.11 ms | 0.26 ms |
  | r=4 | 0.23-0.28 ms | 0.46 ms |
  | r=11 | 0.83 ms | 1.5 ms |
  | eraser r=16 | 0.60 ms | 1.0 ms |
  | 280 px diagonal in one sample | - | 5.4 ms (13.9 ms before chunking) |

  Undo of a stroke takes about 9 ms (box push), against 18 ms for a full flush.
  On the watch a full flush is 49-61 ms, about 3x the sim, so expect roughly
  1-5 ms per sample. That figure is not yet measured on hardware.
- Not verified: anything on the real watch (touch feel, frame cost, keep-awake),
  and the real contact picker and send.

## OS files changed (all guarded by `#if EW_APP_DRAW`)
- `src/apps/system/registry.cpp`: include, launcher entry ("Draw", `Icon::Brush`,
  pink), view instance, `viewFor` case.
- `src/apps/system/ui_icons.h` / `ui_icons.cpp`: `Icon::Brush` (last in the
  enum) and its glyph.
- `src/core/controller.cpp`: swipe-right isn't back on `Screen::Draw`.
- `src/core/power_mgr.cpp`: `Screen::Draw` blocks sleep while `drawKeepAwake()`
  (input in the last 60 s).
- `src/core/ewlog.cpp`: `draw state` console command (`drawConsole`).
- `src/core/model.h`: `Screen::Draw` was already there (unguarded, from the
  cloud contract commit). It is not changed here.
