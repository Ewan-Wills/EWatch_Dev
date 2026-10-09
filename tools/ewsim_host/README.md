# ewsim_host: offline EWatch app footage

Runs an EWatchOS2.1-family firmware on the Mac (no watch attached), drives it
with the same step scripts as `ewt`, and exports MP4s that look like
`ewt record` output: rounded panel corners, touch dots, an orange side-button
bar, crisp x4 pixels, optional 1080x1920 Shorts padding and a 6-frame contact
sheet.

```bash
cd ~/Documents/EWatch/EWatch_Firmware/tools/ewsim_host
./ewsimrec EWatchOS2.1 out/stopwatch.mp4 "goto Stopwatch; wait 600; tap 66 228; wait 3000; tap 66 228" --short
./ewsimrec EWatchOS2.1 out/tour.mp4 examples/tour.ewt --short
./ewsimrec ewcraft out/craft.mp4 examples/craft.ewt --short
./ewsimrec EWatchOS2.1 --screens                          # screen names for goto
./ewsimrec EWatchOS2.1 --snap out/snaps "goto Timer; wait 1000; snap timer"   # PNGs only
```

The first call for a project compiles it (about 4 s on an M4 Pro). Later calls
rebuild only what changed. A 20 s clip renders in 1 to 3 s of wall time.

## Prerequisites
- Xcode command line tools (`xcrun clang++`), Python 3, GNU make.
- ffmpeg + ffprobe with libx264 (Homebrew: `brew install ffmpeg`).
- The project's PlatformIO libraries must already be downloaded
  (`<proj>/.pio/libdeps/<env>/`). Run one `pio run` / `ewt build` for the project if
  they are missing. If they are missing, the build falls back to EWatchOS2.1's copy.
- The project must contain ewsim (`src/core/ewsim.cpp`; add it with
  `ewsim_install.py` from the Claude Knowledge Base).

Nothing here touches the watch, the serial port or the project's own `.pio/build`.
The firmware sources are compiled read-only from the project folder.

## Options
| option | effect |
|---|---|
| `--fps N` `--scale N` `--smooth` `--short` `--lead MS` `--hold MS` | same meaning as `ewt record` (defaults 30, 4, crisp pixels, no pad, 600, 1000) |
| `--time HH:MM[:SS]` `--date YYYY-MM-DD` | the watch's RTC at boot (default 2026-10-07 10:09:30) |
| `--pref NS.KEY=VAL` | seed an NVS preference before boot, e.g. `ewatch.wfStyle=1` (analog face), `ewatch.accColor=0xF800` |
| `--sleep` | keep the real dim (8 s), screensaver (10 s) and screen-off (15 s) timeouts. By default footage never dims or sleeps |
| `--fs DIR` | copy DIR into the simulated LittleFS (photos: `DIR/img/*.jpg`) |
| `--env ENV` | PlatformIO env to read build flags from (`ewatch`, `ewatch_v20`) |
| `--boot MS` | simulated boot time before recording starts (1500) |
| `--seed N` | PRNG seed for `esp_random()` / `random()` |
| `--allow-unsupported` | don't fail when a not-simulated (BLE) app is opened |
| `-v` | print the watch's console to stderr |

Steps are the ewt language: `goto NAME`, `tap X Y`, `dtap X Y`, `long X Y [MS]`,
`drag X1 Y1 X2 Y2 [MS]`, `swipe u|d|l|r`, `btn short|long|vlong|down|up|hold MS`,
`tilt GX GY GZ` / `tilt off`, `shake [MS]`, `wake`, `gest CODE [X Y]`,
`touch down X Y` / `touch up`, `wait MS`, `snap NAME`, `state`,
`expect screen|log|nolog|noerr|fps`, `keepcon on|off`, `raw LINE`, `show [RE]`.
There is one host-only step: `xcheck` compares ewsim's own bus-tap screenshot with the
panel model (they should match, 0 px). `golden` / `expect snap` are not supported.

Exit codes: 0 PASS, 1 a step or expect failed, 2 bad script or usage (checked
before the watch boots), 3 a not-simulated app was opened, 4 empty footage
(every frame one flat colour), 5 deadlock, 6 ffmpeg failed, 70 firmware panic
(assert, esp_restart, deep sleep), 71 crash (segfault: the console tail is printed).

## How it works
- **Real firmware, host shims.** Every `src/**/*.cpp` of the project is compiled
  with clang against `host/include` (Arduino.h, FreeRTOS, ESP-IDF, Wire,
  Preferences, LittleFS). That includes main.cpp, the controller, power manager,
  ewlog console, ewsim and every view. Only the radio modules are swapped for
  `host/stubs/` (wifi_svc, spotify net, bluetooth, xbox_pad, btmouse, ble_stack).
  The real libraries come from the project's `.pio/libdeps`: GFX Library for Arduino (core,
  canvases, ST7789 driver), JPEGDEC, QRCode, CST816S. The Arduino core's String,
  Print and Stream classes are vendored from arduino-esp32 2.0.17 (`vendor/`, LGPL).
