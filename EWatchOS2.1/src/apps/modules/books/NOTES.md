# books module notes

Carried over from the stand-alone `bookreader/` folder (before 2026-10-09), which was built on
an older OS; its launcher extensions (row subtitles, data-driven list hooks) and the Book icon
are OS core now. OS-wide parts of these notes may be out of date.

---

# bookreader — build notes & hardware-verify guide

Read plain-text books on the EWatch. Upload a `.txt` over USB; it's stored in
LittleFS; read it with smooth pixel scrolling, selectable font size and
auto-scroll; the watch remembers your place per book.

**Status:** compiles clean (`pio run -e ewatch` → SUCCESS, Flash 16.6 %, RAM
10.8 %). Not flashed — only Ewan verifies on hardware (see below).

---

## What it does

- **Library screen** (`Screen::BookLibrary`, launcher tile "Books") — lists
  every `/books/*.txt` in LittleFS with its size. Tap a row to open; swipe
  up/down to scroll a long list; long-press a row → delete (with a
  Cancel/Del confirm). Opening the screen starts the serial-upload listener.
- **Reader screen** (`Screen::BookReader`) — word-wrapped text drawn into the
  shared `frameCanvas()` (double-buffered, flicker-free):
  - **Smooth pixel scrolling** — the view keeps a floating pixel position
    (`scrollPx`) that eases toward a goal every frame (~20 fps), so swipes
    glide and auto-scroll is continuous rather than line-jumping.
  - **Swipe up / down** — page down / up (~90 % of a screen, animated).
  - **Tap** — toggles the bottom control panel: font size `A- [S/M/L] A+`,
    auto-scroll speed `- [px/s] +`, and **Play/Pause**. **Double-tap**
    anywhere also toggles Play/Pause.
  - **Font size** — 3 steps (FreeSans 12 pt / FreeSerifBold 12 pt /
    FreeSerifBold 24 pt). Changing it re-indexes the open book and keeps your
    place (position is stored by byte offset, so it survives font changes).
  - **Progress bar** — 2 px accent bar along the bottom edge.
  - **Back** — hardware button (SW2), a right-swipe (both arrive as
    `ButtonShort`), or the top-left `<` chevron → saves position, returns to
    the Library.

## Paging from flash (not whole-book-in-RAM)

On open, the book is scanned **once** and wrapped to the 224 px text width.
Only the **byte offset of each line start** is stored, in a growable PSRAM
array (`heap_caps_realloc(MALLOC_CAP_SPIRAM)`, capped at 300 000 lines). The
raw text is **never** held in RAM. Each frame, only the visible lines
(~8–11) are `seek()`+`read()` from LittleFS and whitespace-normalised for
drawing. A ~500 KB book → ~2 MB of index at most; the panel's 896 KB flash
partition caps real books well below that.

## Persistence

- **Reading position** — saved per book as the top-of-screen **byte offset**
  in NVS namespace `bookrdr`, key `p<fnv1a-hash-of-path>`. Written on exit,
  on Back, and every 3 s while reading. On reopen it binary-searches the line
  index for that offset, so your place is restored even if the font changed.
- **Font size & auto-scroll speed** — global, in the same `bookrdr` namespace
  (`font`, `speed`).
- This namespace is **separate** from the system `ewatch` settings schema, so
  it adds nothing to `storage.cpp` / `model`.

## Storage / filesystem

- **LittleFS** on the existing `spiffs` partition from `huge_app.csv`
  (0x310000, **0xE0000 = 896 KB**). No custom partition CSV needed —
  `LittleFS.begin(formatOnFail=true)` uses the default `spiffs` label and
  formats it as LittleFS on first boot. Books live under `/books/`.
- All FS access (upload task **and** render task) is serialised by a mutex
  (`gFsMutex`), because LittleFS is not re-entrant.

---

## Serial upload protocol (USB CDC, 115200 / full-USB-speed)

PC → watch, all integers **little-endian**:

```
magic "EWBK" | cmd
  'U' upload : u16 nameLen | u32 dataLen | name, wait RECV,
               then data in 2048-byte blocks (wait ACK after each), then u32 crc32(data)
  'L' list   : (no args)
  'D' delete : u16 nameLen | name
```

Watch → PC, newline ASCII lines, all prefixed `>>BOOK ` so boot/log noise is
easy to ignore:

```
>>BOOK READY                 (listener started)
>>BOOK RECV <name> <len>     (file open, send the first block)
>>BOOK ACK  <total>          (block written; send the next)
>>BOOK OK   <name> <len>     (written + CRC verified)
>>BOOK ERR  <reason>         (hdr/size/name/space/open/read/flash/crc)
>>BOOK LIST <n> + >>BOOK FS used= total= + >>BOOK FSFILE ... + >>BOOK FILE <name> <size>  (for 'L')
>>BOOK DEL  <name>           (for 'D')
```

