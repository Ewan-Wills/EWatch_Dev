# printstream — NOTES (EWatchOS2.1)

EWatch app: live camera + print state + basic control for a Creality K2 Plus
(Klipper/Moonraker). WiFi client; no PC companion. Only Ewan flashes & accepts —
this document is the hand-off. **Compiles clean on EWatchOS2.1; not
hardware-verified.**

Final build (isolated core, full OS2.1 + this app):
```
RAM:   66.7% (218,708 / 327,680 bytes)
Flash: 54.7% (1,719,413 / 3,145,728 bytes)
ewatch  SUCCESS
```
(Bigger than the old BaseOS figures because this is the full shipping OS2.1 —
watch faces, launcher, settings, WiFi, Spotify, games — with printstream added,
not a stripped BaseOS copy.)

---

## 0. Port to OS2.1 — what changed from the BaseOS version

The app logic (`printstream.cpp` / `printstream_view.cpp`) is unchanged in
behaviour; only the OS integration points moved to the OS2.1 locations:

- **Registration** is now in `src/apps/system/registry.cpp`, not `view.cpp`:
  `#include "printstream.h"`, `static PrintStreamView vPrintStream;`, a
  `case Screen::PrintStream: return &vPrintStream;` in `viewFor()`, and one
  launcher row `{ "Printer", Icon::Cube, aura::kOrange, Screen::PrintStream, nullptr }`
  in `kTopApps[]`. `Screen::PrintStream` was added to the user-app region of the
  `enum class Screen` in `src/core/model.h`.
- **Keep-awake** moved to `src/core/power_mgr.cpp` → `screenBlocksSleep()`:
  `case Screen::PrintStream: return true;` keeps the display lit on both the
  stats and camera pages (it is one Screen with two pages). This replaces the
  old `controller.cpp` `blockSleep` edit.
- **Frame pacing** uses OS2.1's `desiredFrameMs()` override (in
  `printstream.h`): returns `33` (~30 fps) while on the camera page so new MJPEG
  frames (bumped by `psCamGen()`) are picked up continuously, and `0`
  (event-driven) on the stats page. No `model.revision++` hacks.
- **Self-init, no `main.cpp` edit.** Background tasks are started lazily and
  idempotently from `PrintStreamView::onEnter()` via `printstreamEnsureStarted()`
  (which calls the still-present `printstreamInit()` + `printstreamStartTasks()`,
  both now idempotent). Nothing is wired into `main.cpp`.
- **Libraries.** ArduinoJson and JPEGDEC are already in the OS2.1 `lib_deps`
  (`platformio.ini`) — nothing re-added or vendored.
- **Secrets include path.** Added `-Isecrets` to the `[env:ewatch]`
  `build_flags` so `#include "printer.(local|example).h"` resolves (the stock
  OS2.1 only put `-Isecrets` on the opt-in `ewatch_secrets` env).
- WiFi left **ON** (`-DEWATCH_ENABLE_WIFI=1`) — the app needs it, and OS2.1's
  Spotify/Gallery/WiFi-settings depend on it too.

---

## 1. What it does

Launcher tile **"Printer"** → a single app with two pages, toggled by a vertical
swipe:

- **Stats page** (default): print state (Standby/Printing/Paused/Complete/…),
  job filename, a progress bar + %, nozzle and bed temps (current/target), and
  three control buttons — **Pause / Resume / Home**. Each control opens a
  **CONFIRM / CANCEL modal**; the command is only posted to the printer after
  CONFIRM. Swipe up → camera.
- **Camera page**: decodes the printer's live MJPEG feed (JPEGDEC) to the
  screen, with a bottom status strip (state + %). Swipe down (or tap the strip)
  → stats.

Back = hardware button / right-swipe (ButtonShort) → returns to the launcher.
The PrintStream screen blocks auto-sleep (`power_mgr.cpp`) so the display stays
lit while you watch a print.

The watch must be joined to the same LAN as the printer (Settings → WiFi,
client mode).

---

## 2. Camera endpoint — DISCOVERED

Probed the two live printers (`192.168.0.214`, `192.168.0.244`) with `curl`
(status/GET only — no control/gcode sent). Findings:

- Moonraker `GET /server/webcams/list` → `{"webcams": []}` (confirmed empty, as
  the brief warned — Creality runs its own cam daemon, not a Moonraker webcam).
