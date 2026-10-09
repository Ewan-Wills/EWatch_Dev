# hidctl module notes

Carried over from the stand-alone `hidctl/` folder it was developed in
(before 2026-10-09). OS-wide parts of these notes may be out of date.

---

# hidctl — control the watch with a keyboard / mouse

**Status: compiles clean on EWatchOS2.1 (not flashed, not hardware-verified).
Honest MVP.**
Final build: `env:ewatch` SUCCESS, Flash 54.5% (1.71 MB), RAM 61.7%.

> **Ported to EWatchOS2.1.** This app was re-scaffolded onto the current
> shipping OS (watch faces, launcher, settings, WiFi, Spotify, games) and the
> on-watch Notes half was re-registered using the OS2.1 app APIs. See
> **§9 Port notes (OS2.1)** at the bottom for exactly what changed from the
> old BaseOS build. The flash footprint is much larger than the old BaseOS
> figure (16.9%) because the whole latest OS is now included and WiFi is left
> ON per the OS2.1 house rule — this app simply adds itself to it.

This was flagged as the highest-uncertainty app. Here is exactly what is real,
what is stubbed, and what Ewan must do to verify.

---

## 1. Feasibility verdict (the ≤30-min check)

**USB-host is NOT wired. Confirmed from the hardware, not guessed.**

- `src/drivers/pins.h` / `PINOUT.md` account for every GPIO on the board. There
  is **no VBUS-source / host-enable / 5 V-boost pin** anywhere — I grepped the
  whole tree for `vbus|usb_host|boost|5v|otg` and found nothing.
- The single USB connector is the ESP32-S3 **native-USB device** port, used for
  charging and flashing (`ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1` in
  `platformio.ini`). The watch is powered by a soft-latch LDO off the battery;
  it has no way to *source* 5 V onto VBUS to run a bus-powered keyboard/mouse.
- Even with a software USB-host stack, a watch cannot power a peripheral it
  can't feed VBUS to. So USB-host HID is a dead end on this hardware. Verdict
  reached well inside the 30-minute box.

**The real wireless path is BLE-HID host (central).** The S3 radio is BLE-capable
(the firmware already links bluedroid BLE for RF Diag). A watch acting as a BLE
HID *central* can pair a BLE keyboard/mouse. That is the right long-term answer.

**Why I did NOT ship BLE-HID host in this MVP (honest):** a robust HID central on
arduino-esp32 2.0.17 means scanning + connecting, discovering the HID service
(0x1812), subscribing to the input-report characteristics, **and parsing each
device's HID report descriptor** to turn raw report bytes into key/mouse events
(boot-protocol helps for keyboards but mice and many BT keyboards only offer
report protocol). That descriptor parser is the hard, device-specific part. It
needs NimBLE-Arduino added as a dep and — critically — **real BLE devices to
test against**, which I cannot do (I don't flash and can't pair hardware). The
spec explicitly allows the pivot when "the lib story is too heavy for MVP", so I
took the documented fallback rather than ship an untested BLE parser that
"compiles" but has never seen a real report descriptor.

## 2. Path taken

Built the **full on-watch half** behind a clean, transport-agnostic input seam,
and drove it with a **USB-serial feeder** so the hero demo works today:

- **Notes editor** (`src/apps/hidctl.{h,cpp}`): a text buffer (1024 chars),
  caret with insert/backspace/arrow/Home/End, word-wrapped and drawn in
  FreeMono 12pt, scrolls to keep the caret visible. **Persisted to LittleFS**
  at `/notes.txt` — loaded on entry, saved on exit and on the `Save` button.
- **Mouse cursor overlay**: a small arrow driven by relative mouse deltas,
  clamped to the screen. A left-click is routed through the **same**
  `handleTap(x,y)` path as a finger tap, so the mouse can press `Clear`, `Save`,
  and the back chevron. Finger touch still works in parallel.
- **Input seam** (`hid::hidFeedKeyChar / hidFeedKeySpecial / hidFeedMouseMove /
  hidFeedMouseButton` in `hidctl.h`): the transport-agnostic boundary. Today a
  serial feeder calls these; a future BLE-HID central calls the identical
  functions and **nothing in the Notes/cursor code changes**. That seam is the
  real engineering value here.
- **Serial feeder** (`pc/hid_feeder.py`): reads the Mac's real keyboard + mouse
  with `pynput` and streams the protocol below over USB-CDC. See `pc/README.md`.

Reachable from the launcher: swipe up from the watch face → **Notes** tile
(registered in `src/apps/system/registry.cpp` + `Screen::HidCtl` in
`src/core/model.h` — this is the OS2.1 registration path; see §9).

WiFi is left **ON** (`-DEWATCH_ENABLE_WIFI=1`) per the OS2.1 house rule — the
goal is "the latest full OS + this app added", not a stripped build.

## 3. Serial protocol (PC → watch)

Newline-terminated ASCII, one command per line. The watch ignores any line it
doesn't recognise, so boot logs / stray text are harmless.

