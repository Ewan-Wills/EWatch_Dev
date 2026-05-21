# EWatch Firmware (main branch)

https://www.ewanwills.co.uk/ewatch

> **Status:** beta / engineering build
>
> The firmware is hardware-validated, but this repo is not a finished consumer product.
> Expect active development, a few rough edges, and the need to verify wiring before powering the watch.

## What this is

EWatch is the firmware and supporting source for a custom ESP32-S3 watch project.
It is built for the ESP32-S3FH4R2 package (4 MB internal flash + 2 MB QSPI PSRAM) and is intended for a small, display-centric wearable with soft-latched power.

The code in this branch is the actual firmware for the current PCB design and is optimized around the hardware in `src/drivers/pins.h`.

## Key points

- Target MCU: **ESP32-S3FH4R2** only
- Flash: 4 MB internal
- PSRAM: 2 MB QSPI internal
- Display: ST7789 240x280 IPS over FSPI
- Touch: CST816S over shared I2C bus
- Power: soft-latched LDO; `latchPower()` must be the first line in `setup()`
- Pinout reference: see `PINOUT.md` (authoritative firmware pin map)

## Features

- timekeeping and system UI
- touch and button wake / sleep
- soft-latched power management with crash-safe shutdown
- battery monitoring with ADC and enable gate
- haptic feedback via DRV2603
- optional Wi-Fi / web service support
- optional QR rendering
- optional media browser + playback support
- optional 3D viewer app

## Hardware specs

| Item | Value |
|---|---|
| SoC | ESP32-S3FH4R2 (4 MB flash, 2 MB QSPI PSRAM) |
| Display | ST7789 240x280 IPS |
| Touch | CST816S capacitive touch over I2C |
| IMU | MMA8451 accelerometer |
| RTC | RV-3028 real-time clock |
| Haptic | DRV2603 vibration driver |
| Power | soft-latch LDO with pushbutton wake and shutdown |
| Bootloader | Arduino-ESP32 / PlatformIO build |

## Firmware architecture overview

- `src/main.cpp` handles boot, power-latch safety, early crash recovery, wake validation, and startup sequencing.
- `src/drivers/` contains low-level hardware interfaces:
  - `power.h` for the soft-latched power rail
  - `pins.h` for the canonical pin map
  - `display.h` and `touch.h` for the screen and touchscreen
  - `i2c_bus.cpp` for shared I2C bus access
- `src/core/` contains runtime logic and task coordination:
  - `controller.cpp` manages app flow and input
  - `view.cpp` implements the UI framework
  - `storage.cpp`, `event.cpp`, `apptimer.cpp`, `wifi_svc.cpp` are support systems
- `src/apps/` contains feature modules such as media, QR, stopwatch, timer, and 3D viewer.

### Important firmware details

- `latchPower()` is intentionally the first call in `setup()`.
  The watch has no hard power switch; the firmware must take over the LDO enable pin immediately.
- The project uses a crash-loop guard in RTC memory to avoid a dead system that cannot be power-cycled without removing the battery.
- Touch wake is validated by polling the CST816S sensor; spurious wakes are rejected before the rest of the system powers up.
- The chosen ESP32-S3FH4R2 variant is critical. A bare S3 package caused earlier boot-loop and reliability failures during board bring-up.

## Build from source

### Prerequisites

- PlatformIO
- Arduino-ESP32 platform compatible with `platformio.ini`
- `GFX Library for Arduino` pinned to `~1.4.7`
- `ricmoo/QRCode` dependency

### Build steps

1. Clone the repository.
2. Open the folder in VS Code with PlatformIO.
3. Ensure `platformio.ini` is configured for the correct board variant.
4. Run `pio run`.

### Notes

- `platformio.ini` currently targets `esp32-s3-devkitc-1` as the build board, but the firmware is designed for the S3FH4R2 package.
- If a feature gate is not needed, disable it in `platformio.ini` by setting the corresponding `-DEWATCH_ENABLE_*` flag to `0`.
- The build already enables `-ffunction-sections`, `-fdata-sections`, and linker GC to strip unused feature code.

## Repository layout

- `src/main.cpp` — boot/runtime initialization and power safety
- `src/drivers/` — hardware abstractions and pin definitions
- `src/core/` — controller, view, storage, events, and system logic
- `src/apps/` — app-specific code for timer, QR, media, viewer, etc.
- `platformio.ini` — build configuration and feature gates
- `tools/encode_media.py` — helper for media asset generation

## PINOUT reference

See `PINOUT.md` for the authoritative firmware pin mapping.

## Known issues

- Beta-quality firmware: expect some user flows to be rough and some hardware behaviour to be fragile.
- Power latch timing is strict. If `latchPower()` is delayed or omitted, the watch can fail to boot.
- The ADC battery monitor uses a bodged reroute from GPIO34 to GPIO5; verify the board before applying power.
- The display and touch interface are sensitive to signal integrity on FSPI/I2C.

## Roadmap

- complete stable power-on/off and UI flow
- tighten battery/fault handling
- finish Wi-Fi and web service integration
- improve boot diagnostics and crash reporting
- harden the hardware design for production-level reliability

## Brief history

This project began as a university wearable prototype and has been iterated for about five years.
It passed a university innovation competition and Innovate UK Converge, but this README is intentionally focused on what works now rather than the story behind it.

## Acknowledgements

- the ESP32-S3 platform community
- contributors to the Arduino GFX Library and QRCode library
- the hardware partners behind the current PCB bring-up

## License

This repository does not currently include a license file. Treat the code as engineering work in progress until a license is explicitly added.
