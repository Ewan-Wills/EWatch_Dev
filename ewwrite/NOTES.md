# ewwrite: EWatchOS2.1 + the Write app (type by drawing letters)

Built 2026-10-07 by Claude Opus 5.5 (ewills172). A copy of EWatchOS2.1 (its
working tree on that date, including the uncommitted edits it had then) with one
new app, **Write**: draw a letter or digit with a finger, and it is typed.
The OS notes that came with the copy are in `NOTES_os21.md`.

**Status:** builds for `ewatch_v20` (flash 73.0%). Verified only in the offline
simulator (ewsim_host). **Not yet run on a watch**: real-finger accuracy and
recognition time on the ESP32 are unknown until it's tried (see "Hardware session").

## Using it
Launcher: **Write** (yellow pencil, under Stopwatch).

```
+--------------------------------+
|  HELLO 4|                      |  text line (last line, cursor; arrow = earlier lines)
| [H] [M] [N]   [AB] [CLR]       |  3 candidates, case, clear
|--------------------------------|
|                                |
|        draw here               |  ink area (y >= 96)
|                                |
+--------------------------------+
```
- **Letters/digits:** draw A-Z or 0-9 (uppercase shapes), in as many strokes as you
  like and in any stroke order or direction. When the finger stays up for **600 ms**,
  the strokes are recognised and the character is added.
- **Space:** one straight stroke left to right. **Backspace:** right to left.
  **Enter:** one straight stroke from top-right down to bottom-left (like `/`,
  the Palm Graffiti return). The straight-stroke test allows +-25 degrees for space and
  backspace, needs the stroke to be at least 50 px long, and checks how straight it is.
- **Chips:** the top 3 guesses. Tap another chip to **correct** the last
  character. The app also **learns** that drawing: it is saved as a template for the
  chosen character (see Teaching).
- **AB / ab:** upper or lower case output (recognition is always uppercase shapes).
- **CLR:** tap = clear the text. Long-press = also forget every learned drawing.
- **Side button** = back. **Swipe-right-to-go-back is switched off in Write**
  (controller.cpp), so a stroke drawn left to right doesn't leave the app.
- The screen stays on while you write: `power_mgr` blocks sleep for 60 s after the last touch.

## How it works
- `src/apps/write/write_recog.{h,cpp}`: the **$P point-cloud recogniser**
  (Vatavu, Anthony & Wobbrock, ICMI 2012), written from the paper's pseudocode, with
  two **$Q** speed-ups (MobileHCI 2018): squared distances and early abandoning.
  Every glyph is resampled to 32 points, scaled uniformly into a unit box and
  centred. Then it is greedily matched to each template from 5 start points in both
  directions. Because a glyph is just a set of points, stroke order and direction
  don't matter. The result is the best distance per character. Top-3 distinct
  characters, `score = 1 - 2 * RMS point distance` (1 = identical, 0 = the points are
  half a glyph-box off on average). Below **0.20** the glyph is rejected
  ("? not sure", nothing typed). Pure C++: the same file builds into the firmware,
  the simulator and the host bench.
- **Templates:** 80 built-in shapes, 1-4 per character, in
  `src/apps/write/write_templates.h` (2.8 KB in flash). That header is **generated**
  by `tools/write_glyphs.py`: the shapes are defined there with lines and arcs.
  The clouds are built once on first entry (about 20 KB, in PSRAM).
- **Edit gestures** are found geometrically before matching, because $P ignores
  direction and couldn't tell space from backspace: a single stroke whose chord is
  >= 90% of its path length and that bends less than 14% of its length.
- **Letter/digit look-alikes** (O/0, I/1, S/5, Z/2, B/8, G/6): if the top two are such a
  pair and within 0.06 of each other, the previous typed character decides (a digit
  after a digit, a letter after a letter). Logged as `ctx=1`.
- **Teaching:** each correction stores that drawing's normalised cloud as a user
  template (newest 3 per character, 72 in total) in LittleFS `/write_tpl.bin`
  (`WRT1` format, 65 bytes each). User templates compete with the built-in ones and
  are marked `user=1` in the log.
