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
  'U' upload : u16 nameLen | u32 dataLen | name | data | u32 crc32(data)
  'L' list   : (no args)
  'D' delete : u16 nameLen | name
```

Watch → PC, newline ASCII lines, all prefixed `>>BOOK ` so boot/log noise is
easy to ignore:

```
>>BOOK READY                 (listener started)
>>BOOK RECV <name> <len>     (header parsed, receiving)
>>BOOK OK   <name> <len>     (written + CRC verified)
>>BOOK ERR  <reason>         (hdr/size/name/write)
>>BOOK LIST <n> + >>BOOK FILE <name> <size>  (for 'L')
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
