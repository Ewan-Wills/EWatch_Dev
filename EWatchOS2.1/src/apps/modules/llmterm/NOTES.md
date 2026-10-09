# llmterm module notes

Carried over from the stand-alone `llmterm/` folder it was developed in
(before 2026-10-09). OS-wide parts of these notes may be out of date.

---

# llmterm — build notes & hardware-verify guide (EWatchOS2.1)

The EWatch as a terminal for a Mac-hosted local LLM. The watch picks a prompt,
sends it over USB serial, and renders the streamed answer. **The watch runs no
model and does no on-device inference** — the Mac does all of it over USB serial.

> **Ported to EWatchOS2.1.** This folder was re-scaffolded from the current
> shipping OS (watch faces, launcher, settings, WiFi, Spotify, games, …) and the
> `LlmTermView` re-registered into it. **WiFi is left ON** (OS2.1 default; Spotify
> / Photos / WiFi-settings depend on it) — unlike the old BaseOS build, llmterm no
> longer gates WiFi off. The watch still does no networking of its own; it only
> talks to the Mac bridge over USB-CDC.

## What it does (MVP)

- New launcher app **"LLM Term"** (swipe up from the watch face → in the app grid,
  icon = Memory, blue).
- **Menu**: a list of canned prompts drawn as tappable pills. (On-watch free-text
  entry is deliberately omitted for MVP — fiddly on a 240×280 touch panel.)
- Tap a prompt → sends `PROMPT <text>\n` over USB serial, switches to the **chat**
  view.
- **Chat**: a "...thinking" hint until the first token, then streamed tokens
  rendered in a fixed-pitch terminal grid (built-in 6×8 font ×2 → ~19 cols × 12
  rows), word-wrapped, scrolling as it fills, with a blinking block caret. An
  `end`/`err` marker appears top-right when the stream finishes.
- **Back** (SW2 button or right-swipe → both arrive as `ButtonShort`, plus the
  top-left chevron) returns chat → menu, or menu → launcher.

## Serial protocol (USB CDC, 115200; native USB ignores baud)

```
watch -> PC :  PROMPT <text>\n
PC -> watch :  >>TOK <chunk>\n      one streamed chunk (zero or more)
               >>END\n              stream finished
               >>ERR <message>\n    backend unreachable / failed (shown inline)
```

- Watch→PC uses a bare `PROMPT ` line; PC→watch telemetry is `>>`-prefixed so the
  watch ignores boot-log noise and any unrecognised `>>` line (e.g. `>>READY`).
- **Token escaping:** so one token maps to exactly one line, the bridge escapes
  `\` → `\\` and newline → `\n` (two literal chars). The watch un-escapes on
  receipt; `\n` inside a token becomes a hard line break. `\t` is rendered as a
  space (tabs don't map onto the cell grid).
- On entering a prompt the watch flushes stale RX bytes first, so a previous
  run's tail can't leak into the new answer. The view also flushes once on first
  `onEnter()` to drop boot-log noise.

## How to run the PC side

`pc/` is unchanged by the OS2.1 port. See `pc/README.md` for the full walkthrough.
Short version:

```sh
# 1. a model runner on the Mac
brew install ollama && ollama serve          # or the ollama.com app
ollama pull llama3.2:1b                       # ~1 GB; qwen2.5:0.5b is smaller

# 2. python deps
cd EWatch_Firmware/ShortsApps/llmterm/pc
pip install -r requirements.txt

# 3. the bridge (close any pio monitor first — one owner per port)
python3 llmterm_bridge.py                     # auto port, Ollama, llama3.2:1b
```

Backend order is Ollama (`http://localhost:11434/api/generate`, streaming NDJSON)
then a llama.cpp server (`http://localhost:8080/completion`, streaming SSE)
fallback. Force one with `--backend ollama|llamacpp`; choose the model with
`--model`; override the port with `--port`.