- **Shared text:** `src/apps/write/write_text.h`. `writeGetText(buf, cap)`,
  `writeSetText()`, `writeClearText()` and `writeTextRevision()` work from any task,
  up to 256 chars. The text is saved in NVS `ewwrite/text` when you leave the app or
  press CLR, and comes back after a reboot. Nothing sends it anywhere yet (no
  USB/BLE keyboard).
- **Settings in NVS (namespace `ewwrite`):** `gapMs` is the end-of-glyph pause
  (default 600, accepted 250-3000), `upper` is the case, `text` is the text.
  In the simulator: `--pref ewwrite.gapMs=800`.
- **Memory:** about 0.7 KB of internal static RAM (the view object and the text). The
  template clouds (about 20 KB), the ink buffer (4.6 KB), the user templates (18 KB)
  and the temporary file buffer are in PSRAM.

### Files changed in the OS copy
`core/model.h` (adds `Screen::Write` after Tamagotchi, before the `#if` gates, so
ewt's screen numbering is right), `apps/system/registry.cpp` (include, `vWrite`,
`viewFor` case, launcher row), `core/power_mgr.cpp` (`Screen::Write ->
writeKeepAwake()`), `core/controller.cpp` (no swipe-right back on Write),
`apps/system/ui_icons.{h,cpp}` (`Icon::Pen`). New: `src/apps/write/*`,
`tools/write_glyphs.py`, `tools/write_score.py`, `test/write*.ewt`, `test/host/`.

### Console log (for tests)
```
I/WRITE: char=H score=0.92 ms=0 cands=H:0.92,M:0.81,N:0.75 us=1 strokes=3 pts=22
I/WRITE: edit=space ms=0 us=1          (space | backspace | enter)
I/WRITE: reject score=0.12 ...         nothing typed
I/WRITE: fix from=T to=7  /  learn char=7 user=1  /  clear  /  case=lower  /  forget user=0
I/WRITE: text=HELLO 4|7                ('|' = newline, the last 47 chars)
```
`ms`/`us` = time for normalisation + matching. **In the simulator they read 0/1**,
because CPU time is free there. Only the watch gives real numbers.

## Verification (2026-10-07, all in ewsim_host, first-person)
| check | result |
|---|---|
| `ewt build ewwrite -e ewatch_v20` | built, flash 73.0% |
| `test/write.ewt` (H E L L O, space, 4 2, backspace, enter, a 2-stroke T in reverse order, correct T->7 by chip, the same T again comes back as 7 from the user template, `swipe r` stays in Write, `expect noerr`, side button -> AppList) | PASS |
| `test/write_sweep.ewt`: 216 synthetic finger draws (6 per character, random variant, perturbed, text cleared before each so no context), scored by `tools/write_score.py` | **top-1 210/216 = 97.2%, top-3 216/216 = 100%**. Misses: Z->I, 7->T, 0->O, G->0, 3->S, 3->5 |
| host bench, 3600 draws (100 per character, the same recogniser source) | top-1 97.0%, top-3 99.8%. Letters taken for an edit gesture: 0 of 3600 |
| **leave-one-variant-out** (test shapes withheld from the templates: a shape it has never seen) | **top-1 83.9%** (3000 draws). Worst: plain `I` when only serif-I is left (0/40), `U` with a tail, slashed `0`, open `4`. This is the closer guide to a new writer |
| edit gestures, 500 each, +-15 degrees, 90-150 px, bowed +-6%, jitter +-2 px | space, backspace and enter 500/500 each |
| user-template save/load round trip (bench `--selftest`) | PASS |
| checker can fail: `test/write_mustfail.ewt` (draws A, expects B) | exit 1 as wanted |
| checker can fail: sweep with `--pref ewwrite.gapMs=3000` (no glyph completes) | scorer: "recognitions=0, FAIL", exit 1 |
| checker can fail: labels shifted by one | scorer: 2.8% |
| screenshots looked at: empty screen, ink mid-glyph (finished strokes white, live stroke blue), chips after H, lowercase + newline marker, launcher pencil icon | look right |
| demo `media/write_hello.mp4` (HELLO, 960x1120, 30 fps, 22.6 s; sheet `media/write_hello.sheet.png`) | rendered and checked from the sheet |

