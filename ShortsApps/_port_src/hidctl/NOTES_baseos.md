# hidctl — control the watch with a keyboard / mouse

**Status: compiles clean (not flashed, not hardware-verified). Honest MVP.**
Final build: `env:ewatch` SUCCESS, Flash 16.9% (530 KB), RAM 7.5%.

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
(registered in `src/apps/system/view.cpp` + `Screen::HidCtl` in `model.h`).

WiFi is gated off (`-DEWATCH_ENABLE_WIFI=0`) — this app doesn't network. That
dropped flash from 32.9% → 16.9%.

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
  hardware `Event`, it only dirties the model, so `render()` (which the I/O task
  drives continuously) drains the queue and applies keys/mouse, then repaints a
  double-buffered `frameCanvas()` (no flicker). Repaint is change-gated so an
  idle screen only redraws the ~2 Hz caret blink.
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
  as "activity", so without this the watch would deep-sleep mid-typing — fixed
  in `src/core/controller.cpp` `blockSleep` chain).

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
  reboots (the partition exists in `huge_app.csv`; `LittleFS.begin(true)` will
  format it on first use — note this reformats that data partition if it held
  SPIFFS-format data before);
- full-canvas repaint latency/throughput while typing fast or sweeping the mouse;
- that the watch's USB-CDC RX reliably receives the feeder's bytes while also
  emitting log output on TX;
- `pynput` behaviour/permissions on the actual Mac (I syntax-checked the script;
  I could not exercise the listeners).

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
