# flypet module notes

Carried over from the stand-alone `flypet/` folder it was developed in
(before 2026-10-09). OS-wide parts of these notes may be out of date.

---

# flypet — build notes (EWatchOS2.1)

A virtual-fly tamagotchi for the EWatch. A procedurally-drawn housefly lives on
the watch as a pet with state (hunger / energy / mood) and behaviours
(walk / groom / rest / feed). A Mac-side "brain" can drive it over USB serial;
with no PC attached the watch runs its own small autonomous brain so the pet is
always alive.

**This app is now ported onto the current shipping `EWatchOS2.1`** (full OS:
watch faces, launcher, settings, WiFi, Spotify, games, etc. — the fly is added
on top). Previously it was a trimmed BaseOS copy. See "Port to OS2.1" below for
exactly what changed.

## Honest scope

- **The heavy sim is Mac-side and the whole thing is behaviour-inspired, not a
  connectome.** Running the real Drosophila connectome on an ESP32-S3 is
  impossible, and `pc/flypet_sim.py` does **not** attempt it — it is a small
  finite-state machine over four behaviours and three drives. It is a nod to the
  connectome work, nothing more. There are no datasets anywhere.
- The watch is a thin client: it animates whatever behaviour/stats it is told,
  and falls back to an equally-simple on-watch state machine when no PC is
  connected.
- **Only compilation is verified here** (`pio run -e ewatch` → `[SUCCESS]`).
  On-hardware behaviour is for Ewan to confirm with the steps at the bottom.

## What's on the watch

- Reachable from the launcher: swipe up from the watch face → **FlyPet** tile
  (in the apps list, between "Pet" and "Island"), icon `Bird`.
- Vector-drawn fly (no bitmap assets), oriented to its heading: tapered abdomen
  with segment banding, iridescent thorax, red compound eyes, antennae, six
  animated legs, two fluttering wings.
- Behaviours: **walk** (legs cycle, wings flutter, moves to target), **groom**
  (stationary, front legs rub over the head), **rest** (wings folded, rising
  "z"s), **feed** (walks to the food crumb and eats, hunger drops).
- HUD: three stat bars — Food (= 100 − hunger), Enrg (energy), Mood — colour
  coded green/amber/red. Status line shows the current behaviour and `[PC]` or
  `[solo]`, plus a link dot (green when a PC is driving).
- Rendered through the shared PSRAM frame canvas (`frameCanvas()`), full-canvas
  redraw per frame with `flush()` — no flicker.

## Port to OS2.1 — what changed from the BaseOS copy

1. **Registration moved out of `view.cpp` into `apps/system/registry.cpp` +
   `core/model.h`.** Added:
   - `core/model.h`: `FlyPet,` in the `enum class Screen` user-app region (after
     `Island,`).
   - `registry.cpp`: `#include "flypet.h"`; `static FlyPetView vFlyPet;`;
     `case Screen::FlyPet: return &vFlyPet;` in `viewFor()`; and one launcher row
     `{ "FlyPet", Icon::Bird, aura::kPurple, Screen::FlyPet, nullptr }`.
2. **Continuous animation now uses `desiredFrameMs()`, not `model.revision++`.**
   OS2.1's render task only re-renders on events + a 1 Hz heartbeat unless the
   view asks for frames. `FlyPetView::desiredFrameMs()` returns `33` (≈30 fps)
   whenever the view is on screen. The old per-frame `model.revision++` bump in
   `render()` is removed (it no longer drives the OS2.1 render task).
