# ClaudeFace firmware

Stand-alone EWatch v2 firmware that is only the **Claude Code face**: agent
state, 5h/7d usage rings, the primary session's turn timer and tool, other
sessions, fed over USB-C serial by the host bridge (`../bridge`). It is drawn
landscape: the watch sits on its side at the desk with its **top edge pointing
right**.

No launcher, no other faces, no WiFi, no sleep. AuroraOS no longer contains
the Claude face; this folder is the only copy. Protocol, bridge, hooks and the
overall design are in `../docs/claude-face.md`.

| Input (viewer's frame) | Does |
|---|---|
| tap | ACK, when the primary session is DONE (unacked) or anything is BLOCKED. Closes the detail page if it is open |
| long press (0.7 s) | detail page on / off |
| swipe left | next session (SELECT_SESSION) |
| swipe right | previous session |
| swipe up | open the detail page |
| swipe down | nothing |
| any swipe, detail page open | close it |
| side button, short press (< 1 s) | detail page on / off |
| side button, hold 3 s | power off |

- Session swipes need a live bridge and at least two sessions; otherwise they
  do nothing. The detail page closes itself after 30 s.
- Touch points and CST816S swipe directions arrive in native portrait panel
  terms and are rotated first (`src/cf_orient.h`, `cfChipGestureToPanelDir`
  in `src/cf_policy.h`, which uses AuroraOS' names for the chip's gesture
  ids). The full native-to-viewer table is at the top of `src/claude_face.cpp`.
- Buzz patterns: BLOCKED 3 short (repeats every 60 s, max 5, until ACK or
  resolved), a turn over 60 s DONE 1 medium, 5h window crossing 80 / 95 %
  2 short, leaving LIMITED on a reset 1 short. Quiet hours make them
  visual-only.

## Power

- The LDO latch goes HIGH on the first line of `setup()` and is pad-held
  (`gpio_hold_en`), so a flash, USB chip reset, panic or watchdog reset can't
  drop the rail. Only the 3 s side-button hold releases it.
- Power-off on battery: the rail drops and the watch is off; press SW2 to
  start it again. On USB the board stays powered, so the watch goes dark and
  waits: hold SW2 for 1 s and it restarts.
- A press that is still down from powering on is ignored until it is
  released, so holding SW2 a little long at power-on can't turn it straight
  back off.
- No sleep of any kind. The screen never turns off: 100 % for 10 s after a
  visible state change, a touch, the button, a wrist raise (MMA8451 jolt) or
  boot, then the ambient level (default 15 %). A BLOCKED session forces
  100 % while the bridge is live. With no bridge frame for a while it simply
  stays at ambient, as a clock.
- The clock always comes from the RV-3028, so it works as a watch with no
  bridge. The bridge's TIME frame sets it (local = UTC epoch + offset).
- Task watchdog (20 s) on the UI loop, the serial link task and the haptic
  task; a hang reboots, and the latch hold keeps the watch on through it.

## Console (same USB serial as the frames)

```
cf stats                                   link / frame / haptic counters
cf set quiet <start 0-23> <end 0-23>       quiet hours (start == end disables)
cf set ambient <0-100>                     ambient backlight %
cf set haptics <0|1>
cf buzz <blocked|done|cross|reset>         test a pattern (ignores quiet hours)
cf hello                                   send a HELLO frame
```
Settings live in NVS namespace `claudeface`. On the first boot they are copied
from AuroraOS' `ewatch` namespace if it has them, so quiet hours, ambient and
the UTC offset carry over.

## Build / flash
```bash
~/.platformio/penv/bin/pio run                       # build
~/.platformio/penv/bin/pio run -t upload && ~/.platformio/penv/bin/pio device monitor
```
The Claude Face bridge (`launchd com.ewills.claude-bridge`) holds the watch's
serial port: `launchctl bootout gui/$(id -u)/com.ewills.claude-bridge` first,
`launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.ewills.claude-bridge.plist` after
(or `curl -X POST '127.0.0.1:7337/serial/release?seconds=120'` for a short
release). The latch hold should keep the watch powered through the reset
without holding the button (verified on the AuroraOS latch-hold build on
2026-09-23, not yet on this firmware).

Same pins as AuroraOS / HeadFace: espressif32@6.13.0 (arduino-esp32 2.0.17,
gnu++11), GFX Library for Arduino ~1.4.7, CST816S. `src/pins.h` is a verbatim
copy of AuroraOS' `src/drivers/pins.h`. To go back to AuroraOS, flash it from
`../../AuroraOS` the same way.

## Host checks (no watch needed)
```bash
test/claude/run.sh        # protocol + haptic logic (289 checks), mutation self-tests, then render.sh
test/integration/run.sh   # snapshot double buffer, orientation, gesture table, button, backlight, time
test/render/render.sh     # every face state -> test/render/out/*.png + contact_sheet.png
test/xcheck/run.sh        # frames from the real TS bridge encoder parse in the C parser
```
Each suite also runs deliberately broken builds or inputs and fails if they
pass. `render.sh` and the render part of `claude/run.sh` compile the real
Arduino_GFX sources from `.pio/libdeps`, so run `pio run` (or
`pio pkg install`) once first. `xcheck` needs `npm install` in `../bridge`.

## Files
- `src/main.cpp` power latch, setup, the UI loop (touch, button, RTC, accel, battery), power-off
- `src/board.*` minimal drivers taken from AuroraOS: display + backlight, DRV2603, CST816S, RV-3028, MMA8451, battery
- `src/serial_io.*` Serial write mutex, logging, the link / console task
- `src/settings.*` NVS settings
- `src/claude_face.*` the face: haptics, backlight policy, dirty-region rendering, input
- `src/cf_link.*` `#CF` frame handling, double-buffered snapshot, HELLO / EVENT, `cf` commands
- `src/claude_face_draw.*`, `src/cf_orient.h`, `src/fonts/` pure landscape drawing (host-testable)
- `src/cf_logic.*`, `src/cf_util.h`, `src/cf_policy.h` pure protocol / haptic / input / backlight logic (host-testable)
- `src/cf_protocol.h` generated by `../protocol/gen.py`, do not edit