- The printers serve **Fluidd** (Klipper web UI) behind **nginx on port 4408**.
- Moonraker `GET /server/config` reports the Fluidd webcam config:
  `stream_url = "/webcam/?action=stream"`, `webcam_enabled = true`,
  `service = "mjpegstreamer-adaptive"`.
- `GET http://<ip>:4408/webcam/?action=stream` returned **502 Bad Gateway**
  (nginx) during exploration because **both printers were idle** — the
  mjpg-streamer upstream the proxy points at isn't serving frames while no
  print/camera is active. The route exists; the daemon just wasn't up.
- Ports that did **not** serve the cam: `:8080` (refused), `:8000` (different
  Creality HTML portal), `:1984`/`:8554` (go2rtc / rtsp — closed).
  `http://192.168.0.170` is a **separate** device (its own Creality web UI).

**Working URLs (per printer):**
```
MJPEG stream : http://<printer-ip>:4408/webcam/?action=stream
JPEG snapshot: http://<printer-ip>:4408/webcam/?action=snapshot
```
Standard mjpg-streamer multipart feed (`multipart/x-mixed-replace` with
`Content-Length` per JPEG part) — exactly what the decoder expects.
**Could not be frame-verified** (no camera live during exploration — see §6).
If your K2 Plus uses a different port/path, change it in
`secrets/printer.local.h` (no code edit needed).

**Fallback built in:** if the MJPEG stream can't be opened or sends no usable
per-part `Content-Length`, the camera task automatically falls back to polling
`?action=snapshot` every `CAM_SNAPSHOT_MS`. If neither works the page shows
"Camera unavailable" and the stats page still works fully. Set `CAM_USE_MJPEG 0`
for snapshot-only.

---

## 3. Moonraker calls used

Base: `http://<printer-ip>:7125` (plain HTTP, no auth — Moonraker default).

| Purpose | Request |
|---|---|
| Poll state/temps/progress | `GET /printer/objects/query?print_stats&heater_bed&extruder&display_status` |
| Pause (confirmed) | `POST /printer/print/pause` |
| Resume (confirmed) | `POST /printer/print/resume` |
| Home (confirmed) | `POST /printer/gcode/script?script=G28` |

Parsed from the query reply (ArduinoJson):
`result.status.print_stats.{filename,state,print_duration}`,
`result.status.display_status.progress` (0..1 → %),
`result.status.extruder.{temperature,target}`,
`result.status.heater_bed.{temperature,target}`.

Confirmed live during exploration (real responses captured):
- `.214` → `state: "standby"`, progress 0, bed 21.9°, nozzle 23.5°.
- `.244` → `state: "complete"`, last job
  `Magnet Brushes Kill Switch Enclosure.step_PETG_35m51s.gcode`, progress 1.0.

Poll cadence: `PS_POLL_PRINTING_MS` (2 s) while printing/paused, else
`PS_POLL_IDLE_MS` (5 s); exponential backoff + an "offline" flag after repeated
errors.

---

## 4. Architecture / files

Mirrors `Spotify_Controller`: background FreeRTOS tasks own all network I/O; the
view only reads immutable snapshots and posts commands.

- `src/apps/printstream.h` — public API + `PrintStreamView` (with the OS2.1
  `desiredFrameMs()` override).
- `src/apps/printstream.cpp` — two tasks, started lazily via
  `printstreamEnsureStarted()`:
  - `psNetTask` (core 0, prio 4, 8 KB): Moonraker poll + confirmed commands.
  - `psCamTask` (core 0, prio 3, 10 KB): opens the MJPEG stream (or snapshot
    fallback) only while the camera page is active; writes each complete JPEG
    into a PSRAM buffer and bumps a generation counter.
  - Entire file is `#if EWATCH_ENABLE_WIFI`-guarded, with no-op stubs otherwise.
- `src/apps/printstream_view.cpp` — the UI; decodes camera JPEGs with JPEGDEC on
  the render task (same pattern as Spotify album art), draws stats, and gates
  controls behind the confirm modal. `onEnter()` calls
  `printstreamEnsureStarted()`.
- OS2.1 integration: `src/core/model.h` (`Screen::PrintStream`),
  `src/apps/system/registry.cpp` (include / instance / viewFor / launcher tile),
  `src/core/power_mgr.cpp` (`screenBlocksSleep`). No `main.cpp` change.

