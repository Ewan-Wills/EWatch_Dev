# EWatchOS2.1 notes


## Split-out features (2026-10-09)
Doom, Racer, Pet, the Spotify integration (app, web sign-in, secrets env) and
the wake soak test (+ wake gesture traces, soak_pull.py, soak_logs) were
removed from this tree. Each lives in its own project folder = this OS as of
that day + that one feature: ../ewdoom ../ewracer ../ewpet ../ewspotify
../ewwakesoak (all built clean). Base flash 73.1% -> 66.3%. The raise check
keeps the MMA FIFO pre-trigger history (it is the turn reference); only the
soak/trace parts went. Sections below that describe the soak/traces now refer
to ../ewwakesoak.

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

## Bluetooth in the swipe-down menu (2026-10-08)
- Control Centre (swipe down from the face) has a Bluetooth tile (tiles now:
  WiFi / Bluetooth, Silent / Torch, Settings). On: the BLE media remote
  ("EWatch Remote") runs in the background on every screen; a prev / play-pause /
  next row appears at the top and the header shows Connected / Not paired (long
  buzz on tap when not paired).
- The on/off choice is saved in NVS (`bt`/`on`) and restored at boot (`btBegin()`
  in main.cpp). Opening the Bluetooth app turns it on (and saves that); leaving
  the app keeps it on; only the tile turns it off.
- While on: no real light sleep (fake sleep, like an active WiFi radio), WiFi is
  suspended (BLE needs its RAM), CPU pinned at 240 MHz: more battery drain.
- Ported from the `usbhost` tree (without its USB keyboard parts). Builds for
  ewatch + ewatch_v20 (72.6%). Test: `test/cc_bt.ewt`. NOT yet run on hardware
  (watch battery flat, 2026-10-07/08).

## Wake settings fixed + wake soak test (2026-10-08)
- Settings -> Sleep "Wake on" Touch / IMU now gate SCREEN-OFF (light sleep)
  wakes too; before, only deep sleep read them (raise-to-wake ran off the Wake
  page threshold alone, touch always woke). Raise-to-wake = IMU ticked AND
  threshold > 0. The button always wakes from screen-off (safety).
  NOTE: wakeOnImu defaults to false, so raise-to-wake is off until ticked.
- "Sleep -> power off (s)" now does what it says: after that long in
  screen-off the watch goes to deep sleep (wakes on the ticked sources, via
  reboot); 0 = never. T_DEEP (NVS pDeepMin, 45 min) is now deep -> full LDO
  power-off. Before, deep sleep only started after a hidden 45 min.
- Optional "Sleep touch hold" ms for touch out of the DARK screen (Settings ->
  Wake row 5, NVS tchSlpHold2). Default 0 = off: 350 ms made touch-wake too
  hard, so it was reverted to the old behaviour (key renamed so the saved 350
  is ignored). Screensaver dismissal unchanged ("Touch protect", 100 ms dwell).
- BUG FIXED: deep sleep entered from the screen-off loop with the light-sleep
  GPIO wake set still armed, so the RV-3028 minute tick rebooted the watch out
  of deep sleep (boot `reset=DEEPSLEEP wake=7` = GPIO). Looked like "turns on by
  itself every ~60 s" once deep sleep started at 60 s. Now the off loop
  disarms them, and enterDeepSleep()/reArmFromSpuriousWakeAndSleep() clear ALL
  wake sources before arming theirs. The re-arm path also used the old
  "sleepOff" meaning for its timer; it now uses T_DEEP (pDeepMin) like
  enterDeepSleep. Bench test on USB: `pwr set dtest 20` (RAM only, one shot)
  makes the NEXT screen-off deep-sleep after 20 s even on USB; watch the port.
  Buggy build: rebooted at once (wake=7). Fixed: port gone > 170 s.
- Deep sleep: never while USB is attached (looked like a crash: port vanishes,
  wake = reboot), and at least 60 s of screen-off whatever the setting says.
- Wake Soak app (`src/apps/wake_soak.*`): when on, every wake from screen-off or
  the screensaver is logged to LittleFS /wakesoak.csv (+ EWLOGI SOAK); touch and
  motion wakes prompt "Was that on purpose?" (hold YES 1 s / NO / 5 s timeout =
  not intentional; NO and timeout turn the screen straight back off);
  button wakes are logged as auto_yes; sleep touches rejected by the hold filter
  are logged as `rejected` with their held_ms. Console: soak on|off|stats|dump|
  clear|test (`soak test` injects a fake touch wake to try the prompt).
- Soak #1 result (2026-10-08, 2.4 h worn): motion woke it ~29/h, 63 of 69
  unwanted (jolt-to-wake: any >0.5 g move, no orientation check); the single
  accel sample at wake did NOT separate wanted from unwanted. Touch: 6 wakes,
  fine. Saved soak_logs/wakesoak-20261008-193415.csv.
