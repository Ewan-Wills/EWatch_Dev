# EWatch — 3D Racer

A pseudo-3D, OutRun-style arcade racer for the EWatch (ESP32-S3). Tilt the
watch to steer down an endless projected road; the whole point is the *sense of
speed* on a tiny portrait screen.

> **Status:** playable. Curving scrolling road with a sense of speed, tilt
> steering (the curve shoves you outward — fight it), auto-accelerate,
> hold-to-boost, off-road slow + rumble, traffic cars to overtake, crash → game
> over with instant retry, live distance/speed/passed HUD, and persisted best
> distance. Next up: hills, then roadside scenery sprites.

## Hardware

| Item | Value |
|---|---|
| SoC | ESP32-S3FH4R2 (4 MB flash, 2 MB QSPI PSRAM) |
| Display | ST7789 240×280 IPS over FSPI, RGB565 |
| Tilt | MMA8451 accelerometer (steering) |
| Touch | CST816S capacitive (boost / brake) |
| Haptic | DRV2603 (collision buzz, off-road rumble) |
| Power | soft-latch LDO + pushbutton; no hard switch |

Pin map: see [PINOUT.md](PINOUT.md) (authoritative). Pins live in
[src/drivers/pins.h](src/drivers/pins.h).

## What the game can build on

- **Framebuffer blit** — `frameCanvas()` returns a 240×280 PSRAM
  `Arduino_Canvas`. Draw the whole frame, then `flush()` once → tear-free
  motion. This is the rendering core the projected road needs.
- **Tilt steering** — `taskIO` samples the MMA8451 at 50 Hz into
  `model.ax/ay/az`; the view reads it under a `ModelLock`.
- **Input events** — touch press/hold/release, gestures, and button
  short/long are delivered to `currentView->onEvent()`.
- **Haptic** — `hapticBuzz(intensity, ms)` is fire-and-forget.
- **Persistence** — `Storage::load()/save()` keep `model.bestDistance` (best
  score) and device settings in NVS.
- **Power safety** — soft-latch with a crash-loop guard + task watchdog (the
  watch can't be power-cycled without it), plus deep-sleep / auto-power-off.

## Architecture

- [src/main.cpp](src/main.cpp) — boot, power latch, crash recovery, wake
  validation, task launch.
- [src/drivers/](src/drivers/) — `display`, `touch`, `haptic`, `i2c_bus`,
  `power`, `pins`.
- [src/core/](src/core/) — `controller` (I/O + render tasks), `model` (shared
  state), `event`, `storage`, `view` (View interface).
- [src/apps/game/racer_view.cpp](src/apps/game/racer_view.cpp) — the game
  (road projection, driving model, HUD) and the view-framework glue. New game
  modules go under `src/apps/game/`.

## Build

PlatformIO. `pio` may not be on your PATH — use `~/.platformio/penv/bin/pio`.

```sh
pio run -e ewatch -t upload -t monitor      # build, flash, watch serial
```

## Controls

- **Tilt** = steer (smoothed; flip `STEER_SIGN` in `racer_view.cpp` if reversed).
- Auto-accelerate; **hold a finger on the screen** = boost.
- Off-road (onto the grass) slows you and rumbles the haptic.
- **Short button press** starts a run / ends it; **hold 3 s** powers the watch off.
- Score = distance; best distance persists to NVS and shows on the start screen.

The driving feel (`MAX_SPEED`, `ACCEL`, `STEER_*`, road look) is all named
constants at the top of `racer_view.cpp` — tune on hardware.