- The listener runs as its own FreeRTOS task (`bookup`, core 0), started the
  first time the Books app is opened. It scans the RX stream for the `EWBK`
  magic, so stray bytes are ignored.
- Upload streams to `/books/.incoming`, CRC32-checks the payload, then
  atomically `rename()`s to the final name. A bad CRC or short transfer
  discards the temp file. CRC is the standard IEEE/zlib CRC-32 (matches
  Python's `zlib.crc32`).
- Filenames are sanitised: basename only, `[A-Za-z0-9._ -]`, `.txt` enforced.
- Why blocks + ACK: the USB-Serial/JTAG driver (HWCDC) drops RX bytes when
  its queue is full and has no flow control, so streaming a whole book lost
  most of it. RX queue is raised to 4 KB in `main.cpp`; replies are
  `Serial.flush()`ed because HWCDC otherwise holds short lines in TX.
- The ewlog debug console (`ewcon`) also reads Serial; it is paused while the
  Library is on screen or an upload runs (`ewlogConsolePause`), else it eats
  upload bytes ("unknown: '<book text>' (try help)").
- The FS lock is taken per block, never across the upload: the Library
  rescans under the same lock from the render task, and a render task blocked
  for >20 s trips the task WDT.
- A chip reset drops the LDO latch (GPIO17), so a WDT/panic reset powers the
  watch off instead of rebooting, and `pio run -t upload` fails with
  "Device not configured". **Hold the power button while flashing** and let go
  at "Hard resetting" (a 3 s hold in running firmware powers off).

PC side: `pc/upload.py` (pyserial). See `pc/README.md`. Quick:
`python3 upload.py mybook.txt` (auto-detects the port).

---

## Power / sleep behaviour

The system auto-sleeps after `sleepTimeoutSec` (default **5 s**) of no input.
That would sleep mid-page, so `controller.cpp`'s `blockSleep` chain now also
blocks sleep on `BookReader` and `BookLibrary`. To avoid staying lit forever,
each view self-navigates back to `Watch` after its own idle window
(**Library 30 s, Reader 60 s**; auto-scroll/animation count as activity, and
an in-progress upload holds the screen). After that the normal auto-sleep
resumes. (This is the one edit outside `src/apps/` — a 3-line change to the
documented `blockSleep` OR-chain, exactly as BaseOS/CLAUDE.md §10 suggests.)

---

## What's stubbed / simplified (honest list)

- **Not flashed, not run.** Only compilation is verified. Scroll smoothness,
  touch hit-zones, index timing and typography are unverified on real
  hardware — see verify steps.
- **No in-firmware way to create books** — they only arrive via the uploader.
- **Font "size" ladder is 3 fixed GFX fonts** (12/12/24 pt), not arbitrary
  sizes; only 12 pt and 24 pt glyph sets exist on the include path.
- **Index build is synchronous** on the render task in `onEnter`. It feeds the
  task WDT every ~8 KB; a very large book (hundreds of KB) will pause for
  roughly a second when opened. No progress spinner yet.
- **One book open at a time** (single global `File` + index). Fine for a reader.
- Library `>>BOOK FILE` list and on-watch long-press delete are included
  (spec marked them optional).

## Known risks to check on hardware

- **Ascent/line-height** are derived at runtime from `getTextBounds("Mg")` +
  `font->yAdvance`. If a font renders cramped or clipped, tune `gLineH` /
  `ascent` in `drawFrame()`.
- **Touch coordinates** — the control-panel button rects and the back chevron
  hit-box are hand-placed; verify taps land (CST816S origin, 240×280).
- **LittleFS first mount** formats the `spiffs` partition. If that region held
  a real SPIFFS image from another firmware, its contents are wiped (expected
  for this standalone app; the partition is otherwise unused here).
- **Upload while reading** — uploads write a *different* file than the one
  open, and all FS ops share a mutex, but this path is untested on hardware.

---

## EXACT hardware-verify steps (for Ewan)

1. **Flash + monitor:**
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/bookreader
   ~/.platformio/penv/bin/pio run -e ewatch -t upload -t monitor
   ```
2. On the watch: swipe up from the watch face → tap **Books**. Expect an empty
   library ("No books yet…") and `>>BOOK READY` in the monitor.
3. **Close the monitor** (Ctrl-C), then upload a book:
   ```sh
   cd pc && pip install -r requirements.txt
   python3 upload.py /path/to/some_book.txt
   ```
   Expect `>>BOOK OK …`. The book appears in the Library within a second.
4. **Open it** (tap the row). Confirm: text is word-wrapped to the width, no
   clipping at line edges, no flicker.
5. **Scroll:** swipe up → it should glide (not jump) one screen; swipe down to
   go back. Check the bottom progress bar advances.
6. **Controls:** single-tap → panel appears. Tap **A+/A-** (text re-wraps,
   place kept), **speed +/-**, then **Play** → text auto-scrolls smoothly;
   **Pause** stops it. Double-tap toggles play/pause too.
7. **Position memory:** scroll to the middle, press **SW2** (back) to the
   Library, reopen the book → it should return to roughly where you left off.
   Power-cycle (SW2 long-press off, then on) and reopen → place still restored.
8. **Delete:** long-press a library row → **Del** → it disappears; confirm via
   `python3 upload.py --list`.
9. **Idle/sleep:** leave the reader untouched ~60 s (not auto-scrolling) → it
   should return to the Library, then the watch sleeps normally.

If anything in 4–6 looks off (spacing, hit-zones, speed), the tunables are all
in `src/apps/bookreader.cpp` (`kReaderFonts`, `kSpeeds`, `gLineH`/`ascent`,
button rects in `drawFrame`/`onEvent`).

## Reader crash fixed (2026-10-05): GFX custom-font text above y=0

Opening a book crashed (`LoadProhibited` in the GPIO ISR, backtrace through
`ReaderView::drawFrame` -> `Arduino_GFX::drawChar`). Arduino_GFX 1.4.9's
custom-font `drawChar` only clips right/bottom; glyph pixels above y=0 are
written before the frame canvas buffer and corrupt the heap (here the GPIO
ISR arg table). `drawFrame` now skips any line whose tallest glyph would
cross the top edge. Left edge is safe (min xOffset -1 vs MARGIN 8).

## Pages, go-to and position (2026-10-05)

- A **page** is one screenful of whole lines (`TEXT_H / lineHeight`: 9 lines
  at S/M, 4 at L). Swipe up/down moves exactly one page, snapped to a page
  boundary.
- Tap the text to open the control panel. Top row shows **Pg P.L of N**
  (page, line within page, total pages). Buttons: page -10 / -1 / +1 / +10
  (jump to line 1 of that page), line - / +, then font, speed, play. Taps
  inside the panel that miss a button keep it open; a tap above closes it.
- Position is saved as the top line's byte offset (NVS `bookrdr`, key
  `p<fnv1a(path)>`) from the scroll *goal*, so leaving mid-scroll keeps the
  page you were going to. Written on exit (back, button, idle-to-Library),
  after every panel jump, and by the 3 s autosave only when it changed.
  Byte offset, not line number, so a font change keeps your place.
  A 3 s button hold (power off) skips onExit: at most ~3 s of reading lost.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Library = launcher list; tougher reader touch (2026-10-05)

- `LibraryView` now subclasses `AuraListView` (the app launcher list): same
  cards, momentum scroll, transitions, Book icon (new `Icon::Book`, also on
  the launcher tile). Rows: name (".txt" dropped) + "NN KB". Tap opens; hold
  ~0.65 s opens a Delete/Cancel dialog acted on finger-up. Empty state text.
  AuraListView gained hooks for this: `setEntries`, `activate`, `longPress`,
  `drawOverlay`, `interceptEvent`, and an optional `AuraEntry::sub` line;
  labels too wide for size 2 drop to size 1. Max 40 rows (kMaxRows).
- Reader tap detection: a tap = released within 450 ms, moved <= 12 px, no
  swipe in the last 300 ms, and no swipe gesture within 160 ms after release
  (acted on then). Swipes/drags can no longer open the menu or hit buttons.
- Opening the menu pauses auto-scroll; closing it (tap above the panel, or the
  button) resumes it if it was running. Play/Pause in the panel sets the
  state and closes the menu. Button press with the menu open just closes it.

## Reader menu: swipe-in, full screen (2026-10-05)
- Menu opens ONLY with a left swipe (finger right-to-left) and slides in from
  the right (eased, ~200 ms). Right swipe closes it (the OS delivers swipe-right
  as ButtonShort; the button closes it too). Taps never open it.
- Full screen, big targets: page -10/-1/+1/+10 (52x48), Line -/+ (110x42),
  Font -/value/+, Speed -/value/+ (48x42), Play/Pause (closes, sets scroll),
  Books (back to the library). Hit-testing only once fully slid in.
- While the menu is open, vertical swipes and double-tap are ignored. With it
  closed: vertical swipes page, double-tap toggles auto-scroll, the chevron
  (tap) goes back.
