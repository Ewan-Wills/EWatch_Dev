# EWatch ShortsApps — PORT TO EWatchOS2.1 (read first)

Your app folder has been **re-scaffolded from the current `EWatchOS2.1`** (the
full shipping OS: watch faces, launcher, settings, WiFi, Spotify, games, etc.).
Your app's previous view code (`src/apps/<app>.*`), `pc/`, and `NOTES_baseos.md`
are already copied in. Your job: **register the app into OS2.1 and make it build
clean**, adapting the few APIs that changed from BaseOS.

Work ONLY inside your own `ShortsApps/<app>/`. Build with the isolated core:
```
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/<app>
~/.platformio/penv/bin/pio run -e ewatch        # MUST end in [SUCCESS]
```
Do NOT flash. Do NOT claim it works — only compilation is yours to verify.

## What changed from BaseOS → OS2.1 (the whole diff you care about)

**1. Registration moved to `src/apps/system/registry.cpp`** (not `view.cpp`). To
register an app with a `Screen::Foo` you add, in this order:
- `src/core/model.h`: add `Foo,` to the `enum class Screen` user-app region
  (near `Island,` — see the `BookLibrary`/`BookReader` lines already there as the
  worked example).
- `registry.cpp`: (a) `#include "foo.h"` with the other app includes; (b)
  `static FooView vFoo;` with the other `static …View v…;` instances; (c) a
  `case Screen::Foo: return &vFoo;` in `viewFor()`; (d) ONE launcher row in
  `kTopApps[]`: `{ "Foo", Icon::X, aura::kColor, Screen::Foo, nullptr },`.
  Valid icons (see `src/apps/system/ui_icons.h`): Stopwatch, Timer, Music, QR,
  Photo, Cube, Sparkle, Bird, Skull, Tunnel, Ship, Car, Paw, Island, Gear,
  Wrench, Info, Power, Moon, Sun, Torch, Wifi, Play, Pause, Next, Prev, Check,
  Cross, Clock, Vibrate, Memory. Colors: `aura::kBlue/kGreen/kOrange/kTeal/`
  `kPink/kPurple/kYellow/kRed/kLine` (`src/apps/system/ui_style.h`). Pick the
  closest icon; there's no perfect one for every app.

**2. Keep-awake moved to `src/core/power_mgr.cpp`** → `screenBlocksSleep(Screen)`
(NOT controller.cpp's blockSleep). If your screen must stay lit with no touch
input (streaming/animation/reading), add `case Screen::Foo: return true;` there.
See the `BookLibrary`/`BookReader` cases already added.

**3. Animation uses `desiredFrameMs()`, not constant `model.revision++`.** OS2.1's
render task only re-renders on events + a 1 Hz heartbeat UNLESS the view returns
`desiredFrameMs() > 0`. For continuous animation / streaming redraws, override it:
```cpp
uint16_t desiredFrameMs() const override { return animatingNow ? 33 : 0; } // ~30fps
```
(`animatingNow` = your condition: scrolling, playing, a live stream, a running
sim.) You can delete the old per-frame `model.revision++` hacks, or leave them —
but without `desiredFrameMs()>0` they will NOT produce smooth motion on OS2.1.
`ModelLock`, `model.revision`, `switchTo`, `theme()`, `ThemeColors`,
`drawTitleBar(title[,titleY,lineY,...])`, `tappedBack`, `frameCanvas()`,
`hapticBuzz` all still exist and work the same.

**4. Libraries already present in OS2.1** `lib_deps`: GFX, CST816S, QRCode,
ArduinoJson, JPEGDEC. **Do NOT re-add or vendor these** (BaseOS lacked JPEGDEC;
OS2.1 has it). `.pio/libdeps` is pre-populated for an offline build.

**5. WiFi is ON by default** and Spotify/Gallery/WiFi-settings depend on it. For
these ports **leave the OS intact — do NOT gate WiFi off** (the BaseOS size
optimisation is unnecessary now, and turning it off strips OS features). The goal
is "the real latest OS + your app added", a full standalone flash per app.

**6. Prefer self-init over editing `main.cpp`.** If your app needs a background
task (serial listener, poller), start it lazily & idempotently from your view's
`onEnter()` (see how bookreader calls `bookUploadStart()`), rather than wiring
into `main.cpp`. Only touch `main.cpp` if there is genuinely no other way, and
document it.

**7. Fonts** (`FreeSans12pt7b`, `FreeSerifBold12pt7b/24pt7b`, etc.) are on the
include path under `src/apps/assets/fonts/`. Same as before.

**8. Missing app-specific headers.** If your app `#include`s a config or secrets
header that didn't survive the re-scaffold (e.g. `printer.example.h`), recreate
it from `NOTES_baseos.md` + the spec. Secrets: provide `secrets/<x>.example.h`
(tracked) and build against a placeholder `secrets/<x>.local.h`; add `.local.h`
to `.gitignore`. `#if __has_include("<x>.local.h")` with a fallback keeps it
compiling with no secrets.

## Worked example to copy
`ShortsApps/bookreader/` is already ported and builds clean on OS2.1. Look at its
`model.h` enum lines, its `registry.cpp` edits (include / `vBookLibrary` /
`vBookReader` / viewFor cases / "Books" tile), its `power_mgr.cpp` sleep-block
cases, and the `desiredFrameMs()` override in `bookreader.h`. Mirror that.

## Deliverable
- `pio run -e ewatch` ends in `[SUCCESS]`, app reachable from the launcher.
- `pc/` kept working (unchanged unless the port required it).
- Update `NOTES.md` (you may base it on `NOTES_baseos.md`) to say it's now on
  OS2.1, note anything you changed in the port, and keep the exact
  flash+hardware-verify steps for Ewan. You may delete `NOTES_baseos.md` after.
- Report: compile status (final pio line), what you changed for the port, and
  anything stubbed/uncertain. Honesty over optimism.

## Hardening rules (2026-10-05, learned on bookreader; already in every OS2.1 tree)

- **Serial:** the ewlog debug console also reads `Serial`. If your app reads
  Serial, call `ewlogConsolePause(true)` in `onEnter` and `false` in `onExit`
  (and only read while paused). The RX queue is 4 KB and HWCDC silently drops
  bytes beyond it: for anything bigger than a few hundred bytes from the PC,
  send in <= 2 KB blocks and ACK each one. `Serial.flush()` after any reply
  the PC waits on.
- **Canvas:** `frameCanvas()` is a `SafeCanvas` (clips off-screen writes;
  GFX 1.4.9 text partly above/left of the screen otherwise writes outside the
  framebuffer). If you create your own canvas, use `SafeCanvas` too.
- **Locks:** never hold a mutex across a slow operation (a transfer, a big
  Serial write); the render task waiting > 20 s trips the task WDT.
- **Flashing:** the firmware pad-holds the power latch, so the watch flashes
  without holding the side button.
