# EWatch firmware

One OS, optional app modules, a few long-lived forks, and the history on
archive branches.

```
EWatchOS2.1/                 the OS (the only firmware on main)
  src/apps/modules/<name>/   optional apps, compiled only when asked for
  companions/<name>/         Mac-side tools for a module (bridges, uploaders)
  test/                      ewt test scripts (test/<module>... need that module)
tools/
  ewsim_host/                offline simulator / video renderer (ewsimrec)
  flash.py                   flashing helper
  hwtests/                   hardware bring-up tests
```

## Build

```
cd EWatchOS2.1
pio run -e ewatch_v20 -t upload                   # standard OS (V2.0 board; ewatch = V2.1)
EW_APPS="doom books" pio run -e ewatch_v20 -t upload   # with modules
EW_APPS=all pio run                               # everything (size / CI check)
```

`ewt flash` (the test tool) honours `EW_APPS` the same way. An env can pin a
set instead: `custom_apps = doom books` in an `[env:...]` that
`extends = env:ewatch`.

## Modules

| Module | App |
|---|---|
| `blecontrols` | ewatch.cloud control panel over BLE |
| `books` | .txt library + reader (uploader in companions/books) |
| `claudeface` | Claude Code face fed by the Mac bridge (companions/claudeface) |
| `doom` | Doom |
| `flypet` | virtual-fly pet (Mac brain in companions/flypet) |
| `hidctl` | Notes editor + cursor fed over serial |
| `homeassistant` | Home Assistant light toggle (WiFi/REST) |
| `livestream` | live frames from a Mac over WiFi |
| `llmterm` | terminal for a Mac-hosted LLM |
| `macropad` | touch macro grid -> Mac |
| `motion` | on-device motion language model (gesture actions) |
| `mouse` | BLE mouse: global cursor, click/drag, scroll |
| `pet` | tamagotchi |
| `printstream` | 3D-printer camera + stats |
| `racer` | OutRun-style racer |
| `screenshare` | mirror a Mac window to the watch |
| `spotify` | Spotify Connect remote |
| `stocks` | live stock tracker (feed from the Mac) |
| `usbhost` | USB host scanner + USB keyboard for the BT remote |
| `wakesoak` | wake soak test + gesture traces (pull: companions/wakesoak) |
| `write` | type by drawing letters |

How to add one: `EWatchOS2.1/README.md` ("Optional app modules").

## Branches

- `main`: the OS + modules. OS fixes land here first.
- `feat/*`: work in progress; merged into main or deleted.
- `fork/<name>`: an app that has to change OS core in a way main shouldn't
  take. Merge `main` into it regularly. Today: `fork/ewcraft` (Craft + Xbox
  pad on NimBLE, which can't link next to the OS's Bluedroid).
- `archive/<name>`: frozen earlier projects, each at the root of its branch
  so it still checks out and builds: `firstos`, `firstos-fable`, `baseos`,
  `ewatchos21-with-old-apps`, `doom-original`, `3d-racer`, `3d-watchface`,
  `basic-analog`, `basic-digital`, `gallery-face`, `head-face`, `tank-face`,
  `scrubmarine`, `spotify-controller`, `starfox-shooter`, `tunnel-racer`,
  `tamagotchi`, `llm-ondevice`, `shortsapps`, `claudeface-standalone`.
- `snapshot/2026-10-09`: everything on disk before the repo rework.
