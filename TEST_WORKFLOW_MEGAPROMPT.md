# MEGAPROMPT — Autonomously test & verify the EWatch apps (ewt harness)

Paste this whole block to an autonomous agent/session to run the pass. It verifies,
on the REAL EWatch, that each EWatchOS2.1 app boots, navigates, animates where
expected, and runs crash-free — using only the `ewt` automation. It produces a
report + snapshots and updates each app's NOTES. This is first-person hardware
verification; report only what `ewt` actually proves.

---

You are verifying eight EWatchOS2.1 apps on the real EWatch over USB using the
`ewt` test harness. Do them ONE AT A TIME (single watch). Do not guess results —
run each step and read what `ewt` prints.

## Tooling & environment
- `EWT="python3 $HOME/Documents/Claude\ Knowledge\ Base/firmware/knowledge/ewt.py"`
- `export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps`
  (isolated toolchain — the shared ~/.platformio has the wrong arduino-esp32 and
  will fail the build; see firmware/knowledge/ewatch-isolated-pio-core-vs-v3-fork.md).
- `export EWT_WHO="apptest"` so the lock holder line identifies you.
- The watch is a **V2.0** board; `ewt` auto-selects the env from the board MAC —
  never pass `-e`. `ewt flash <app>` is autonomous: it builds (errors only),
  flashes with retries, and waits for boot. No button-hold needed.
- The `ewt` skill reference is already enough; do NOT read firmware to learn the
  input system. Full command/step list: the `ewatch-test` skill /
  firmware/knowledge/ewatch-test-harness.md.

## Coordination (shared single watch — REQUIRED)
- Only one `ewt` drives the watch at a time (lock `/tmp/ewt-watch.lock`). Other
  Claude sessions on this Mac use the watch too (e.g. "Bluetooth MP3 player in
  ewatch"). Before you start, confirm the watch is free (a busy `ewt` prints who
  holds it); if busy, use `ewt ... --wait SECS` or message the holder and wait.
- Never leave a loop running that auto-flashes. When finished, leave the watch on
  a known screen (`goto Watch`).

## Apps in scope (top-level under ~/Documents/EWatch/EWatch_Firmware/)
screenshare, hidctl, stocks, homeassistant, printstream, macropad, llmterm, flypet.
- **bookreader** (ShortsApps/bookreader) is ALREADY verified — skip it.
- **llm_ondevice** is a bare llama2.c sketch with NO ewsim — handle it separately
  at the end (see last section), not with the ewt app loop.

## KEY LIMITATION — serial-port contention (state this in the report)
`ewt` owns the USB serial port while driving the watch. The serial-driven apps
(screenshare, stocks, macropad, llmterm, hidctl, flypet) take their live PC data
over that SAME port, so you CANNOT exercise their live data path under `ewt`.
Verify only: the app boots, shows its idle/"waiting" state, is navigable, and
stays crash-free (plus flypet animates). Record explicitly that the live PC-data
path was NOT exercised (it needs the app's `pc/` companion, run separately with
`ewt` NOT holding the port).

## Per-app smoke tests
For each app: write `<app>/test/smoke.ewt` with the steps below, then run it.
`ewt run` resolves the file path relative to the CURRENT directory — so either
`cd <app>` first and `ewt run test/smoke.ewt`, or pass the full path. Screen enum
names are in (parentheses); `ewt -P <app> screens` lists them if unsure.

- **screenshare** (Screenshare): `goto Screenshare; expect screen Screenshare; wait 800; expect noerr; snap boot`
- **hidctl** (HidCtl — tile "Notes"): `goto HidCtl; expect screen HidCtl; tap 120 150; wait 300; expect screen HidCtl; expect noerr; snap boot`
- **stocks** (Stocks): `goto Stocks; expect screen Stocks; wait 600; swipe l; wait 400; expect screen Stocks; expect noerr; snap boot`
- **homeassistant** (HomeAssistant — "Home"): `goto HomeAssistant; expect screen HomeAssistant; wait 1200; expect noerr; snap boot` (no WiFi/token configured in test → expect a "connecting/unavailable" state; smoke = boots, no crash)
- **printstream** (PrintStream — "Printer"): `goto PrintStream; expect screen PrintStream; wait 1200; expect noerr; snap boot` (no printer/WiFi config → stats-waiting; smoke = boots, no crash)
- **macropad** (Macropad): `goto Macropad; expect screen Macropad; tap 60 120; wait 300; expect screen Macropad; expect noerr; snap boot`
- **llmterm** (LlmTerm — "LLM Term"): `goto LlmTerm; expect screen LlmTerm; tap 120 120; wait 500; expect screen LlmTerm; expect noerr; snap boot` (no PC bridge → no tokens; must not crash)
- **flypet** (FlyPet): `goto FlyPet; expect screen FlyPet; expect fps 10; wait 1500; tap 120 140; wait 400; expect screen FlyPet; expect noerr; snap boot` (animates standalone; the `expect fps 10` proves the animation loop runs)

Tune tap coordinates to the actual layout if an `expect screen` fails because a tap
left the app — Read the snap to see where controls are, then adjust. Screen is
240×280, origin top-left.

## Procedure (per app, sequential)
1. `$EWT flash <app>` — if it prints build errors, fix them in `<app>/src` and
   re-flash until it boots.
2. Write `<app>/test/smoke.ewt` (steps above).
3. `cd <app> && $EWT run test/smoke.ewt` (or pass the full path).
4. First run creates goldens for any `expect snap`; Read each `boot` snap once
   (`<app>/.ewt/boot.png`, ~90 tokens) to confirm it looks right, then it guards
   regressions for free.
5. Record: app · flash ok? · smoke pass/fail · fps (flypet) · snap path · any
   error line + decoded backtrace on failure.

## Output
- A results table across all 8 apps.
- Update each `<app>/NOTES.md` with a "Verified on hardware (ewt)" line: date,
  what passed (boot/nav/stability, flypet fps), and the explicit note that the
  live PC-data path was NOT exercised under ewt.
- Surface the snapshots (and optionally `ewt record <app>/demo.mp4 "<steps>" --short`
  for any you want as Shorts b-roll).
- HONESTY: report only what `ewt` proved. Do NOT claim the PC data path works. If
  an app crashes or won't build, report the failure + backtrace — do not paper over
  it. Leave the watch on `goto Watch` when done and release the lock.

## llm_ondevice (separate — no ewsim)
It's a standalone llama2.c sketch, so `ewt`'s `goto`/`state`/input won't apply.
Verify it by flashing with the firmware-root flasher and reading its serial:
`python3 ~/Documents/EWatch/EWatch_Firmware/flash.py llm_ondevice` (hold the button
through the upload if prompted), then capture the boot serial for the
`-- done: N tokens, X tok/s --` line. Already verified once at **14.73 tok/s**;
only re-run if asked. It must call `latchPower()` first in setup() (it does) or the
watch powers off on button release.
