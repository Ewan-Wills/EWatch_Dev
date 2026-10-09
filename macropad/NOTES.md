# Macropad — build notes (EWatchOS2.1)

A touchscreen macro keyboard. The watch draws a labelled grid of tiles; a tap
buzzes the haptic and sends `MACRO <id>\n` over USB serial. A Mac companion
(`pc/macropad_listener.py`) maps each id to a real action (cmd+c, launch an
app, a hotkey...). The Mac performs the action — the watch only sends ids.

**This app is now ported onto the full EWatchOS2.1** (watch faces, launcher,
settings, WiFi, Spotify, games, etc.), not the old stripped BaseOS. WiFi stays
ON — the OS is intact and the app is simply added to it.

## What it does

- App `Macropad` in the launcher under a **TOOLS** section (swipe up from the
  watch face).
- Draws a **2x3** grid (6 tiles) of labelled buttons. Grid size and labels are
  compile-time config in `src/apps/macropad_config.h` (set `MACROPAD_COLS` /
  `MACROPAD_ROWS`, e.g. 3x3, and list one label per tile).
- Tap a tile: fills it with the accent colour (visual feedback), `hapticBuzz`,
  and writes `MACRO <id>\n` to `Serial`. Release restores the tile.
- Back: hardware button (short press) or a right-swipe (both arrive as
  `ButtonShort`), or the top-left back chevron -> returns to the launcher.
- Theme-aware: a Settings -> Display colour change repaints the grid.

## Serial protocol

- USB CDC, 115200 (native USB ignores baud). Newline-terminated ASCII.
- **Watch -> PC:** `MACRO <id>\n` on each tap. `id` is the 0-based tile index,
  row-major (left-to-right, top-to-bottom). Ids map to `kMacroLabels` order.
- The PC side ignores any line not starting with `MACRO ` (so boot banners and
  firmware logs are harmless).
- No PC -> watch messages in this MVP.

## PC side — how to run

See `pc/README.md`. Short version:

```sh
cd pc
pip install -r requirements.txt
python3 macropad_listener.py            # auto-detects /dev/cu.usbmodem*
python3 macropad_listener.py --dry-run  # verify taps without firing actions
```

**macOS permission:** `pyautogui` keystrokes need the terminal app granted
**Accessibility** (and sometimes **Input Monitoring**) in System Settings ->
Privacy & Security. `app`/`shell` actions don't. Details in `pc/README.md`.
Close any `pio ... -t monitor` / serial monitor first — one owner of the port.

Edit `pc/macros.json` to remap ids -> actions. Keep its keys in the same order
as `kMacroLabels`.

## Port — what changed for OS2.1

The view code (`src/apps/macropad.*`, `src/apps/macropad_config.h`) is unchanged
— the BaseOS APIs it used (`View`, `drawTitleBar`, `tappedBack`, `switchTo`,
`theme()`/`ThemeColors`, `contrastFor`, `hapticBuzz`, `frameCanvas`,
`EventType::Touch/TouchUp/ButtonShort`) all still exist and behave the same on
OS2.1. Registration moved, so the port is three small edits plus the config
change:

- `src/core/model.h`: added `Macropad,` to `enum class Screen` (right after
  `Island,`).
- `src/apps/system/registry.cpp`: `#include "macropad.h"`; `static MacropadView
  vMacropad;`; `case Screen::Macropad: return &vMacropad;` in `viewFor()`; and
  one launcher row
  `{ "Macropad", Icon::Memory, aura::kPurple, Screen::Macropad, "TOOLS" }` in
  `kTopApps[]`. (Registration used to live in `view.cpp` on BaseOS.)
- **WiFi:** kept ON (`-DEWATCH_ENABLE_WIFI=1` in `platformio.ini`). On BaseOS
  this app gated WiFi off; OS2.1's launcher/Spotify/WiFi-settings depend on it,
  so it stays on. Macropad itself does no networking.
- **No keep-awake added.** Macropad is tap-driven, and every tap counts as
  activity that resets the sleep timer, so `power_mgr.cpp`
  (`screenBlocksSleep`) was left untouched — the grid will dim/sleep on its own
  idle timeout like any other normal screen, which is correct here.
- **No `desiredFrameMs()` override.** The grid is static between taps; press
  feedback is drawn immediately from `onEvent()`. OS2.1's event-driven render is
  exactly what this needs, so no continuous-animation frame request is required.

Last build: **SUCCESS** — RAM 61.2% (200468 B), Flash 54.3% (1708981 B of 3 MB).
(The larger footprint vs the old BaseOS build is the full OS2.1 now linked in.)

## What's MVP / stubbed

- Labels and grid dimensions are **compile-time** (header), not editable on the
  watch — change the header and reflash to re-label. This matches the spec
  ("labels configurable in a header").
- No on-watch confirmation of PC receipt (one-way fire-and-forget). The watch
  can't tell whether the listener is running; the buzz/flash is local feedback
  only.
- `macros.json` ships with sensible macOS defaults (Copy/Paste/Undo/Redo/
  Mission Control/Spotlight); remap freely.

## Risks / things only hardware can confirm

- **Not flashed or run by me — only compilation is verified.** Tap
  hit-testing, haptic feel, and serial output must be confirmed on the watch.
- macOS will prompt for Accessibility/Input Monitoring the first time a
  keystroke action fires; until granted, `hotkey`/`keys`/`type` silently do
  nothing (documented in the README).
- The USB port is shared with flashing/monitoring; the listener and a serial
  monitor can't both hold it at once.

## Hardware-verify steps (for Ewan)

1. Flash:
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd ShortsApps/macropad
   ~/.platformio/penv/bin/pio run -e ewatch -t upload
   ```
2. On the watch: swipe up from the watch face -> find the **Macropad** tile
   (TOOLS section) -> tap it. You should see a 2x3 grid: Copy, Paste, Undo,
   Redo, Mission, Spotlight.
3. **Verify serial output first (no permissions needed):**
   `~/.platformio/penv/bin/pio device monitor -b 115200` (or `screen`), tap
   tiles, confirm each prints `MACRO 0`..`MACRO 5` matching the tapped tile.
   Confirm each tap buzzes and the tile flashes the accent colour. Then **quit
   the monitor** (it holds the port).
4. **Run the companion:**
   ```sh
   cd pc && pip install -r requirements.txt
   python3 macropad_listener.py --dry-run     # confirm taps map correctly
   ```
   Then grant Terminal **Accessibility** (System Settings -> Privacy &
   Security), reopen Terminal, and run without `--dry-run`. Put the cursor in a
   text field, select some text, tap **Copy** then **Paste** on the watch and
   confirm the Mac copies/pastes; tap **Spotlight** and confirm it opens.
5. Back out with the side button (short press) or a right-swipe; confirm you
   land back on the launcher.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-06
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to Macropad (6-key grid), tap on Copy stays in-app, 0 errors (7 steps). Snap: `.ewt/boot.png`.
- NOT exercised: the live PC/HID key path. ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
