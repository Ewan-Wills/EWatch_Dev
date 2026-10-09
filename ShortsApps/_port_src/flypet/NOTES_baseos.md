# flypet — build notes

A virtual-fly tamagotchi for the EWatch. A procedurally-drawn housefly lives on
the watch as a pet with state (hunger / energy / mood) and behaviours
(walk / groom / rest / feed). A Mac-side "brain" can drive it over USB serial;
with no PC attached the watch runs its own small autonomous brain so the pet is
always alive.

## Honest scope

- **The heavy sim is Mac-side and the whole thing is behaviour-inspired, not a
  connectome.** Running the real Drosophila connectome on an ESP32-S3 is
  impossible, and `pc/flypet_sim.py` does **not** attempt it — it is a small
  finite-state machine over four behaviours and three drives. It is a nod to the
  connectome work, nothing more. There are no datasets anywhere.
- The watch is a thin client: it animates whatever behaviour/stats it is told,
  and falls back to an equally-simple on-watch state machine when no PC is
  connected.

## What's on the watch

- Reachable from the launcher: swipe up from the watch face → **FlyPet** tile
  (first tile, before System).
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
  redraw per frame with `flush()` — no flicker. The view bumps `model.revision`
  each frame so the render task keeps a steady ~20 fps animation loop
  independent of sensor-noise cadence.

## Input

- **Tap inside the arena** → drops a food crumb there, the fly walks over to eat
  it, and the watch sends `>>FEED x y` to the PC. Haptic buzz on feed.
- **Shake** (IMU motion) → the fly is startled and buzzes off to a new spot.
- **Back** → hardware button short-press or right-swipe (both arrive as
  `ButtonShort`) returns to the launcher.

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

Close any `pio device monitor` first — the USB serial is the same port.

## Subsystems trimmed

- **WiFi gated off**: `-DEWATCH_ENABLE_WIFI=0` in `platformio.ini` (drops
  WiFi/WebServer/DNSServer and the WiFi settings page). No networking is used.
- Diagnostics screens and `anim.*`/`apptimer.*` are left in place (unused by this
  app but harmless; the linker's `--gc-sections` drops dead code). The fly
  animation is hand-rolled in the view, not via `anim.*`.

## What's stubbed / simplified (MVP)

- The on-watch autonomous brain is intentionally tiny (random behaviour picks +
  linear drive decay). The Mac brain is only slightly richer. Neither models
  real fly neuroscience.
- Fly stats are **not persisted** across app exits / deep sleep — the pet resets
  to default stats each time the screen is opened. (Persisting via `storage` /
  NVS would be the obvious next step; left out for MVP.)
- No sound (the watch has no audio hardware — only haptics, which are used).
- `x`/`y` motion is a straight-line seek; no obstacle/path logic.

## Risks / things to watch on hardware

- **Animation smoothness** depends on the render task getting frequent redraws.
  We force this by bumping `model.revision` every frame. If CPU load is a
  concern, lower the effective frame rate by only bumping every other frame.
- **Serial contention**: the pet streams `>>STAT` telemetry every ~2 s and on
  events. This shares the USB CDC used for flashing/monitoring — don't run
  `pio device monitor` and `flypet_sim.py` at once.
- `frameCanvas()` returns null if PSRAM is exhausted; handled with a fallback
  "no mem" message, but if it happens the pet won't animate.
- Auto-sleep is **not** blocked, so the watch will deep-sleep after the normal
  idle timeout even with the pet on screen. If you want the pet to stay alive
  during a demo, bump the sleep timeout in Settings or add `Screen::FlyPet` to
  the `blockSleep` OR-chain in `controller.cpp` (`taskRender`).

## EXACT hardware-verify steps (for Ewan)

1. Build + flash:
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd EWatch_Firmware/ShortsApps/flypet
   ~/.platformio/penv/bin/pio run -e ewatch -t upload
   ```
2. On the watch: swipe up from the watch face → tap the **FlyPet** tile. Expect
   a fly walking/grooming/resting on its own, with the status line showing
   `[solo]` and the three stat bars drifting over time.
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

`pio run -e ewatch` → **SUCCESS**. RAM 7.1% (23,252 B), Flash 14.9%
(470,281 B of 3,145,728 B). Only compilation is verified here — on-hardware
behaviour is for Ewan to confirm with the steps above.
