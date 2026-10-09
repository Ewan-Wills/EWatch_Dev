# EWatch ShortsApps — per-app specs (MVP)

Batch of standalone EWatch apps for short-form content. Each is its own BaseOS
copy under `ShortsApps/<app>/` + (where noted) a `pc/` companion. Read
`_PRIMER.md` first. MVP bar = compiles clean + honestly-complete code + NOTES.md
telling Ewan how to verify on hardware. **Only Ewan flashes & accepts.**

Build everything with the isolated core:
`PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps ~/.platformio/penv/bin/pio run`

---

## 1. screenshare  — mirror a Mac window to the watch over USB
- **Hero demo:** a chosen Mac window streams live onto the watch face over USB.
- **PC (`pc/`):** Python. Capture a window/region (`mss` + `Pillow`), downscale to
  240×280, JPEG-encode (quality ~40–60), send framed over serial. ~10–15 fps target.
  Flags: `--port`, `--fps`, `--region`/`--window`, `--quality`.
- **Watch:** read framed JPEG (magic + uint32 LE len + payload), decode with
  **JPEGDEC** (already a dep) straight into `frameCanvas()`, flush. Show a
  "waiting for PC…" state until first frame. Back = ButtonShort.
- **Protocol:** binary frames over USB CDC. Document it.
- **Gate off:** WiFi (`-DEWATCH_ENABLE_WIFI=0`).
- **Risks:** CDC throughput vs fps; keep per-frame decode < WDT. Tune fps down if needed.

## 2. hidctl — control the watch with a keyboard / mouse
- **Hero demo:** type into an on-watch Notes screen from a real keyboard; move a
  cursor over the UI with a mouse.
- **Feasibility FIRST:** USB-host (peripheral plugged into the watch) needs the S3
  to be USB host AND source 5 V VBUS — the port is the charge/flash *device* port,
  so this is almost certainly not wired. Spend ≤30 min checking; if not feasible,
  **pivot to BLE HID host** (S3 is BLE-capable): watch pairs a BLE keyboard/mouse.
  Document which path you took and why.
- **Watch:** a `Notes` view (text buffer, caret, drawn with a mono font; persist to
  LittleFS via `storage`) that consumes key events; a cursor overlay driven by
  mouse deltas that can tap UI. For BLE HID host on arduino-esp32 2.0.17, use the
  NimBLE HID-central approach; if the lib story is too heavy for MVP, implement
  the Notes UI + a clean `onKey()/onMouse()` seam and stub the transport with a
  serial fallback so it compiles and demos over USB from a tiny `pc/` feeder.
- **No PC script required** if BLE works; include the serial feeder only as fallback.
- **Risks:** biggest-unknown app. Be explicit in NOTES about exactly what works.

## 3. bookreader — read plain-text books on the watch  ★ wants to be finished
- **Hero demo:** upload a .txt over serial, it's saved to flash, read it with
  smooth scrolling. Ewan actually wants to use this one — make it solid, not a toy.
- **PC (`pc/`):** Python uploader — send a .txt (chunked, framed) to the watch;
  the watch writes it to LittleFS. Flags `--port`, `<file>`. List/delete optional.
- **Watch:** library list of stored books (LittleFS); open one → paginated/
  word-wrapped text with **smooth pixel scrolling** (animate via `frameCanvas()`
  offset; swipe up/down or auto-scroll with speed control). Remember last position
  per book (persist offset). Good typography (FreeSerif/Sans). Font size setting.
- **Storage:** LittleFS; wrap text to the 240px width; don't load whole book into
  RAM if large — page from flash.
- **Gate off:** WiFi.
- **Risks:** LittleFS space (books are small, fine); smooth scroll without flicker
  (redraw discipline / canvas).

## 4. stocks — live stock tracker, swipe through tickers
- **Hero demo:** set tickers in the PC app; swipe through them on the watch, each
  showing price, day %, and a sparkline.
- **Data source:** use **Stooq** (`https://stooq.com/q/l/?s=<sym>&f=sd2t2ohlcv&e=csv`)
  — free, no API key — for quotes, and Stooq daily CSV for the sparkline history.
  (If a US symbol needs a suffix, handle it.) No keys anywhere.
- **PC (`pc/`):** Python. Config file / CLI list of tickers; poll every N sec;
  push each ticker's {symbol, price, changePct, sparkline points} framed over
  serial. Flags `--port`, `--tickers AAPL,MSFT,…`, `--interval`.
- **Watch:** swipe left/right (or up/down) through received tickers; draw name,
  big price, colored +/-%, and a sparkline (polyline in `frameCanvas()`).
- **Gate off:** WiFi (data comes via PC).
- **Risks:** Stooq rate limits / symbol formats — document.

## 5. homeassistant — toggle a light (LOW PRIORITY)
- **Hero demo:** a button on the watch toggles a Home Assistant light; shows state.
- **Watch:** WiFi client (keep `-DEWATCH_ENABLE_WIFI=1`). Call HA REST API
  (`POST /api/services/light/turn_on|turn_off`, `GET /api/states/<entity>`), Bearer
  token. Read HA base URL + token + entity_id from a git-ignored
  `secrets/ha.local.h` (provide `ha.example.h`); do NOT hardcode secrets. One light
  entity for MVP; a toggle button + state dot. (Ewan's HA is on his home Pi with a
  lamp + alarm clock.)