| Line | Meaning |
|---|---|
| `KC <n>`       | key char, `n` = decimal ASCII 32..126 (e.g. `KC 65` = `A`) |
| `KS <NAME>`    | special: `ENTER BACK TAB ESC LEFT RIGHT UP DOWN HOME END` |
| `MM <dx> <dy>` | relative mouse move, signed decimals (e.g. `MM 4 -3`) |
| `MB <0\|1>`    | left button: `1` = press (= click at cursor), `0` = release |

Watch → PC: prints `>> HIDCTL ready` once when the Notes screen opens (a
handshake cue for the feeder). All other watch log lines are boot/diagnostic
noise the PC side can ignore.

## 4. How the seam is wired (so the next person can add BLE)

- The feeder runs in its own FreeRTOS task (`feederTask`, created in
  `onEnter`, stopped in `onExit` via the `gActive` flag). It only reads `Serial`
  and pushes decoded items onto a 64-slot queue, then bumps `model.revision`.
- Input is **applied in `render()`**, not `onEvent()` — serial input posts no
  hardware `Event`. On OS2.1 the render task is event-driven and does NOT
  re-render on every `model.revision` bump, so the view also overrides
  `desiredFrameMs()` to return 33 ms (~30 fps) while the screen is open. That
  tick drives `render()`, which drains the queue, applies keys/mouse, blinks the
  caret, and repaints a double-buffered `frameCanvas()` (no flicker). Repaint is
  change-gated so an idle screen only redraws the ~2 Hz caret blink.
  `desiredFrameMs()` returns 0 once the app is left, so there is no battery cost
  elsewhere.
- To add BLE-HID host: add NimBLE-Arduino to `lib_deps`, write a central that
  parses report descriptors, and have it call the same `hid::hidFeed*()`
  functions. Delete the serial feeder task if you want BLE-only. The UI is untouched.

## 5. What works vs what's stubbed

**Implemented and compile-clean:**
- Notes editor: insert, backspace, Enter/Tab, Left/Right/Up/Down/Home/End caret
  moves, word-wrap, caret-follow scrolling, char counter + dirty `*` marker.
- LittleFS load/save of the note (`/notes.txt`), Save + Clear buttons, haptics.
- Mouse cursor overlay + click-to-tap routed through the touch hit-test path.
- Serial-feeder transport + `pc/hid_feeder.py` (keyboard + mouse, auto port detect).
- Auto-sleep is blocked while on the Notes screen (serial input is not counted
  as "activity", so without this the watch would deep-sleep mid-typing). On
  OS2.1 this is a `case Screen::HidCtl: return true;` in
  `src/core/power_mgr.cpp` `screenBlocksSleep()` (see §9).

**Stubbed / deferred (be clear about this):**
- **BLE-HID host is NOT implemented.** No pairing, no report-descriptor parser.
  This is the deferred "real" wireless transport. The serial feeder stands in.
- **USB-host HID is impossible on this board** (hardware, not software) — won't
  be added.
- Text entry is append/edit at a caret; no selection, cut/paste, or undo.
- One note only (`/notes.txt`), capped at 1024 chars.

## 6. NOT verified (I did not flash)

"Compiles" ≠ "works". Nothing below is confirmed on hardware:
- that LittleFS mounts on the `spiffs` partition and persists the note across
  reboots (the partition exists in the partition table; `LittleFS.begin(true)`
  will format it on first use — note this reformats that data partition if it
  held SPIFFS-format data before);
- full-canvas repaint latency/throughput while typing fast or sweeping the mouse;
- that the watch's USB-CDC RX reliably receives the feeder's bytes while also
  emitting log output on TX;
- `pynput` behaviour/permissions on the actual Mac (I syntax-checked the script;
  I could not exercise the listeners);
- that the `desiredFrameMs()` 30 fps tick is enough to keep typing feeling live
  on the real render task under OS2.1's full workload (WiFi/Spotify running).

