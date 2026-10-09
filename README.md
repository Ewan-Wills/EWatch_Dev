# EWatch BaseOS

A **bare-bones firmware slate** for the EWatch (custom ESP32-S3 wearable). Every
driver, core subsystem, and the full system shell is present and working — but
**no user apps are installed**. It boots to the watch face; swiping up shows a
launcher with a single `System` entry (settings, diagnostics, power off).

This is the clean foundation to build new apps on. If you're an AI agent (or a
human), the build-an-app guide is **[CLAUDE.md](CLAUDE.md)** — read it first.

## What's included

- **Drivers** — ST7789 display + PWM backlight, CST816S touch, DRV2603 haptic,
  shared I2C bus, soft-latch power, canonical pin map.
- **Core loop** — single shared `model` (mutex-guarded), event queue, `View`
  framework, a two-task controller (I/O @ 50 Hz on core 0, render on core 1),
  NVS-backed settings persistence.
- **System shell** — watch face (multiple styles + 7-segment renderer), the
  swipe carousel launcher, all Settings pages, diagnostics (Sensor Test, Touch
  Gestures, IMU Gestures), and Power Off.
- **Optional subsystems, kept available** — animation/tween engine
  (`core/anim`), RTC-anchored timer-wake-from-sleep (`core/apptimer`), and WiFi +
  on-device web settings server (`core/wifi_svc`). Drop the ones an app doesn't
  use — see CLAUDE.md §0 and §9.

## What was removed

The five reference apps (Stopwatch, Timer, QR Share, Media viewer, 3D Viewer)
and their assets, plus the media-encoder tool, so the launcher starts empty.
The platform capabilities they relied on (timer-wake, animation) remain.

## Hardware

| Item | Value |
|---|---|
| SoC | ESP32-S3FH4R2 (4 MB flash, 2 MB QSPI PSRAM, native USB) |
| Display | ST7789 240×280 IPS, RGB565 over FSPI |
| Touch | CST816S capacitive over the shared I2C bus |
| IMU | MMA8451 accelerometer |
| RTC | RV-3028 |
| Haptic | DRV2603 motor driver |
| Power | soft-latch LDO with pushbutton wake/shutdown (no hard switch) |

Authoritative pin map: **[PINOUT.md](PINOUT.md)** / `src/drivers/pins.h`.

## Build

PlatformIO. If `pio` isn't on your PATH, use `~/.platformio/penv/bin/pio`.

```sh
pio run                       # build the firmware (env:ewatch)
pio run -t upload -t monitor  # flash + serial monitor @ 115200
pio run -e disptest -t upload -t monitor   # standalone display + touch bring-up test
```

Feature gates live in `platformio.ini` (e.g. `-DEWATCH_ENABLE_WIFI=1`); flip a
gate to `0` to compile that subsystem out.

## Repository layout

```
src/main.cpp            boot, power-latch safety, crash-loop guard, task launch
src/drivers/            hardware abstractions + pins.h
src/core/               model, event, view, controller, storage, anim, apptimer, wifi_svc
src/apps/system/        the system shell (watch face, launcher, settings, diagnostics)
src/apps/assets/fonts/  GFX FreeFonts on the include path
src/test/               disp_touch_test.cpp (env:disptest)
CLAUDE.md               how to add an app — start here
PINOUT.md               firmware pin reference
```

## Important

- `latchPower()` is intentionally the first line of `setup()`. The watch holds
  its own power rail; never delay or remove it.
- All I2C runs on the single I/O task — views read the `model`, they don't touch
  the bus. RTC writes go through `requestSetRTC()`.
- A per-task watchdog + RTC-memory panic-loop guard drop the power rail after
  repeated crashes so a hang can't drain the battery unrecoverably.

## License

No license file is currently included. Treat the code as engineering work in
progress until a license is explicitly added.
