# ewspotify — EWatchOS 2.1 + Spotify

Split out of `../EWatchOS2.1` on 2026-10-09: this is the OS as it was then,
with the Spotify Connect remote: now-playing app, web-page Client ID / sign-in, and the opt-in `ewatch_secrets` env (secrets/).
The other four features split out that day are not in this tree. Everything below is the base OS README.

---

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
- Games: hold the side button ~2 s to leave Doom; other games have on-screen exits.
- Long-press the side button 3 s = power off (global, unchanged).

## What's aboard

| Section | Apps |
|---|---|
| Apps  | Spotify remote, Stopwatch, Timer, QR Share, Media, 3D Viewer |
| Games | Doom, Tunnel Racer, Starfox, Racer, Pet (tamagotchi), Island |
| Toys  | Particles, Flock |
| System| Settings, diagnostics, Power Off |

Faces: **Aurora** (rounded-segment digits + complications), **Analog**,
**Orbit**, **Photo** (your photo, or a Claire Wills painting by default), **Mono**.

## Architecture notes

- `src/apps/system/ui_style.*` — design tokens (palette, radii, cards).
- `src/core/ui_motion.*` — easing + momentum/rubber-band scroll physics.
- `src/core/ui_transition.cpp` — snapshot slide transitions (2 PSRAM frames);
  views opt in via `View::paintTo()`. Non-participating views get a clean cut.
- `src/apps/system/launcher.*` — one list view drives Apps/System/Settings.
- Ported apps live in `src/apps/games/`, `src/apps/spotify/`, `src/apps/island/`.
- Doom owns the I2C bus while running (`controllerSuspendIo()` — the same
  cooperative pause light sleep uses). Racer's best run lives in NVS `racer`.
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
