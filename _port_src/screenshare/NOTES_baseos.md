# screenshare — build notes

Mirror a chosen Mac window/region onto the EWatch face, live, over the USB
cable. The watch is a pure JPEG sink; all capture/scaling/encoding happens on
the Mac.

## What it does

- Watch app `Screenshare` (launcher: swipe up → **Screenshare**).
- Shows **"Waiting for Mac…"** until the first frame lands; drops back to that
  banner if the stream stalls for >1.5 s (companion quit / cable out).
- Reads framed JPEGs off USB CDC, decodes each with **JPEGDEC** straight into
  the shared 240×280 frame canvas (`frameCanvas()->getFramebuffer()`), then
  `flush()`es the whole buffer to the panel.
- Back = side button (`ButtonShort`) or right-swipe, or tap the top-left back
  chevron → returns to the launcher.
- Haptic tick on back.

Mac companion: `pc/screenshare.py` (capture via `mss`, scale/letterbox via
`Pillow`, JPEG-encode, stream over `pyserial`). See `pc/README.md`.

## Wire protocol (USB CDC, 115200 / native-USB FS)

Little-endian, one frame per capture:

```
'E' 'W'          2 magic bytes (0x45 0x57)
uint32  len      JPEG payload length (LE)
len bytes        baseline JPEG, already scaled to <= 240x280
uint8   csum     XOR of all `len` payload bytes
```

- The watch parser is a resync state machine: it hunts for the `EW` magic, so
  boot-log text and partial frames can't wedge it. A length that is 0 or larger
  than the 96 KiB guard is treated as desync and dropped.
- The trailing XOR checksum is verified before decode; bad-checksum or
  failed-decode frames are counted (`bad`) and skipped, not shown.
- Watch→Mac telemetry is plain ASCII, prefixed `>>` so the PC parser ignores
  it: `>>READY screenshare` on entry and every ~0.5 s while waiting;
  `>>BYE screenshare` on exit.

## How to run the Mac side

```sh
cd pc
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 screenshare.py                      # auto-detect port, primary display, 12 fps
python3 screenshare.py --region 0,0,1280,720 --fps 15 --quality 55
python3 screenshare.py --window "Safari"    # needs pyobjc (see README)
```

macOS will ask for **Screen Recording** permission on first run — grant it to
the terminal/Python, then re-run. Full flag list in `pc/README.md`.

## Design / firmware wiring

- Registered at the 4 standard sites: `Screen::Screenshare` enum
  (`src/core/model.h`); `#include "screenshare.h"`, `static ScreenshareView
  vScreenshare;`, `viewFor()` case, and a `kTopApps[]` tile
  (`src/apps/system/view.cpp`).
- **WiFi gated off** (`-DEWATCH_ENABLE_WIFI=0` in `platformio.ini`) — no
  networking; frames come over USB.
- **Auto-sleep suppressed while active:** the render loop in
  `src/core/controller.cpp` now treats `Screen::Screenshare` like `PowerOff` in
  its `blockSleep` check, because frames arrive with no touch activity to reset
  the idle timer. (Leaving the app re-enables normal sleep.)
- `render()` bumps `model.revision` each cycle so the render task keeps calling
  it, which is what pumps the serial parser. Effective ceiling is ~15–20 fps
  (one decode per render; the render loop idles up to 50 ms between cycles).
- JPEG scratch buffer is one 96 KiB PSRAM allocation (`ps_malloc`, heap
  fallback), freed on `onExit()`. The 134 KiB frame canvas is the shared lazy
  PSRAM buffer.
- Added dependency `bitbank2/JPEGDEC @ ^1.6.0` (v1.8.4 vendored into
  `.pio/libdeps/ewatch/JPEGDEC` for the offline build, copied from the sibling
  Spotify_Controller project).

## Compile status

`pio run -e ewatch` compiles clean with the isolated core:

```
RAM:   [=         ]  12.5% (used 40916 bytes from 327680 bytes)
Flash: [==        ]  15.4% (used 483289 bytes from 3145728 bytes)
========================= [SUCCESS] Took 19.40 seconds =========================
```

## What is MVP / stubbed / not verified

- **Compilation only has been verified. Nothing has been run on hardware** — no
  flash, no end-to-end stream test. Timing, real fps, decode latency and CDC
  throughput are all unmeasured estimates.
- The ~15–20 fps ceiling and "frames decode < WDT" claim are reasoned from the
  render-loop structure, **not measured**. If decode is slower than expected,
  lower `--fps`/`--quality` (watch side needs no change).
- `--window` capture is macOS-only and needs `pyobjc`; without it, use
  `--region`. Multi-monitor coordinate math relies on `mss` monitor geometry.
- No on-watch fps/stat overlay (kept the hot path clean); the Mac side prints
  throughput instead.
- No flow control / ACK pacing — the Mac sends at its target fps and the watch
  consumes best-effort. If the CDC buffer ever backs up, frames queue on the
  host; latency grows but frames stay intact (checksum-guarded). A future
  improvement would be watch→PC `>>ACK` pacing.

## Risks

- **CDC throughput vs fps** (flagged in the spec): a busy 240×280 frame at q55
  is ~10–15 KiB; at 15 fps that's ~150–225 KiB/s. Native-USB FS has headroom,
  but decode time may bind before bandwidth does. Start at the defaults (12 fps,
  q50) and tune down if it stutters.
- **Per-frame decode vs 20 s task WDT:** a single decode is far under the WDT,
  and serial draining is bounded per cycle; no busy-loops. Believed safe but
  unmeasured.
- Boot-log bytes that happen to contain `EW` + a plausible length could, very
  rarely, cost one dropped frame on first connect; the checksum rejects it and
  the parser resyncs.

## EXACT steps for Ewan to flash + verify on hardware

1. Build + flash + monitor (isolated core):
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/screenshare
   ~/.platformio/penv/bin/pio run -e ewatch -t upload -t monitor
   ```
2. On the watch: from the watch face swipe up → tap **Screenshare**. You should
   see **"Waiting for Mac…"**. The monitor should print `>>READY screenshare`
   roughly twice a second. (Quit the PlatformIO serial monitor before step 4 —
   only one program can own the port.)
3. Set up the Mac companion once:
   ```sh
   cd pc
   python3 -m venv .venv && source .venv/bin/activate
   pip install -r requirements.txt
   # optional, for --window:  pip install pyobjc-framework-Quartz
   ```
   Grant **Screen Recording** permission when macOS prompts, then re-run.
4. Start streaming (watch still on the Screenshare screen):
   ```sh
   python3 screenshare.py --fps 12 --quality 50
   ```
   It auto-detects `/dev/cu.usbmodem*`; pass `--port` if needed.
5. **Verify (first-person):** the watch face shows the live Mac capture within
   ~1 s and updates continuously. The Mac prints an `fps`/`KiB/s` line every 2 s.
   Try `--region`/`--window` to mirror a specific window. Press the side button
   to return to the launcher (watch prints `>>BYE screenshare`).
6. If it stutters or lags, drop `--quality` to 40 and/or `--fps` to 8–10 — no
   firmware change needed.