### Secrets
`secrets/printer.example.h` (tracked template) → copy to
`secrets/printer.local.h` (git-ignored via `.gitignore`) and edit. Holds printer
IP, Moonraker port, camera host/port/paths, poll cadence, and the
MJPEG-vs-snapshot toggle. Nothing is hardcoded in the app sources. The build
falls back to the example header if no `.local.h` exists, so it always compiles.
`secrets/` is on the include path via `-Isecrets` in `platformio.ini`.

---

## 5. What's stubbed / simplified (MVP honesty)

- **No live frame ever decoded by me.** The MJPEG multipart parser and the
  JPEGDEC path compile and follow the exact album-art pattern that works in
  Spotify, but no printer camera was streaming during exploration, so
  end-to-end camera decode is **unverified on real data**. If frames arrive
  without `Content-Length`, the app falls back to snapshot polling.
- **Pause/Resume/Home were never actually sent** (brief forbids control during
  exploration). The endpoints are the standard Moonraker ones; the request
  plumbing is the same path proven against the live query endpoint.
- No ETA/time-remaining, no multi-printer switching in the UI (one printer per
  `.local.h`; both known IPs are noted in the example header). No gcode
  thumbnail. Temps are integer-rounded.
- Progress uses `display_status.progress`; on some setups `print_stats` carries
  a more precise value — fine for MVP.

---

## 6. EXACT hardware-verify steps (Ewan)

1. **Configure the printer.** `cp secrets/printer.example.h secrets/printer.local.h`
   and set `PRINTER_HOST` to the printer you'll test (e.g. `192.168.0.214`).
   Leave camera defaults unless your cam is elsewhere.
2. **Build + flash:**
   ```
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd ShortsApps/printstream
   ~/.platformio/penv/bin/pio run -e ewatch -t upload -t monitor
   ```
3. **Join WiFi:** on the watch, Settings → WiFi → client mode, connect to the
   LAN the printer is on. Confirm the WiFi icon shows connected.
4. **Open the app:** swipe up from the watch face → **Printer**. On the **Stats**
   page you should see the live state, job name, progress and temps updating
   (compare against Fluidd at `http://<ip>:4408`). With a printer idle it should
   read "Standby"/"Complete" and real temps — that alone proves Moonraker
   polling.
5. **Camera:** **start a print (or enable the camera) so the cam daemon is up**,
   then swipe up to the camera page. Expect live frames. If you see
   "Camera unavailable", check `http://<ip>:4408/webcam/?action=stream` in a
   browser on the same LAN — if the browser shows video but the watch doesn't,
   capture the serial log (`PS: cam ...` lines) and the `Content-Type`; the
   likely cause is a missing per-frame `Content-Length` (then snapshot fallback
   should still show stills) or colour-swapped output (switch
   `RGB565_LITTLE_ENDIAN` → `RGB565_BIG_ENDIAN` + `draw16bitBeRGBBitmap` in
   `printstream_view.cpp`, same note as Spotify).
6. **Controls (do last, on a safe/throwaway print):** tap Pause → CONFIRM; the
   print should pause and the state flip to "Paused" within ~2 s. Resume →
   CONFIRM. Home → CONFIRM only when the bed is clear. Watch the serial line
   `PS: cmd N -> HTTP 200` for the result of each.

Serial is 115200; look for `PS:` log lines for poll/cmd/camera diagnostics.

---

## 7. Risks

- **Camera framing unverified** (no live cam during build) — see §5. Snapshot
  fallback de-risks this: worst case you get periodic stills, not video.
- **MJPEG bandwidth / decode time on the S3.** Full-res frames are downscaled by
  JPEGDEC (1/2…1/8 chosen from the source width). If frames are large the net
  task may lag; the stall guard (`CAM_STALL_MS`) drops a dead stream rather than
  tripping the task watchdog. If video is choppy, prefer snapshot mode
  (`CAM_USE_MJPEG 0`) or lower the camera resolution on the printer.
- **Control actions hit real hardware.** Mitigated by the on-screen confirm; the
  command is posted only after CONFIRM, and the queue is flushed if WiFi drops so
  nothing fires late.
- `secrets/printer.local.h` must exist with the right IP at flash time, or the
  build uses the example default (`192.168.0.214`).

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-06
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to PrintStream ('WiFi not connected' state, Pause/Resume/Home buttons drawn), 1.2 s idle, 0 errors (5 steps). Snap: `.ewt/boot.png`.
- NOT exercised: the printer WiFi/stats path. ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
