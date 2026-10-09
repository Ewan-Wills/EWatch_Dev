# EWatch ShortsApps — shared build primer (read first, every agent)

You are building **one** standalone EWatch app for a batch of short-form-content demos.
Each app lives in its own folder under `EWatch_Firmware/ShortsApps/<app>/` and is a
**full, self-contained firmware flash** (its own copy of BaseOS) plus, where noted, a
**PC-side companion script** in `<app>/pc/`.

Do not touch any other app's folder. Do not edit anything outside your own
`ShortsApps/<app>/` folder except to read reference apps.

---

## 0. Hardware (fixed — never assume otherwise)

| Item | Value |
|---|---|
| MCU | ESP32-S3FH4R2 — 4 MB flash, 2 MB QSPI PSRAM, **native USB (CDC)** |
| Display | ST7789 **240 wide × 280 tall**, RGB565, col offset 0 / row offset 20 |
| Touch | CST816S capacitive, I2C @ 400 kHz, addr 0x15 |
| IMU | MMA8451 accel · RTC | RV-3028 · Haptic | DRV2603 |
| Radio | **BLE only — NO Bluetooth Classic / A2DP** |
| Power | soft-latch LDO, no hard switch; `latchPower()` is first line of setup() |

- The chip is **BLE only**. No classic Bluetooth, no A2DP, no Bluetooth audio.
- There is **no audio output hardware** (only a haptic vibration motor).
- WiFi TX power is board-specific and already handled by the firmware
  (`src/drivers/hw_rev.h`): default build env is **V2.1**.

## 1. How an app is structured

Every existing app (`BaseOS`, `Spotify_Controller`, `tamagotchi`, …) is a complete
PlatformIO project. **The canonical app-authoring guide is `BaseOS/CLAUDE.md`** —
READ IT before writing code. The recipe in short:

1. Scaffold your project by copying BaseOS (command below).
2. Write a `View` subclass in `src/apps/<app>.{h,cpp}`.
3. Register it in **4 marked sites** (`// <- add ...` markers):
   - add a `Screen::<App>` enumerator in `src/core/model.h`
   - `#include`, `static <App>View v<App>;`, `viewFor()` case, and a `kTopApps[]`
     launcher tile in `src/apps/system/view.cpp`
4. Build. Flip off unused subsystems per BaseOS/CLAUDE.md §0 (e.g.
   `-DEWATCH_ENABLE_WIFI=0` if you don't network).

### Scaffold command (run once, from EWatch_Firmware/)
```sh
rsync -a --exclude node_modules --exclude .pio --exclude .git \
  BaseOS/ ShortsApps/<app>/
```

## 2. Build (prove it compiles — this is your acceptance bar)

**CRITICAL — use the isolated PlatformIO core dir.** The shared `~/.platformio`
on this machine currently holds the arduino-esp32 **v3** fork (another project,
laya-train, needs it). Building against it FAILS (GFX 1.4.7 vs v3 `spi`
signature). A dedicated core with the correct **arduino-esp32 2.0.17** is already
installed here — always export it, and never touch `~/.platformio`:

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
PIO=~/.platformio/penv/bin/pio
cd ShortsApps/<app>
$PIO run                 # env:ewatch (V2.1). MUST succeed with no errors.
```
Your folder is pre-scaffolded from BaseOS with `.pio/libdeps` already populated,
so the first build compiles offline against the shared core (~1–2 min).
- **Do NOT flash** (`-t upload`) and do NOT claim the app "works" — only Ewan
  verifies on real hardware. Your deliverable is: **compiles clean** + code that
  is honestly complete for an MVP + a `NOTES.md`.
- `pio` is not on PATH; always use `~/.platformio/penv/bin/pio`.
- macOS has no `timeout`; don't use it.

## 3. Drawing / input / state (see BaseOS/CLAUDE.md §§5–8 for detail)
- Global `Arduino_GFX *gfx` (null-check it). Screen 240×280. RGB565 `uint16_t`.
- Read the theme: `ThemeColors t = theme();` — don't hardcode a palette.
- Full-screen animation: `Arduino_Canvas *cv = frameCanvas(); … cv->flush();`
  (lazy 240×280 PSRAM buffer; returns nullptr if PSRAM exhausted — handle it).
- Fonts in `src/apps/assets/fonts/` (FreeSans/Serif/Mono 12 & 24pt).
- Input via `onEvent(const Event&)`: `Touch`, `TouchHold`, `TouchUp`, `Gesture`
  (`e.gesture` = SwipeUp/Down/Left/Right/SingleTap/DoubleTap/LongPress),
  `ButtonShort` (= BACK; right-swipe maps here too — always handle it), `Tick`
  (1 Hz). Hit-test `e.x`/`e.y` manually.
- Shared state is the mutex-guarded global `model`; `{ ModelLock lk; … }`, keep
  the lock scope tiny, never draw while holding it.
- **Only `taskIO` may touch I2C/`Wire`.** From a view, read `model`; don't poll
  sensors directly.
- Haptic feedback on meaningful taps: `hapticBuzz(intensity, ms)` (safe anywhere).
- Watchdog: keep `render()`/`onEvent()` short; no long `delay()`/busy loops.

## 4. Serial (USB CDC) — companion-app convention
Native USB CDC is already on (`ARDUINO_USB_CDC_ON_BOOT=1`). Use `Serial` @115200.
Because the USB serial is also the flashing/monitor channel, pick a convention
and document it in your NOTES.md + pc/README. Recommended protocol:

- **Text/control:** newline-terminated ASCII commands, one per line
  (`CMD arg1 arg2\n`). Prefix watch→PC telemetry lines with a sentinel like
  `>>` so the PC parser ignores boot/log noise.
- **Binary/bulk (images, files):** a tiny framed protocol — magic byte(s),
  length (uint32 LE), payload, optional CRC. Drain to frame boundaries.
- Baud 115200 works everywhere; native USB CDC ignores baud and runs at USB FS
  speed, so bulk transfer (screenshare, file upload) is fine.

PC scripts: Python 3, `pyserial`. Put a `pc/requirements.txt` and a
`pc/README.md` with exact run steps. Auto-detect the port where sensible
(`/dev/cu.usbmodem*` on macOS) and accept `--port` to override. Keep each app's
PC script fully inside its own `pc/` folder — no shared PC code between apps.

## 5. Deliverable checklist (per app)
- [ ] `ShortsApps/<app>/` is a BaseOS copy that **`pio run` compiles clean**.
- [ ] App registered in the launcher (4 sites) and reachable (swipe up → tile).
- [ ] Unused subsystems trimmed/gated per BaseOS/CLAUDE.md §0.
- [ ] `pc/` companion (if the spec has one): script + requirements.txt + README.
- [ ] `ShortsApps/<app>/NOTES.md`: what it does, the serial/network protocol,
      how to run the PC side, what's MVP-stubbed, known risks, and **exactly
      what Ewan must do to verify on hardware** (flash cmd + what to look for).
- [ ] Be honest about anything you could not compile-verify or had to stub.

## 6. Non-negotiables
- Never remove/alter `latchPower()`; never call `unlatchPower()` in app logic.
- Never hardcode a pin — include `pins.h`, use `PIN_*`.
- Don't invent hardware the watch doesn't have (no audio out, no BT Classic).
- Report honestly. "Compiles" ≠ "works"; only say what you verified.