3. **Keep-awake moved to `core/power_mgr.cpp` → `screenBlocksSleep()`.** Added
   `case Screen::FlyPet: return true;` there so the watch stays lit while the pet
   is on screen with no touch input. **This fixes the bug the BaseOS NOTES flagged
   as unfixed** ("Auto-sleep is not blocked … add Screen::FlyPet to the blockSleep
   OR-chain"). It is now done — the pet no longer deep-sleeps out from under a
   demo at the idle timeout.
4. **WiFi is kept ON** (`-DEWATCH_ENABLE_WIFI=1`). The BaseOS copy gated WiFi off
   as a size optimisation; on OS2.1 that would strip real OS features
   (Spotify / Photos / WiFi settings), so the gate is left at its OS default.
   flypet itself uses no networking — it just doesn't disable it.
5. **Serial listener is now a lazy background task started from `onEnter()`**
   (`flypetSerialStart()`), not an inline `pumpSerial()` in `render()` and not a
   hook in `main.cpp`. A dedicated FreeRTOS task (`"flypet"`, core 0) owns the
   USB-CDC **read** side, assembles newline-terminated lines and hands them to the
   UI task through a 6-slot queue. `render()` drains the queue and applies the
   commands, so **all mutation of the pet state stays on the UI task** (no data
   race) and the task never writes to Serial (so the TX telemetry on the UI task
   never contends with it). The task is created once, idempotently.
6. **Libraries**: nothing re-added or vendored — OS2.1's `lib_deps` already has
   GFX / CST816S / QRCode / ArduinoJson / JPEGDEC.

## Input

- **Tap inside the arena** → drops a food crumb there, the fly walks over to eat
  it, and the watch sends `>>FEED x y` to the PC. Haptic buzz on feed.
- **Shake** (IMU motion) → the fly is startled and buzzes off to a new spot.
- **Back** → hardware button short-press or right-swipe (both arrive as
  `ButtonShort`) returns to the launcher (`Screen::AppList`).

## Serial protocol (USB CDC, 115200 8N1)

PC → watch (newline-terminated ASCII, `FLY` must be the first token):

```
FLY action=<walk|groom|rest|feed|wander> x=<0..100> y=<0..100> \
    hunger=<0..100> energy=<0..100> mood=<0..100> state=<label>
```

All keys optional; unseen keys keep their previous value. `x`/`y` are the target
position in arena percent. A `feed` action plants a crumb at `x,y`.

Watch → PC (lines prefixed `>>`, so the PC parser skips boot/log noise):

```
>>HELLO flypet v1          on screen open
>>STAT hunger=.. energy=.. mood=.. action=.. x=.. y=..   ~every 2 s
>>FEED x=.. y=..           user tapped to feed
>>BYE flypet               on screen exit
```

The watch treats itself as **PC-driven** while a `FLY` line arrived within the
last 4 s, otherwise **autonomous**.

## How to run the PC side

See `pc/README.md`. In short:

```sh
cd pc
pip install -r requirements.txt
python3 flypet_sim.py            # auto-detects /dev/cu.usbmodem*
```

Close any `pio device monitor` first — the USB serial is the same port. (`pc/`
was not changed by the OS2.1 port.)

## What's stubbed / simplified (MVP)

- The on-watch autonomous brain is intentionally tiny (random behaviour picks +
  linear drive decay). The Mac brain is only slightly richer. Neither models
  real fly neuroscience.
- Fly stats are **not persisted** across app exits / deep sleep — the pet resets
  to default stats each time the screen is opened. (Persisting via `storage` /
  NVS would be the obvious next step; left out for MVP.)
- `gStateLabel` (optional `state=` from the PC) is parsed but not yet drawn on
  screen — carried over from the BaseOS version, harmless.
- No sound (the watch has no audio hardware — only haptics, which are used).
- `x`/`y` motion is a straight-line seek; no obstacle/path logic.

## Risks / things to watch on hardware

- **Animation smoothness** now depends on OS2.1's render task honouring
  `desiredFrameMs()` (≈30 fps). If CPU load is a concern, raise the return value
  (e.g. 50 ms ≈ 20 fps).
- **Serial contention**: the pet streams `>>STAT` telemetry every ~2 s and on
  events. This shares the USB CDC used for flashing/monitoring — don't run
  `pio device monitor` and `flypet_sim.py` at once.
- The serial listener task calls `esp_task_wdt_add(nullptr)` / `..._reset()` each
  loop like the OS's other background tasks; it blocks on `Serial.read()` with a
  10 ms poll delay when idle.
- `frameCanvas()` returns null if PSRAM is exhausted; handled with a fallback
  "no mem" message, but if it happens the pet won't animate.
- Sleep is now blocked on `Screen::FlyPet`, so the watch stays lit while the pet
  is up (fixed in this port).

## EXACT hardware-verify steps (for Ewan)

1. Build + flash:
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd EWatch_Firmware/ShortsApps/flypet
   ~/.platformio/penv/bin/pio run -e ewatch -t upload
   ```
2. On the watch: swipe up from the watch face → tap the **FlyPet** tile. Expect
   a fly walking/grooming/resting on its own, with the status line showing
   `[solo]` and the three stat bars drifting over time. The screen should stay
   lit (sleep is blocked on this screen now).
3. **Tap the fly / arena**: a yellow crumb appears, the fly walks to it and eats
   (Food bar rises), with a haptic buzz.
4. **Shake the watch**: the fly should startle and dart to a new spot ("bzzzt!").
5. **Back** (button short-press or right-swipe) returns to the launcher.
6. PC drive: quit any serial monitor, then
   ```sh
   cd pc && pip install -r requirements.txt && python3 flypet_sim.py
   ```
   The watch status line should flip to `[PC]` (green dot, one haptic buzz) and
   the fly should follow the Mac brain. Tapping the fly should print
   `>>FEED x=.. y=..` in the script's console and the fly walks to eat.
7. Unplug / stop the script → within ~4 s the watch returns to `[solo]`.

## Compile status

`pio run -e ewatch` → **[SUCCESS]**. RAM 61.2% (200,588 B of 327,680 B), Flash
54.5% (1,714,677 B of 3,145,728 B). These figures are for the **full OS2.1 + the
fly**, not the trimmed BaseOS build, so they are much larger than the old notes
— that is expected. Only compilation is verified here; on-hardware behaviour is
for Ewan to confirm with the steps above.

## 2026-10-05 — serial hardening (traps A-E from the bookreader fix)

- **Changed (Trap A, console steals bytes):** `src/apps/flypet.cpp`. The
  `flypet` listener task read `Serial` forever, even after the screen exited,
  and the OS `ewcon` console task read it at the same time, so FLY lines were
  split between the two (console would answer `unknown: ... (try help)`).
  Now `onEnter` calls `ewlogConsolePause(true)` and sets `gOwnSerial`; the
  listener only reads while `gOwnSerial` is set; `onExit` clears it and calls
  `ewlogConsolePause(false)`, so the console is never left paused.
- **Not changed:** Trap B (PC sends ~80 B lines at 10 Hz, never >4 KB unacked),
  Trap C (`>>STAT/>>FEED/>>HELLO` are telemetry; the PC never blocks waiting
  for them), Trap D (only the built-in font is used; every custom-font user in
  the tree resets `setFont(nullptr)`), Trap E (no mutex held). `pc/flypet_sim.py`
  unchanged. Build `pio run -e ewatch` -> SUCCESS. Not tested on hardware.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-07
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to FlyPet, animates standalone at 10.5 fps (expect fps 10), solo brain ('feeding [solo]', Food/Enrg/Mood bars), tap stays in-app, 0 errors (11 steps). Snap: `.ewt/boot.png`. The app owns USB serial, so the smoke runs with `raw keepcon 1` (ewlog test hook): the console stays with ewt and the app's PC RX reader is deliberately inert.
- NOT exercised: the live PC-data path (pc/ companion over USB serial). ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
- Harness notes: the app's own serial output (`>>STAT`, `>>HELLO`) can interleave with a snapshot stream and corrupt it; the first ewt command after the app is already open sometimes gets no reply (second run passes).