- RAISE CHECK (power_mgr.cpp raiseCheck): on a motion wake the screen stays
  dark while the accel is polled every 20 ms for up to 1200 ms. Wake only if it
  settles (4 calm samples: change < 0.2 g/sample, |a| 0.7-1.3 g) with the
  screen facing up (-az >= 0.35 g, within 70 deg) AND >= 20 deg away from the last settled pose
  (baseline taken at screen-off, updated on each rejection). Otherwise
  re-sleep and log `sleep_motion_rejected` (gesture col = reason 1 not_facing,
  2 not_steady, 3 no_turn; held_ms = check time; ax/ay/az = the settled
  pose judged, or the last raw sample if it never settled).
  v4 (current): MMA at 200 Hz during the check; settled = 8 samples x 5 ms
  (change < 0.15 g/sample, |a| 0.6-1.4 g); facing (-ay - az)/sqrt2 >= 0.4 g;
  turn >= 10 deg; window 1.5 s. Ewan's Raise wake (g) setting was 0.76 g, so
  gentle raises never tripped the INT: set to 0.38 g (Settings -> Wake;
  the +/- steps wrap at the ends).
  SPEED: face pre-rendered into panel RAM (DISPOFF) on the render core while
  the check runs on core 0 (helper task; I2C vs SPI overlap). Every EWLOG
  line is a synchronous LittleFS append (tens of ms): wake-path logs now run
  after the backlight, and the wake backlight is set instantly (the fade
  logged first). wake_ms now ends at backlight-on. Live v3: 193-450 ms.
  v3 facing test: (-ay - az)/sqrt2 >= 0.55 g, i.e. anywhere from flat
  face-up to upright facing the eyes. v2 used -az only and rejected every
  real glance: Ewan holds it upright (ay -0.93 g, az -0.25 g).
  v1 (800 ms, 0.1 g calm, 60 deg) was too strict live: of ~5 real raises
  only 1 passed (not_steady x2, not_facing at ~50 deg) -> loosened to the above.
  Accepted motion wakes: gesture = 0, held_ms = time to accept.
  Console: `pwr raisetest [fresh]` (bench), `pwr set rchk 0|1` (RAM on/off).
  Axis fact: screen face up = az NEGATIVE (-4300 on a desk); the IMU
  diagnostics label was inverted ("face down" when face up), fixed.
  Bench (face up, still): baseline=now -> no_turn; fresh -> ok in 100 ms.
- PRODUCTION RAISE WAKE v5 (2026-10-09). Soak #2 (label mode, 62 motion
  triggers, 2 confirmed glances) showed v4 woke 48/50 unwanted. v5: turn is
  measured from the FIFO pre-trigger pose (oldest ~100 ms of the 640 ms
  history; FIFO armed at screen-off whenever raise wake is on), >= 25 deg,
  ending facing >= 0.8 g, settled 30 ms at 200 Hz; the first read after the
  rate switch is dropped (stale). Replay of the labelled traces through this
  logic: unwanted 7/60; the one replayable glance (small tilt from face-up,
  held still) wakes. Only 2 positives: re-tune when more glance data exists
  (a glance-cue collection mode was proposed).
  Defaults: Wake on IMU OFF (unchanged), raise trigger 0.38 g (was 0.5).
  Soak prompt OFF and label mode default OFF; the trace/soak code stays.
  Bench: `pwr raisetest` only runs while awake (wake first).
- GESTURE TRACES (src/apps/wake_gest.*, 2026-10-08): with the soak on, every
  wake stores an accel trace in LittleFS /wakegest.bin (cap 320 KB, ~300+
  wakes): pre = MMA8451 FIFO (circular, armed at screen-off) = 32 samples /
  640 ms BEFORE the trigger; base = the raise check's reference pose; chk =
  the check's samples at 100 Hz; post = ~1 s after the screen came on (I/O
  task reads). CSV row gets trace_id (16th column); answer = label.
  LABEL MODE (`soak label on|off`, default on): motion triggers wake and ask
  even when the check says no, so every trigger is labelled (gesture col =
  the verdict). Ignore/timeout counts as NOT intended.
  GOTCHA: MMA STANDBY->ACTIVE (FIFO arm, rate switch) restarts the HPF and
  fires a TRANSIENT: the first build woke itself ~40 ms after every
  screen-off. fifoArmQuiet(): arm, wait 150 ms, clear the latch. Verified:
  90 s on the desk, 0 triggers. `pwr fifotest` = bench read-back.
  Pull: tools/soak_pull.py -> wakesoak-*.csv + wakegest-*.csv (long format:
  trace_id,src,answer,verdict,check_ms,phase,t_ms,ax,ay,az) + raw .bin.
  Live test before traces (v4 raise check, 12 wakes): wake_ms median 180,
  min 65; overhead outside the check 2 ms (was ~65: synchronous log writes).
- Pull the log on the Mac: `python3 tools/soak_pull.py [--clear]` -> soak_logs/
  wakesoak-*.csv + per-source summary. Uses `soak get` (each line FNV-1a hashed,
  damaged lines re-asked). GOTCHA: Serial.flush() per line on HWCDC (core
  2.0.17) loses the START of every write; no flush + setTxTimeoutMs(1000) is
  clean. The n column carries on across reboots.
- Verified on the watch: real sleep_touch / sleep_motion / saver_touch /
  sleep_button wakes logged; `soak test` timeout -> pwr=O in 5 s; 3 pulls of
  the same log byte-identical.
- ewt false alarm: `log dump` replays old boots ("RTC mirror"), and ewt's crash
  regex matches those boot lines for the next calls -> "watch crashed" when it
  hadn't. Check `state` uptime against the flash time before believing it.
