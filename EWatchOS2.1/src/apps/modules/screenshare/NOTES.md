# screenshare module notes

Carried over from the stand-alone `screenshare/` folder it was developed in
(before 2026-10-09). OS-wide parts of these notes may be out of date.

---

# screenshare — build notes (EWatchOS2.1)

Mirror a chosen Mac window/region onto the EWatch face, live, over the USB
cable. The watch is a pure JPEG sink; all capture/scaling/encoding happens on
the Mac.

**This app now runs on the full EWatchOS2.1** (watch faces, launcher, settings,
WiFi, Spotify, games, etc.), with `Screenshare` added as a launcher app. It was
ported from the older BaseOS scaffold — see "Port to OS2.1" below for exactly
what changed. The view/decode logic and the `pc/` companion are unchanged.

## What it does

- Watch app `Screenshare` (launcher: swipe up → **Screenshare**, in the APPS
  section after Island).
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

## Port to OS2.1 — what changed from the BaseOS version

The scaffold is the current OS2.1. Only registration + frame-pacing wiring
changed; `src/apps/screenshare.cpp` logic and `pc/` are unchanged apart from
the two points noted.

- **Registration moved to `src/apps/system/registry.cpp`** (BaseOS used
  `view.cpp`). Added, mirroring the bookreader port:
  - `src/core/model.h`: `Screenshare,` in the `enum class Screen` user-app
    region (right after `Island,`).
  - `registry.cpp`: `#include "screenshare.h"`; `static ScreenshareView
    vScreenshare;`; `case Screen::Screenshare: return &vScreenshare;` in
    `viewFor()`; and one launcher tile
    `{ "Screenshare", Icon::Photo, aura::kTeal, Screen::Screenshare, nullptr }`
    in `kTopApps[]`.
- **Keep-awake moved to `src/core/power_mgr.cpp` → `screenBlocksSleep()`**
  (BaseOS used controller.cpp's `blockSleep`). Added
  `case Screen::Screenshare: return true;` so the panel stays lit while a
  stream is running with no touch input.
- **Continuous redraw uses `desiredFrameMs()`, not `model.revision++`.** OS2.1's
  render task only re-renders on events + a 1 Hz heartbeat unless the view asks
  for animation. `ScreenshareView::desiredFrameMs()` now returns `33` (~30 fps)
  so the view redraws continuously — this is what pumps the serial parser and
  pushes decoded frames to the panel. The old per-cycle `model.revision++` bump
  in `render()` was removed (it did nothing useful on OS2.1).
- **WiFi is kept ON.** BaseOS gated WiFi off (`-DEWATCH_ENABLE_WIFI=0`); OS2.1
  leaves the full OS intact (Spotify/Gallery/WiFi settings all present).
  Screenshare still uses only USB for frames; WiFi is simply not touched.
- **JPEGDEC** is already an OS2.1 dependency (`bitbank2/JPEGDEC` in
  `platformio.ini` `lib_deps`, pre-populated in `.pio/libdeps`). Nothing
  re-added or vendored. No BaseOS-era lib_deps/vendoring to strip — the
  scaffolded `platformio.ini` is already the OS2.1 one.
- `main.cpp` was **not** touched: the serial listener is self-contained in
  `render()`/`pumpSerial()` driven by the frame pacing above.

## Design / firmware wiring

- JPEG scratch buffer is one 96 KiB PSRAM allocation (`ps_malloc`, heap
  fallback), allocated on `onEnter()`, freed on `onExit()`. The 134 KiB frame
  canvas is the shared lazy PSRAM buffer (`frameCanvas()`).
- `pumpSerial()` drains all buffered USB-CDC bytes each render cycle and decodes
  every complete, checksum-valid frame it assembles — so one render() can emit
  several queued frames; the 33 ms pacing is only the minimum wake rate.

## Compile status

`pio run -e ewatch` compiles clean with the isolated core:

```
RAM:   [=======   ]  66.6% (used 218372 bytes from 327680 bytes)
Flash: [=====     ]  54.3% (used 1709401 bytes from 3145728 bytes)
========================= [SUCCESS] Took 85.40 seconds =========================
```

(RAM/Flash are higher than the BaseOS build because OS2.1 is the full OS with
WiFi + Spotify + games, not the stripped BaseOS copy.)

## What is MVP / stubbed / not verified

- **Compilation only has been verified. Nothing has been run on hardware** — no
  flash, no end-to-end stream test. Timing, real fps, decode latency and CDC
  throughput are all unmeasured estimates. **Only Ewan flashes & accepts.**
- The ~30 fps `desiredFrameMs()` ceiling is a wake-rate target, not a measured
  throughput. Effective fps is bounded by decode time and CDC bandwidth; if it
  stutters, lower `--fps`/`--quality` (watch side needs no change).
- `--window` capture is macOS-only and needs `pyobjc`; without it, use
  `--region`. Multi-monitor coordinate math relies on `mss` monitor geometry.
- No on-watch fps/stat overlay (kept the hot path clean); the Mac side prints
  throughput instead.
- No flow control / ACK pacing — the Mac sends at its target fps and the watch
  consumes best-effort. If the CDC buffer ever backs up, frames queue on the
  host; latency grows but frames stay intact (checksum-guarded).

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

## 2026-10-05 — serial hardening (console pause + block flow control)

Ported the fixes found in ShortsApps/bookreader. Built `[SUCCESS]`; NOT yet
verified on the watch.

- **Console stole frame bytes (trap A).** The ewlog console task (`ewcon`)
  reads Serial continuously, so it raced `pumpSerial()` for JPEG bytes.
  `ScreenshareView::onEnter` now calls `ewlogConsolePause(true)`, `onExit`
  calls `ewlogConsolePause(false)`. No background task reads Serial.
- **HWCDC RX overflow (trap B).** The companion used to write whole frames
  (4-15 KiB, up to 12/s) with no wait, while the watch only drains every
  33 ms render tick; anything past the 4 KiB RX queue was silently dropped
  (frames failed csum). Protocol is now block-ACKed, both sides:
  PC sends header + <=2048 B, waits `>>ACK <got>`, next 2048 B ...; the last
  block carries the csum and the PC waits `>>FRAME OK|BAD <n>` before the next
  frame. `kBlock` (screenshare.cpp) must equal `BLOCK` (pc/screenshare.py).
  While a frame is in flight `pumpSerial()` keeps polling (<=100 ms idle,
  <=500 ms per call) so each block does not cost a render tick. A frame that
  stalls >1 s mid-way is dropped and the parser resets; the PC, after a 2 s
  reply timeout, sleeps 1.2 s, flushes input and starts a fresh frame.
- **TX replies stuck in HWCDC buffer (trap C).** `Serial.flush()` after every
  ACK/FRAME/READY/BYE line.
- Not applicable here: trap D (app only draws built-in-font text at fixed
  positions; the JPEG blit is already clipped to 240x280), trap E (no mutex
  held in render()/pumpSerial()).

Hardware check to do: run `python3 pc/screenshare.py --fps 12 --quality 50`
and confirm the fps line shows ~8-12 fps with no `>>FRAME BAD` lines.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-06
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to Screenshare, 'Waiting for Mac...' idle screen drawn, 0 errors (6 steps). Snap: `.ewt/boot.png`. The app owns USB serial, so the smoke runs with `raw keepcon 1` (ewlog test hook): the console stays with ewt and the app's PC RX reader is deliberately inert.
- NOT exercised: the live PC-data path (pc/ companion over USB serial). ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