What the synthetic draws are: the template shapes after random size (95-150 px),
position, aspect (+-15%), slant (+-0.2 shear), rotation (+-8 degrees), a smooth wobble
(up to 3% of the height), +-1.5 px jitter, shuffled stroke order, flipped stroke
directions, and 35% of multi-stroke glyphs joined into one stroke. Each draw is
sampled as a finger would be: `touch down/move/up` every 10-18 px, 60 ms per step.
They are still generated from my own shapes, so they are **optimistic**. The
leave-one-out figure is the honest lower guide.

**Speed (estimate, UNVERIFIED on the watch):** 0.56 ms mean on an M4 Pro Mac. One
recognition does about 348k point-distance evaluations (early abandoning saves 41%
compared with plain $P). At roughly 10-20 cycles each on the S3 at 240 MHz (the view
asks for 33 ms frames while a glyph is pending, which turns the 240 MHz mode on),
that is **about 15-30 ms**. The `ms=` field on the watch will give the real number. If
it's slow: drop kN to 24, or add $Q's lookup-table lower bound.

## Limits / unknowns
- **Real fingers are untested.** The touch chip's sampling (about 25 Hz TouchHold),
  finger size on a 1.69" screen and Ewan's handwriting may all differ from the
  synthetic draws. Correct-by-chip teaching is the fix for a writer's own shapes.
- Uppercase shapes only. Lowercase output comes from the AB/ab toggle. No punctuation.
- A single straight horizontal stroke is always space or backspace. If you pause
  for more than 600 ms after the top bar of a T, it becomes a space. Raise `gapMs` if
  that happens.
- I vs 1 (a plain line is I) and O vs 0 rely on the drawn shape plus the
  previous-character rule. Mixed strings like "O0" may need a chip tap.
- The CST816S can send gesture codes while you draw. They are ignored (only a long
  press on CLR does anything), and swipe-right-back is suppressed. Whether the real chip
  drops or merges touches during fast strokes is unknown.
- `ms` in the simulator is meaningless (see above).

## Adding or changing built-in templates
1. Edit the shapes in `tools/write_glyphs.py` (`G['X'] = [variant, ...]`; strokes are
   lists of points on a 100-unit-tall grid, y down; helpers `L()`, `arc()`, `seq()`).
2. `python3 tools/write_glyphs.py header` regenerates `src/apps/write/write_templates.h`.
3. Measure:
   ```
   make -C test/host && python3 tools/write_glyphs.py bench /tmp/d.txt 23 100 1.0 && test/host/bench /tmp/d.txt
   python3 tools/write_glyphs.py loo test/host/bench /tmp      # leave-one-variant-out
   python3 tools/write_glyphs.py tests                          # regenerate test/*.ewt
   cd ../ewsim_host && ./ewsimrec ewwrite --snap /tmp/s ../ewwrite/test/write_sweep.ewt -v 2>/tmp/sweep.log
   python3 ../ewwrite/tools/write_score.py /tmp/sweep.log
   ```
   Note: `tests` regenerates the sweep from the current shapes, and its draws change
   when the shapes change.
4. Or on the watch, without code: draw the letter, tap the right chip (teaching).

## Hardware session (another session owns the watch; nothing here touched it)
```
EWT="python3 $HOME/Documents/Claude Knowledge Base/firmware/knowledge/ewt.py"
PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps $EWT flash ewwrite
$EWT -P ewwrite run test/write.ewt          # smoke: letters, edits, chip fix, learning, swipe-r, back
$EWT -P ewwrite tail 5 -g WRITE             # while Ewan writes by hand: real ms= and scores
$EWT -P ewwrite record write.mp4 "goto Write; wait 20000" --short   # real-finger footage
```
On the watch, check: real `ms=` per recognition (the estimate is 15-30 ms); no stalls
(the render task does the matching); that the CST816S doesn't cut strokes up; that
Ewan's own letters are recognised; that the text survives a reboot (NVS); and that
`/write_tpl.bin` survives one too. `test/write_sweep.ewt` also runs with ewt (about 14 min of
watch time over USB; score it with `tools/write_score.py` on an `ewt tail` capture).