## Build

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd EWatch_Firmware/ShortsApps/llmterm
~/.platformio/penv/bin/pio run -e ewatch
```

**Status: compiles clean on OS2.1.** Final summary line from the last build:

```
RAM:   62.6% (used 204980 bytes from 327680 bytes)
Flash: 54.4% (used 1710801 bytes from 3145728 bytes)
ewatch  SUCCESS  00:00:31
```

(Bigger than the old BaseOS figures because this is the full OS2.1 image with
WiFi/Spotify/games present, plus llmterm — a complete standalone flash.)

## What's wired into OS2.1 (the port)

Registration moved from BaseOS's `view.cpp` / `controller.cpp` to the OS2.1
registry + power manager. Changes made, all inside this app folder:

- `src/apps/llmterm.{h,cpp}` — the `LlmTermView` (the app view; carried over).
- `src/core/model.h` — added `LlmTerm,` to `enum class Screen` (after `Island`,
  outside the Spotify `#if` so it's always present).
- `src/apps/system/registry.cpp` — `#include "llmterm.h"`, `static LlmTermView
  vLlmTerm;`, a `case Screen::LlmTerm: return &vLlmTerm;` in `viewFor()`, and one
  launcher tile `{ "LLM Term", Icon::Memory, aura::kBlue, Screen::LlmTerm, nullptr }`
  in `kTopApps[]`.
- `src/core/power_mgr.cpp` — added `case Screen::LlmTerm:` to `screenBlocksSleep()`
  so the screen stays lit while a prompt streams in (tokens arrive over USB with
  no touch activity to reset the idle timer). This replaces BaseOS's
  `controller.cpp` `blockSleep` OR-chain.
- `platformio.ini` — **unchanged**; WiFi stays on (`-DEWATCH_ENABLE_WIFI=1`), per
  the OS2.1 port rule. No app-specific flags needed.
- `src/main.cpp` — **unchanged**. `Serial.begin(115200)` already runs at boot, so
  there is no background task to wire in.

### Frame pacing / serial model (the OS2.1-specific change)

OS2.1's render task only re-renders on events + a 1 Hz heartbeat unless the view
asks for frames via `desiredFrameMs()`. BaseOS llmterm drove its streaming/caret
redraw by bumping `model.revision++` every render; that produces no smooth motion
on OS2.1. The port replaces every `model.revision++` with:

```cpp
uint16_t desiredFrameMs() const override { return streaming ? 33 : 0; }
```

So while a stream is in flight the view is called ~30 fps (tokens flow, caret
blinks), and it drops back to fully event-driven once `>>END`/`>>ERR` clears
`streaming`. `render()` pumps the serial port each frame; `onEvent(Tick)` pumps
once a second as a safety net; back/tap transitions rely on the event itself
triggering a render (confirmed in `controller.cpp`: `dirty = any || …`). The
serial pump is started lazily and idempotently from `onEnter()` (a one-time RX
flush — the CDC link itself is already up from `main.cpp`), not wired into
`main.cpp`. All serial I/O stays on the render task; `taskIO` (I2C owner) is
untouched.

## What's stubbed / simplified for MVP

- **Input is canned prompts only.** No on-screen keyboard. The prompt list is a
  `kPrompts[]` array in `llmterm.cpp` — edit it to taste. Adding a keyboard later
  only needs a new menu mode that builds a `String` and calls `sendPrompt()`.
- **Response buffer is capped at 4000 bytes** (`kRespCap`); longer answers stop
  appending (older text still scrolls off-screen as normal). Raise if needed.
- **No markdown / ANSI handling** — tokens render as plain text; tabs become
  spaces.
- **No cancel-mid-stream signal to the PC** — pressing Back just stops the watch
  reading and returns to the menu; the bridge finishes that generation into its
  (now-drained) buffer before it's ready for the next prompt.
- **llama.cpp fallback** targets the classic `/completion` SSE shape; a very new
  or very old llama.cpp server with a different schema may need a tweak.

## Risks / things to watch on hardware

- **Serial contention:** the USB port is also the flash/monitor channel. If a
  `pio ... -t monitor` (or any other serial app) is open, the bridge can't open
  the port. Close it first.
- **Throughput/WDT:** per-render serial drain is bounded (1024 bytes/pass) and
  the full-screen canvas redraw is the same cost screenshare runs at, so the
  task watchdog has margin — but this is unverified on silicon.