- **No PC script.**
- **Risks:** needs Ewan's HA URL/token/entity at flash time — NOTES must say so.

## 6. printstream — Creality K2 Plus: live camera + basic control
- **Hero demo:** watch shows the printer's live camera and current job, with
  pause/resume/home controls.
- **Targets on LAN:** K2Plus-5821 `192.168.0.214`, K2Plus-248A `192.168.0.244`
  (Moonraker REST confirmed at `http://<ip>:7125`). A Creality web UI also at
  `192.168.0.170`.
- **Camera:** Moonraker `/server/webcams/list` is empty (Creality uses its own cam
  daemon). **Discover the MJPEG endpoint** while a print+camera is live — probe
  `:8080/?action=stream`, `:8000`, `:4408`, and the Creality cam path; record the
  working URL. If no MJPEG is reachable, fall back to periodic snapshot or
  job-status-only and say so.
- **Watch:** WiFi client. Poll Moonraker (`/printer/objects/query?print_stats&...`)
  for state/progress/temps; show job name, %, temps. Decode MJPEG frames (JPEGDEC)
  if available. Buttons: Pause (`/printer/print/pause`), Resume (`/printer/print/resume`),
  Home (`/printer/gcode/script?script=G28`). Confirm-before-send on control actions.
  Printer IP configurable (`secrets/printer.example.h` → `.local.h`).
- **No PC script.**
- **Risks:** camera endpoint unknown until discovered; MJPEG bandwidth on the S3.
  Control actions change real hardware — gate behind an on-screen confirm.

## 7. macropad — touchscreen macro keyboard over serial
- **Hero demo:** tap a grid of buttons on the watch (Copy, Paste, a shortcut, launch
  an app); the Mac performs the action.
- **PC (`pc/`):** Python listener maps incoming button IDs → actions
  (`pyautogui`/`keyboard`: cmd+c, cmd+v, a hotkey, open an app). Config maps id→macro.
  Flags `--port`. Document macOS Accessibility permission requirement.
- **Watch:** a 2×3 or 3×3 grid of labeled tiles; tap → haptic + send `MACRO <id>\n`.
  Labels configurable in a header. Back = ButtonShort.
- **Gate off:** WiFi.
- **Risks:** macOS will prompt for Accessibility/Input Monitoring for pyautogui —
  put that in the PC README.

## 8. llmterm — watch as a terminal for a Mac-hosted LLM
- **Hero demo:** type/pick a prompt on the watch; a local model on the Mac answers;
  tokens stream onto the watch.
- **PC (`pc/`):** Python bridge to a local model. Prefer **Ollama**
  (`http://localhost:11434/api/generate`, stream) with a `--model` flag
  (default e.g. `llama3.2:1b`); fall back to llama.cpp server if present. Read
  prompt lines from serial (`PROMPT <text>\n`), stream tokens back as
  `>>TOK <chunk>\n` then `>>END\n`. Flags `--port`, `--model`.
- **Watch:** pick from a few canned prompts (MVP) or a simple on-screen input;
  send prompt; render streaming tokens with word-wrap + a blinking caret; scroll as
  it fills. Back = ButtonShort.
- **Gate off:** WiFi.
- **Risks:** on-watch text entry is fiddly — canned prompts + optional on-screen
  keyboard is fine for MVP. Token render must respect redraw discipline.

## 9. flypet — virtual-fly tamagotchi
- **Hero demo:** an animated fly lives on the watch as a pet; its behaviour is
  driven by a fly "brain" simulation on the Mac (nod to the Drosophila connectome
  work). MVP: a believable animated fly with state (hunger/energy/mood) + simple
  behaviour; the Mac sim *drives* it. Running the real connectome on-watch is
  impossible — the Mac does any heavy sim.
- **PC (`pc/`):** Python. A simple behaviour/state model (optionally seeded by
  published fly-connectome behaviour patterns — walk/groom/rest/feed) that emits
  commands over serial (`FLY state=<..> action=<walk|groom|rest|feed> x=.. y=..\n`).
  Keep it self-contained; no giant datasets. Flags `--port`.
- **Watch:** reference the existing `tamagotchi` app (`EWatch_Firmware/tamagotchi`)
  for sprite/animation patterns. Draw an animated fly (sprite frames or simple
  vector) that walks/grooms/rests per incoming commands; show pet stats; tap to
  interact (feed) → send event to PC. Should also run standalone (autonomous
  wander) if no PC connected.
- **Gate off:** WiFi.
- **Risks:** keep animation smooth (canvas, delta redraws). Don't overclaim the
  "real brain" — NOTES must state it's behaviour-inspired, Mac-side.

---

### Notes for the coordinator
- `homeassistant`, `printstream` keep WiFi; the rest gate it off.
- `printstream` + `homeassistant` need Ewan's secrets/URLs at flash time.
- Nothing is "done" until Ewan flashes and films it; agents deliver compile-clean MVPs.