- **The frame is the panel.** `Arduino_ESP32SPI` is replaced by a model of the
  ST7789 (CASET/RASET/RAMWR, DISPON/DISPOFF, SLPIN) holding 240x320 GRAM. The
  recorder films rows 20..299. Frames go black when the panel is off or the
  backlight PWM is 0, as a camera would see. ewsim's TapSPI still sits in front of it,
  so `xcheck` can show the two agree.
- **Deterministic clock.** FreeRTOS tasks are host threads, but only one runs at a
  time (`host/src/sched.cpp`). They switch only when they block: delay, a queue or
  semaphore wait, or suspend. When nothing is ready, the clock jumps to the next
  deadline. Panel transfers cost what they cost at 60 MHz SPI (17.9 ms per full
  frame) and I2C costs 400 kHz byte times. CPU time is free. A busy-wait guard
  advances 1 us per clock read and yields 1 ms every 2000 reads. Same script, same
  binary: same MP4, byte for byte (checked with md5).
- **Input goes through the real console.** The runner writes `sim <seq> tap 120 200`
  into the Serial RX buffer and waits for `S:<seq> OK`, exactly as ewt does over
  USB. The real ewlog console task parses it and the real ewsim injects it at the
  controller's 20 ms I/O loop. So Touch/TouchHold/TouchUp, CST816S gesture codes,
  swipe-right = back, ButtonShort timing and the ImuMotion heuristic are the
  firmware's own code. The overlays come from ewsim's finger and button state,
  sampled every 5 ms.
- **Emulated chips on I2C.** RV-3028 (time = `--time` + simulated clock),
  MMA8451 (flat, face up; `tilt` and `shake` override it as on the watch), CST816S
  (present, idle). The battery ADC reads 3.90 V (67 %). The USB host counts as
  plugged in, so the power manager uses its simulated sleep (the same as the watch on
  USB during `ewt record`).
- **Screen names** come from preprocessing the project's `model.h` with its own
  `-D` flags, so `#if` gates are honoured (Island off in EWatchOS2.1). Before each
  `goto`, the runner checks that the screen has its own view in this build and that the
  watch really switched to it.

## Supported (measured 2026-10-07)
- EWatchOS2.1: all 36 screens render. Bluetooth fails loudly (exit 3, NOT SIMULATED
  card). Spotify shows its real needs-WiFi screen (noted in the summary).
- ewcraft: Craft runs (raycaster + helper task, touch controls); Xbox Pad fails
  loudly; `goto Bluetooth` fails (no view in this build).
- flypet (FlyPet), stocks (Stocks), hidctl, macropad, llmterm, screenshare and ewmouse
  build and their own screens render. ewmouse's BT Mouse and Bluetooth fail loudly.
  Apps that wait for a PC show their idle screen ("Waiting for Mac...").
- Not buildable, with a clear error at build time: bookreader (no ewsim yet),
  homeassistant and printstream (they include WiFi.h in the app itself).

## Limits (read before using footage)
- No real radio. WiFi is always off, there is no NTP or web UI, the Spotify API is offline and BLE
  apps aren't simulated. No live data: stocks, LLM replies, the Mac screen share and
  Home Assistant only show their idle states.
- Sensors are simulated: flat accelerometer unless the script tilts or shakes it, fixed
  battery, RTC from `--time`.
- Timing is idealised. CPU time is free, so heavy renderers (Craft, Doom, the
  3D apps) run at the SPI-limited rate (Craft about 52 fps full-frame), which is
  probably faster than on the watch. Everything time-based (stopwatch, animations, physics dt)
  follows the simulated clock and is correct.
- Backlight dimming isn't shown. Off is black, any non-zero duty shows full brightness
  (the same as ewt's bus-tap footage).
- Fresh NVS and LittleFS every run (seed them with `--pref` / `--fs`).

## Layout
```
ewsimrec              entry point (build + run + ffprobe line)
Makefile              make PROJ=<proj> [ENV=ewatch]  -> build/<proj>/ewsim
tools/genconfig.py    reads platformio.ini, lists sources, swaps stubs, generates screens + config.mk
tools/build.mk        compile/link rules
host/include/         shim headers (Arduino, FreeRTOS, ESP-IDF, Wire, FS, GFX umbrella, panel)
host/src/             sched.cpp, arduino.cpp, wire.cpp, storage.cpp, panel.cpp, runner.cpp
host/stubs/           host replacements for radio modules
vendor/arduino-core/  String/Print/Stream/IPAddress from arduino-esp32 2.0.17
examples/             tour.ewt, stopwatch.ewt, craft.ewt
build/                per-project objects + binary (git-ignored)
```
Adding a project-specific radio module: drop `host/stubs/<file>.cpp` that implements
its header and add its `src/`-relative path to `STUBBED` in `tools/genconfig.py`.
For a view that can't work, call `ewsimHostUnsupported("...")` in `onEnter()` and
draw `ewsimDrawUnsupported()` so the run fails loudly.

Docs: Claude Knowledge Base `firmware/knowledge/ewatch-host-simulator.md` (design,
gotchas) and `video-editing/knowledge/offline-ewatch-app-footage.md` (for editors).
