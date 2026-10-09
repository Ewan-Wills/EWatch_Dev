# EWatch — BasicDigital

A deliberately minimal watch firmware for the EWatch v2 hardware
(ESP32-S3FH4R2). It is a stripped-down sibling of **FirstOS**: the hardware
bring-up, power latch, deep-sleep / wake handling, panic-loop guard, task
watchdog, crash-recovery screen, and the model / event / controller / view
runtime architecture are all carried over **verbatim**. The only thing that
shrank is the app surface.

## Screens

1. **Watch face** — big `HH:MM`, a `:SS` row, and a `Wed 18 Jun 2026` date line.
   Drawn with the built-in bitmap font (no FreeFont assets). Tap anywhere to
   open Settings.
2. **Settings** — a four-item menu: **Set Time** / **Set Date** / **Sleep** /
   **Power Off**.
3. **Set Time / Set Date** — three `+` / `-` columns with the FirstOS
   hold-to-ramp behaviour (the longer you hold, the faster it counts). **SAVE**
   writes to the RV-3028 RTC. Setting the time preserves the date and vice
   versa; saving a date recomputes the weekday (Sakamoto's algorithm).
4. **Sleep** — two `-` / `+` stepper rows: **Sleep after** (idle timeout before
   the watch enters deep sleep — `Never`, `10s`, `15s`, `30s`, `1m`, `2m`, `5m`)
   and **Power off** (how long to then wait in deep sleep before dropping the
   rail entirely — `Never`, `1m`, `5m`, `30m`, `1h`). **SAVE** applies the choice
   to the running firmware and persists it to NVS so it survives a reboot.

Navigation back: the top-left `<` chevron, a swipe-right, or a short press of
SW2.

## Power, sleep & wake

* **Power latch** — `latchPower()` (drive GPIO17 high) is the first thing
  `setup()` does, so the watch holds its own rail the moment SW2 releases.
* **Auto-sleep** — after the **Sleep after** idle timeout (default 30 s, set on
  the Sleep page, or `Never` to disable) the watch enters deep sleep. By default
  it does **not** auto-power-off, so it stays revivable; set **Power off** on the
  Sleep page to have it drop the rail after a spell asleep.
* **Wake sources** — it comes out of deep sleep on a **screen tap**, an **SW2
  press**, or a **wrist jolt (IMU)** — all three are enabled by default (there
  is no settings page to toggle them). Touch wakes are validated against EMI
  false-alarms before the UI lights up, exactly as in FirstOS.
* **Power off** — the **Power Off** menu item or a **3 s SW2 hold** drops the
  LDO latch and fully powers down; after that only an SW2 press revives it.
* **Isolated settings** — BasicDigital uses its own NVS namespace (`basicd`),
  so it never inherits FirstOS's saved settings (which previously made it
  power fully off in sleep and appear dead).

## What was removed vs FirstOS

WiFi + web settings, the app carousel, the 3D model viewer, media player, QR
share, stopwatch, countdown timer, sensor-test / gesture diagnostics, the
watch-face style picker, and every settings page except Time, Date and Sleep.
With
WiFi/BLE gone the firmware no longer needs the `huge_app` partition, so it
builds on the default partition table (≈11 % of a 4 MB flash).

## Build & flash

```
pio run                       # compile
pio run -t upload -t monitor  # flash + open serial @ 115200
```

The hardware pin map and I2C addresses live in
[src/drivers/pins.h](src/drivers/pins.h) — identical to FirstOS.
