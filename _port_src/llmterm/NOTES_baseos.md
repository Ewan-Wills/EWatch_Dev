# llmterm — build notes & hardware-verify guide

The EWatch as a terminal for a Mac-hosted local LLM. The watch picks a prompt,
sends it over USB serial, and renders the streamed answer. **The watch runs no
model and does no networking** — WiFi is gated off; the Mac does all inference.

## What it does (MVP)

- New launcher app **"LLM Terminal"** (swipe up from the watch face → first tile).
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
  run's tail can't leak into the new answer.

## How to run the PC side

See `pc/README.md` for the full walkthrough. Short version:

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

**Status: compiles clean.** Final summary line from the last build:

```
RAM:   8.4% (used 27508 bytes from 327680 bytes)
Flash: 14.6% (used 459233 bytes from 3145728 bytes)
ewatch  SUCCESS  00:00:17
```

## What's wired into BaseOS

- `src/apps/llmterm.{h,cpp}` — the `LlmTermView`.
- `src/core/model.h` — `Screen::LlmTerm` enumerator.
- `src/apps/system/view.cpp` — `#include "llmterm.h"`, `static LlmTermView vLlmTerm`,
  `viewFor()` case, and the `kTopApps[]` launcher tile.
- `src/core/controller.cpp` — `Screen::LlmTerm` added to the `blockSleep` OR-chain
  so the screen stays lit while a prompt streams in (tokens arrive over USB with
  no touch activity to reset the idle timer).
- `platformio.ini` — `-DEWATCH_ENABLE_WIFI=0` (no networking; drops WiFi/WebServer/
  DNSServer and the WiFi settings pages).

### Render/serial model (why it works within the task model)

The render task calls `render()` when the model is dirty or at least once a
second. While a stream is in flight the chat `render()` pumps the serial port,
then bumps `model.revision` so the loop immediately re-renders (~20 Hz, bounded
by the 50 ms event-queue wait). It stops bumping on `>>END`/`>>ERR`, so the view
idles afterwards. `onEvent(Tick)` also pumps serial once a second as a safety
net. All serial I/O happens on the render task (the view), never on `taskIO`, so
the I2C-owning task is untouched.

## What's stubbed / simplified for MVP

- **Input is canned prompts only.** No on-screen keyboard. The prompt list is a
  `kPrompts[]` array in `llmterm.cpp` — edit it to taste. Adding a keyboard later
  only needs a new menu mode that builds a `String` and calls `sendPrompt()`.
- **Response buffer is capped at 4000 bytes** (`kRespCap`); longer answers stop
  appending (older text still scrolls off-screen as normal). Fine for the demo;
  raise the cap if needed (watch has 2 MB PSRAM).
- **No markdown / ANSI handling** — tokens render as plain text; tabs become
  spaces.
- **No cancel-mid-stream signal to the PC** — pressing Back just stops the watch
  reading and returns to the menu; the bridge finishes that generation into its
  (now-drained) buffer before it's ready for the next prompt. A `CANCEL` line
  could be added later.
- **llama.cpp fallback** targets the classic `/completion` SSE shape; a very new
  or very old llama.cpp server with a different schema may need a tweak.

## Risks / things to watch on hardware

- **Serial contention:** the USB port is also the flash/monitor channel. If a
  `pio ... -t monitor` (or any other serial app) is open, the bridge can't open
  the port. Close it first.
- **Throughput/WDT:** per-render serial drain is bounded (1024 bytes/pass) and
  the full-screen canvas redraw is the same cost screenshare runs at, so the
  20 s task watchdog has plenty of margin — but this is unverified on silicon.
- **PSRAM:** chat uses the shared 240×280 frame canvas (~134 KiB PSRAM). If PSRAM
  is exhausted `frameCanvas()` returns null and the view falls back to drawing
  straight to the panel (slightly more flicker, still functional).
- **Token boundaries:** a model chunk containing a literal backslash is escaped
  correctly, but a chunk that is *only* whitespace at a wrap boundary may be
  collapsed by the word-wrapper — cosmetic only.

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
3. On the watch: swipe up → **LLM Terminal**. Confirm the prompt menu appears.
4. Tap **"Tell me a short joke"**. **Look for:**
   - the view switches to chat with a "...thinking" hint,
   - within a second or two, tokens stream in word-wrapped with a blinking caret,
   - the bridge terminal prints `PROMPT: 'Tell me a short joke'` then
     `streamed N chunk(s); END`,
   - an `end` marker appears top-right when complete.
5. Press the side button (or right-swipe) → returns to the prompt menu. Try
   another prompt.
6. **Error path:** quit the bridge (Ctrl-C), tap a prompt on the watch — it
   should sit on "...thinking" with no tokens (no PC answering). Restart the
   bridge, or to see an explicit error, stop Ollama (`pkill ollama`) with the
   bridge running and tap a prompt → the watch shows `[error] cannot reach ollama
   backend` and an `err` marker.

Nothing here is verified on hardware by me — only that it compiles clean. Treat
compile-clean as the delivered bar; acceptance is Ewan flashing + filming.
