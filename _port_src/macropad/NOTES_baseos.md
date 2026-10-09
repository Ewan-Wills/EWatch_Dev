# Macropad — build notes

A touchscreen macro keyboard. The watch draws a labelled grid of tiles; a tap
buzzes the haptic and sends `MACRO <id>\n` over USB serial. A Mac companion
(`pc/macropad_listener.py`) maps each id to a real action (cmd+c, launch an
app, a hotkey...). The Mac performs the action — the watch only sends ids.

## What it does

- App `Macropad` in the launcher (swipe up from the watch face -> first tile).
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

## Config gates

- **WiFi gated OFF**: `-DEWATCH_ENABLE_WIFI=0` in `platformio.ini` (no
  networking; drops WiFi/WebServer/DNSServer + their settings pages).

## Build

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd ShortsApps/macropad
~/.platformio/penv/bin/pio run -e ewatch
```

Last build: **SUCCESS** — RAM 7.0% (22996 B), Flash 14.5% (457257 B of 3 MB).

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

- **Not flashed or run by me** — only compilation is verified. Tap hit-testing,
  haptic feel, and serial output must be confirmed on the watch.
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
2. On the watch: swipe up from the watch face -> tap the **Macropad** tile. You
   should see a 2x3 grid: Copy, Paste, Undo, Redo, Mission, Spotlight.
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