## 7. Exactly what Ewan must do to verify on hardware

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/hidctl
~/.platformio/penv/bin/pio run -e ewatch -t upload        # flash
# then, in a SEPARATE terminal set up the feeder (do NOT keep pio monitor open
# on the same port — it fights the feeder for the CDC port):
cd pc
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
# grant Terminal: System Settings -> Privacy & Security -> Accessibility AND
# Input Monitoring (see pc/README.md), then reopen Terminal.
```

On the watch:
1. Swipe up from the clock → tap **Notes**. You should see the Notes screen
   (title, empty text area, a blinking caret, and `Clear` / `Save` buttons).
   The watch prints `>> HIDCTL ready` on serial.
2. Run `python3 hid_feeder.py` (it auto-detects `/dev/cu.usbmodem*`).

What to look for:
- **Keyboard:** typing on the Mac makes characters appear in the Notes buffer,
  with the caret advancing and the line wrapping at ~19 chars. Backspace/Enter/
  arrows behave. The `N/1024` counter climbs and shows a `*` when unsaved.
- **Mouse:** moving the mouse moves the on-screen arrow cursor; it clamps at the
  screen edges. A left-click on `Clear` empties the note (buzz); on `Save`
  persists it (buzz); on the top-left `<` leaves to the launcher.
- **Persistence:** type something, tap `Save`, leave and re-open Notes (or
  reboot) → the text is still there.
- **Back:** SW2 short-press or right-swipe leaves the app (saving if dirty).

If the cursor is too fast/slow use `--mouse-scale`; if keystrokes also land in
other Mac apps and that is annoying, use `--exclusive` (keyboard only).

## 8. Risks / things to watch when verifying
- CDC RX starvation under a fast mouse sweep — the feeder already coalesces
  motion to ~60 Hz and clamps per-packet delta to ±64; drop `--mouse-scale` if
  the cursor stutters.
- `LittleFS.begin(true)` formatting the data partition on first mount (above).
- The feeder is a **global** input listener — without `--exclusive`, everything
  typed also hits the focused Mac app.
- OS2.1 keeps WiFi/Spotify/games resident; if typing ever feels laggy, the
  `desiredFrameMs()` value in `src/apps/hidctl.cpp` (currently 33) is the knob.

## 9. Port notes (OS2.1)

What changed moving this app from the old BaseOS build to EWatchOS2.1. No app
logic was altered — only the registration/keep-awake/frame-pacing seams, which
moved in OS2.1:

- **Registration moved from `view.cpp` → `src/apps/system/registry.cpp`.**
  Added: `#include "hidctl.h"`; `static HidCtlView vHidCtl;`;
  `case Screen::HidCtl: return &vHidCtl;` in `viewFor()`; and one launcher row
  `{ "Notes", Icon::Memory, aura::kBlue, Screen::HidCtl, nullptr }` in
  `kTopApps[]`.
- **`Screen::HidCtl`** added to the user-app region of `enum class Screen` in
  `src/core/model.h` (after `Island`).
- **Keep-awake moved from `controller.cpp` `blockSleep` → `power_mgr.cpp`
  `screenBlocksSleep()`.** Added `case Screen::HidCtl: return true;` so the
  screen stays lit while typing (serial input is not touch "activity").
- **Frame pacing:** OS2.1's render task is event-driven and no longer
  re-renders on every `model.revision` bump, so `HidCtlView` now overrides
  `uint16_t desiredFrameMs() const` → returns `33` while `gActive`, else `0`.
  This is what keeps serial input draining and the caret blinking. The old
  BaseOS build relied on the I/O task bumping `revision` continuously; that
  implicit behaviour is gone on OS2.1.
- **WiFi left ON** (`-DEWATCH_ENABLE_WIFI=1`) — the BaseOS size optimisation
  (`=0`) is dropped per the OS2.1 house rule.
- **`main.cpp` untouched** — the feeder task is started lazily and idempotently
  from `HidCtlView::onEnter()` and torn down in `onExit()`, so no main-loop
  wiring was needed.
- **Libraries:** nothing added or vendored; GFX / CST816S / ArduinoJson /
  JPEGDEC are already present in OS2.1's `lib_deps`.
- `pc/` is unchanged and still drives the same serial protocol (§3).

## 2026-10-05 — Serial hardening audit (traps A-E from bookreader)

- **Fixed (trap A, console steals bytes):** `src/apps/hidctl.cpp` — the
  `hidfeed` task read `Serial` while the ewlog console task (`ewcon`) also
  read it, so feeder lines were randomly split and the console answered
  `unknown: ...`. `onEnter()` now calls `ewlogConsolePause(true)` before the
  feeder starts; `onExit()` calls `ewlogConsolePause(false)` after the feeder
  has been told to stop (and waited for). The feeder's read loop is also gated
  on `gActive && ewlogConsolePaused()` so it can never read once Serial is
  handed back to the console.
- **No change (trap B):** the PC feeder sends short lines (key events, mouse
  coalesced to 60 Hz, ~1 KB/s worst case) and the watch drains every 5 ms; it
  never streams anything near the 4 KB RX queue, so no flow control needed.
- **No change (trap C):** the PC never waits on a watch reply (`>> HIDCTL
  ready` is informational only; `hid_feeder.py` never reads the port).
- **No change (trap D):** the only custom font is FreeMono12pt7b (minYOffset
  -15, min xOffset 0); the first text baseline is y=70 at x>=6, so glyphs
  never reach y<0 / x<0. Title/buttons/count use the built-in font.
- **No change (trap E):** only short `ModelLock` holds (revision bump); no
  mutex around slow work.
- Built `pio run -e ewatch`: SUCCESS. Not flashed / not hardware-verified.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-06
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to HidCtl (Notes), tap in the text area stays in-app, 0 errors (8 steps). Snap: `.ewt/boot.png`. The app owns USB serial, so the smoke runs with `raw keepcon 1` (ewlog test hook): the console stays with ewt and the app's PC RX reader is deliberately inert.
- NOT exercised: the live PC-data path (pc/ companion over USB serial). ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