- **PSRAM:** chat uses the shared 240×280 frame canvas (~134 KiB PSRAM). If PSRAM
  is exhausted `frameCanvas()` returns null and the view falls back to drawing
  straight to the panel (slightly more flicker, still functional). Note OS2.1's
  baseline RAM/PSRAM use is higher than BaseOS (WiFi stack etc.).
- **Token boundaries:** a chunk that is *only* whitespace at a wrap boundary may
  be collapsed by the word-wrapper — cosmetic only.

## EXACT hardware-verify steps (for Ewan)

1. Build + flash + monitor (confirm boot), then **close the monitor**:
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd EWatch_Firmware/ShortsApps/llmterm
   ~/.platformio/penv/bin/pio run -e ewatch -t upload -t monitor
   # watch the "=== EWatch boot ===" line, then Ctrl-C to release the port
   ```
2. Start a model + the bridge:
   ```sh
   ollama serve &            # if not already running
   ollama pull llama3.2:1b   # once
   cd pc && pip install -r requirements.txt
   python3 llmterm_bridge.py
   # expect: "[llmterm] backend=ollama model=llama3.2:1b port=/dev/cu.usbmodem…"
   ```
3. On the watch: swipe up → **LLM Term**. Confirm the prompt menu appears.
4. Tap **"Tell me a short joke"**. **Look for:**
   - the view switches to chat with a "...thinking" hint,
   - within a second or two, tokens stream in word-wrapped with a blinking caret,
   - the bridge terminal prints `PROMPT: 'Tell me a short joke'` then
     `streamed N chunk(s); END`,
   - an `end` marker appears top-right when complete.
5. Press the side button (or right-swipe) → returns to the prompt menu. Try
   another prompt.
6. **Error path:** quit the bridge (Ctrl-C), tap a prompt on the watch — it
   should sit on "...thinking" with no tokens. Restart the bridge, or to see an
   explicit error, stop Ollama (`pkill ollama`) with the bridge running and tap a
   prompt → the watch shows `[error] cannot reach ollama backend` and an `err`
   marker.

Nothing here is verified on hardware by me — only that it compiles clean on
OS2.1. Treat compile-clean as the delivered bar; acceptance is Ewan flashing +
filming.

## 2026-10-05 — serial hardening (traps from the bookreader fix)

Reference: ShortsApps/bookreader (verified on hardware there, not here).

- **Trap A (console steals RX) — fixed.** `LlmTermView::onEnter()` now calls
  `ewlogConsolePause(true)` and `onExit()` calls `ewlogConsolePause(false)`.
  Before, the ewcon console task read Serial concurrently with `pumpSerial()`,
  so chunks of `>>TOK`/`>>END` lines could vanish into the console ("unknown:
  ... (try help)") and a stream could hang on "...thinking" or never see END.
- **Trap C (short TX stuck in HWCDC) — fixed.** `Serial.flush()` after the
  `PROMPT <text>\n` line in `sendPrompt()`; the bridge blocks waiting on it.
- **Trap B — not changed.** The PC streams at LLM generation speed (one short
  `>>TOK` line per token) and the watch drains up to 1 KB per 33 ms frame while
  streaming; no bulk >4 KB burst, so no ACK protocol added to
  `pc/llmterm_bridge.py`. Bytes dropped after leaving chat mid-stream are a
  stale tail anyway (sendPrompt flushes RX).
- **Trap D — none.** All app text uses the built-in font (`setFont(nullptr)`)
  at fixed y >= 5; the scrolling body is a ring of lines at y >= 28.
- **Trap E — none.** The app takes no mutex.

Compile-clean only (`pio run -e ewatch`); not flashed.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-07
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to LlmTerm, tap on a preset prompt sends it (screen shows 'Why is the sky blue?' + '...thinking', waiting for the PC bridge), stays in-app, 0 errors (9 steps). Snap: `.ewt/boot.png`. The app owns USB serial, so the smoke runs with `raw keepcon 1` (ewlog test hook): the console stays with ewt and the app's PC RX reader is deliberately inert.
- NOT exercised: the live PC-data path (pc/ companion over USB serial). ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
- Harness notes: the app's own serial output (`>>STAT`, `>>HELLO`) can interleave with a snapshot stream and corrupt it; the first ewt command after the app is already open sometimes gets no reply (second run passes).
