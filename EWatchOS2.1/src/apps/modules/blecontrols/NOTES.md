# blecontrols module notes

Carried over from the stand-alone `blecontrols/` folder it was developed in
(before 2026-10-09). OS-wide parts of these notes may be out of date.

---

# blecontrols — EWatch "Controls" app (ewatch.cloud control panel over BLE)

Copy of EWatchOS2.1 (feat/bluetooth-app, Island compiled out) plus one app:
`src/apps/blecontrols.{h,cpp}`, `Screen::BleControls`, launcher row "Controls".
Web side: EwatchCloud worktree `EwatchCloud-ble`, branch `feat/ble-controls`
(`ewatch-frontend/src/foundation/ble.jsx` + `src/screens/controls.jsx`, nav "Controls").

## Protocol v1 (keep in sync with ble.jsx)
- Service `7e0a0001-5c3b-4c8e-9a6f-2d1e0c0b0a01`, advertised as "EWatch Controls"
  only while the Controls screen is open (radio fully off otherwise).
- CMD `...0002` write / write-no-response: UTF-8 `<id>=<value>`, several joined by
  `\n`, <= 180 bytes. id: 1-19 of `[A-Za-z0-9_]`. Watch keeps the last 16 ids.
- EVT `...0003` read/notify: `hello app=blecontrols v=1` (on connect),
  `ack <id>` (every command), `evt side=1` (side button), `evt tap=<id>` (row tap).
- Reserved ids handled on the watch: `buzz=<ms>`, `bl=<5-255>`, `msg=<text>` (3 s
  full-screen), `clear=1`. Everything else is shown for the app to use.
- BTC-task callbacks only copy into a queue; parse/draw/notify on the render task.
- No pairing/bonding (v1): anyone in range can write while the screen is open.

## Testing
- `ewt flash blecontrols`, `ewt run test/smoke.ewt` (advertise, back -> deinit, re-init).
- Local web: preview config `web-ble` (port 8220, proxies /__api to :4000,
  /__testlogin for the local test admin). "Simulated watch" transport needs no hardware.

---

# EWatchOS2.1 notes

## 2026-10-05 - serial/render trap audit (traps A-E from bookreader)

Changed:
- Root fix for top/left text clipping (all views): GFX 1.4.9 drawChar, both
  GFXfont and the built-in glcdfont, only skips a glyph that is entirely
  off-screen; a partly-off glyph writes at negative x/y, before the canvas
  framebuffer (heap corruption). `src/drivers/safe_canvas.h` (SafeCanvas)
  clips every canvas write path; frameCanvas() and viewer3d use it. The audit
  found quickset's scrolling header (y=36..40+rise) hitting this; it needs no
  local guard now.
- `src/core/ewlog.cpp` `log dump`: held sLogMutex across the whole 256 KB
  Serial dump. With a host that isn't draining, each HWCDC write can block
  ~100 ms, so every task that logs would stall (trap E). The mutex is now
  taken per 256 B chunk (open/seek/read) and released before Serial.write.

Audited, no change needed:
- Trap A/B/C: the only Serial reader in the OS is the ewcon console itself.
  No OS view runs a PC protocol over Serial (photos upload via the web UI).
- Trap D, custom fonts: watchface Photo face (FreeSansBold24, baseline 96,
  top 61) and Mono face (FreeMono24, baseline 120) are in bounds;
  drawTitleBar draws FreeSans12/FreeMono12 at baseline >= 24 on the panel
  (gfx, not the canvas). Launcher already guards its scrolling text.
- Trap E: other locks (ModelLock, spotify art/spMutex, photo_store,
  wifi scan/job, storage known-list) are held only for short copies; the
  spotify art download runs outside artMutex.
Not flashed or tested on hardware.
