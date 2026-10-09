# printstream — NOTES

EWatch app: live camera + print state + basic control for a Creality K2 Plus
(Klipper/Moonraker). WiFi client; no PC companion. Only Ewan flashes & accepts —
this document is the hand-off. **Compiles clean; not hardware-verified.**

Final build (isolated core):
```
Flash: 33.2% (1,043,269 / 3,145,728 bytes)
RAM:   21.3% (69,748 / 327,680 bytes)
ewatch  SUCCESS
```

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

Back = hardware button / right-swipe (ButtonShort), as per BaseOS. The Printer
screen is added to the auto-sleep `blockSleep` chain in `controller.cpp` so the
display stays lit while you watch a print.

WiFi is kept on (`-DEWATCH_ENABLE_WIFI=1`). The watch must be joined to the same
LAN as the printer (configure the network in Settings → WiFi, client mode).

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
- `GET http://<ip>:4408/webcam/?action=stream` currently returns **502 Bad
  Gateway** (nginx) because **both printers were idle during exploration** — the
  mjpg-streamer upstream the proxy points at isn't serving frames while no
  print/camera is active. The route exists; the daemon just wasn't up.
- Ports probed that did **not** serve the cam: `:8080` (connection refused),
  `:8000` (returns a different Creality HTML portal), `:1984`/`:8554` (go2rtc /
  rtsp — closed). `http://192.168.0.170` is a **separate** device (its own
  Creality web UI, no port 4408).

**Conclusion / working URL to use (per printer):**
```
MJPEG stream : http://<printer-ip>:4408/webcam/?action=stream
JPEG snapshot: http://<printer-ip>:4408/webcam/?action=snapshot
```
This is a standard mjpg-streamer multipart feed (`multipart/x-mixed-replace`
with `Content-Length` per JPEG part), which is exactly what the app's decoder
expects. **This could not be frame-verified** because no camera was live during
exploration — see §6 verify steps. If your K2 Plus uses a different port/path,
change it in `secrets/printer.local.h` (no code edit needed).

**Fallback built in:** if the MJPEG stream can't be opened or sends no usable
`Content-Length`, the camera task automatically falls back to polling
`?action=snapshot` every `CAM_SNAPSHOT_MS`. If neither works the page shows
"Camera unavailable" and the stats page still works fully. Set `CAM_PORT 0` to
disable the camera entirely (stats-only).

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

- `src/apps/printstream.h` — public API + `PrintStreamView`.
- `src/apps/printstream.cpp` — two tasks:
  - `psNetTask` (core 0, prio 4, 8 KB): Moonraker poll + confirmed commands.
  - `psCamTask` (core 0, prio 3, 10 KB): opens the MJPEG stream (or snapshot
    fallback) only while the camera page is active; writes each complete JPEG
    into a PSRAM buffer and bumps a generation counter.
  - Entire file is `#if EWATCH_ENABLE_WIFI`-guarded, with no-op stubs otherwise.
- `src/apps/printstream_view.cpp` — the UI; decodes camera JPEGs with JPEGDEC on
  the render task (same pattern as Spotify album art), draws stats, and gates
  controls behind the confirm modal.
- Registered in `src/core/model.h` (`Screen::PrintStream`) and the 4 sites in
  `src/apps/system/view.cpp`. Init wired in `src/main.cpp`
  (`printstreamInit()` / `printstreamStartTasks()`).
- `src/core/controller.cpp` — `Screen::PrintStream` added to the `blockSleep`
  OR-chain so the watch stays awake on this app.

### Secrets
`secrets/printer.example.h` (tracked template) → copy to
`secrets/printer.local.h` (git-ignored via `.gitignore`) and edit. Holds printer
IP, Moonraker port, camera host/port/paths, poll cadence, and the
MJPEG-vs-snapshot toggle. Nothing is hardcoded in the app sources. The build
falls back to the example header if no `.local.h` exists, so it always compiles.
`secrets/` is on the include path via `-Isecrets` in `platformio.ini`.

### Dependencies added
`platformio.ini` `lib_deps` gained `bblanchon/ArduinoJson @ ^7` and
`bitbank2/JPEGDEC @ ^1.6`. Their packages were copied into
`.pio/libdeps/ewatch/` from the already-populated Spotify_Controller project so
the build stays **offline** against the isolated core.

---

## 5. What's stubbed / simplified (MVP honesty)

- **No live frame ever decoded by me.** The MJPEG multipart parser and JPEGDEC
  path compile and follow the exact album-art pattern that works in Spotify, but
  no printer camera was streaming during exploration, so end-to-end camera
  decode is **unverified on real data**. The parser assumes mjpg-streamer's
  `Content-Length`-per-part framing (the Fluidd config confirms
  `mjpegstreamer-adaptive`). If frames come without `Content-Length`, the app
  falls back to snapshot polling automatically.
- **Pause/Resume/Home were never actually sent** (brief forbids control during
  exploration). The endpoints are the standard Moonraker ones; the request
  plumbing is the same GET path proven against the live query endpoint.
- No ETA/time-remaining, no multi-printer switching in the UI (one printer per
  `.local.h`; the two known IPs are in the example header — change one line to
  point at the other). No thumbnail of the gcode. Temps are integer-rounded.
- Progress uses `display_status.progress`; on some setups `print_stats` has a
  more precise value — fine for MVP.

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
   read "Standby"/"Complete" and real temps — that alone proves Moonraker polling.
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
  tripping the 20 s task watchdog. If video is choppy, prefer snapshot mode
  (`CAM_USE_MJPEG 0`) or lower the camera resolution on the printer.
- **Control actions hit real hardware.** Mitigated by the on-screen confirm; the
  command is posted only after CONFIRM, and the queue is flushed if WiFi drops so
  nothing fires late.
- `secrets/printer.local.h` must exist with the right IP at flash time, or the
  build uses the example default (`192.168.0.214`).
