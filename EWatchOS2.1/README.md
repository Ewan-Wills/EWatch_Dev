# EWatchOS 2.1 — the unified EWatch firmware

One firmware, every app. EWatchOS merges the whole EWatch catalogue onto the
FirstOS Fable core (tiered power management, ewlog, duty-cycled WiFi) under a
new GUI built to feel like a modern smartwatch: physics scrolling, slide
transitions, vector icons, a control centre, and five watch faces.

## Navigation

```
            swipe down                 swipe up
  Control Centre  <----  WATCH FACE  ---->  App Launcher
  (brightness,           swipe L/R          (all apps, momentum
   wifi, silent,         cycles faces        scroll, sections)
   torch, settings)
```

- **Swipe right** (or the side button) = back, everywhere.
- Games have on-screen exits (Doom, a module: hold the side button ~2 s).
- Long-press the side button 3 s = power off (global, unchanged).

## What's aboard

| Section | Apps |
|---|---|
| Apps  | Stopwatch, Timer, Bluetooth (media remote), QR Share, Photos, Media, 3D Viewer |
| Games | Tunnel Racer, Starfox (Island: compiled out) |
| Toys  | Particles, Flock |
| System| Settings, diagnostics, Power Off |

### Optional app modules

Bigger or niche apps are **modules**: one folder each in `src/apps/modules/`,
compiled only when a build asks for them. The standard build has none.

| Module | What |
|---|---|
| `doom` | Doom raycaster (owns the I2C bus while running) |
| `racer` | pseudo-3D OutRun-style racer (best run in NVS `racer`) |
| `pet` | virtual pet (tamagotchi) |
| `spotify` | Spotify Connect remote + web-page sign-in (also `env:ewatch_secrets`) |
| `wakesoak` | wake soak test + wake gesture traces; Mac side in `companions/wakesoak/` |

```
EW_APPS="doom pet" pio run -t upload      # any modules, either board env
EW_APPS="doom" ewt flash                  # same, via the ewt test tool
EW_APPS=all pio run                       # everything (CI / size check)
```
or put `custom_apps = doom pet` in an env that `extends = env:ewatch`.

**Adding a module:** put its sources in `src/apps/modules/<name>/`, guard its
hooks into the OS with `#if EW_APP_<NAME>` (registry entry + view, any
power-manager / console / init hooks), and add its `Screen` id to `model.h`
**unguarded** (ids are identical in every build; ewt reads that list).
Host-side tools for a module go in `companions/<name>/`. `tools/ew_apps.py`
does the rest. A module that has to change or remove OS core in a way this
tree shouldn't take is a fork instead: a long-lived `fork/<name>` branch that
merges `main` regularly.

Faces: **Aurora** (rounded-segment digits + complications), **Analog**,
**Orbit**, **Photo** (your photo, or a Claire Wills painting by default), **Mono**.

## Architecture notes

- `src/apps/system/ui_style.*` — design tokens (palette, radii, cards).
- `src/core/ui_motion.*` — easing + momentum/rubber-band scroll physics.
- `src/core/ui_transition.cpp` — snapshot slide transitions (2 PSRAM frames);
  views opt in via `View::paintTo()`. Non-participating views get a clean cut.
- `src/apps/system/launcher.*` — one list view drives Apps/System/Settings.
- Built-in ported apps live in `src/apps/games/` and `src/apps/island/`;
  optional ones in `src/apps/modules/` (above).
- Doom owns the I2C bus while running (`controllerSuspendIo()` — the same
  cooperative pause light sleep uses).
- Spotify pins a WiFi sync-window open while on screen (`wifiSvcHold`).

## Build & flash

```
pio run                  # build
pio run -t upload        # flash over USB
pio device monitor       # logs; `pwr stats`, `log dump` consoles available
```

Hardware target and pin map are unchanged from FirstOS Fable — see
[PINOUT.md](PINOUT.md). ESP32-S3FH4R2, 4 MB flash (huge_app), 2 MB PSRAM.

## Artwork

The default background is "Neist Point" by Claire Wills, © Claire Wills, all
rights reserved. It is not covered by the repository's MIT licence; see
[tools/default_bg/NOTICE.md](tools/default_bg/NOTICE.md).
