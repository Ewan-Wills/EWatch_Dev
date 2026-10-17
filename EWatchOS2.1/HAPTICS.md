# EWatch haptics guide

How to give an app haptic feedback, and which feel goes with which use.

**Short version:** call `haptic(HapticRole::Something)` with the role that matches *what just
happened*. Don't pick an effect or a raw strength/ms yourself. One table maps every role to a
feel, so the same event feels the same in every app, and the whole watch is retuned in one place.

```cpp
#include "haptic_roles.h"

haptic(HapticRole::Tap);          // a button was pressed
haptic(HapticRole::Confirm);      // saved
haptic(HapticRole::Collect);      // picked up a coin
```

| file | what it is |
|---|---|
| `src/core/haptic_roles.{h,cpp}` | **the role table**: use case -> effect. Edit here to retune |
| `src/drivers/haptic.{h,cpp}` | the waveform engine and the 24 effects |
| `src/core/haptic_bench.cpp` | `hap ...` console: try roles and effects, accelerometer meter |
| `src/apps/system/haptic_lab.cpp` | **Apps -> Haptic Lab**: feel every effect on the watch |

Console (USB, `ewt raw "..."`): `hap roles` prints the live table, `hap role Back` plays a role,
`hap play Swell` plays an effect, `hap meter Click` measures one.

---

## 1. The role table

Effect strengths are what the watch's own accelerometer measured on a V2.0 board at 70 %
strength (2026-10-16). "Stops in" is how long the motor runs on after the drive ends.

### Navigation and controls
The user caused these and is looking at the screen, so keep them short and quiet.

| role | effect | use for | strength |
|---|---|---|---|
| `Tap` | Click | button or tile press | ~290 mg, 60 ms |
| `Open` | Click | open an app, item or detail view | ~290 mg |
| `Back` | Tick | back, close, dismiss | ~150 mg, 50 ms |
| `Scroll` | Tick | list detent, page flip, carousel step | ~150 mg |
| `Adjust` | Tick | +/- step, slider notch, held ramp repeat | ~150 mg |
| `ModeChange` | DoubleClick | switch mode, tab, view style, watch face | 2 x ~290 mg |
| `ToggleOn` | ToggleOn | switched on, started, armed (rising pair) | ~440 mg |
| `ToggleOff` | ToggleOff | switched off, stopped, cancelled (falling pair) | ~400 mg |
| `LongPress` | LongPress | a hold was recognised (swell into a click) | ~900 mg |
| `Destructive` | Thud | delete, clear all, reset, after confirming | ~410 mg, deep |

### Results of an action

| role | effect | use for |
|---|---|---|
| `Confirm` | Success | saved, sent, applied (soft then firm) |
| `Refused` | Knock | can't do that: invalid input, nothing to do, not enough coins (3 fading knocks) |
| `Failed` | Error | tried and it failed: not connected, I/O error (3 hard pulses) |

### The watch telling you something
You might not be looking, so these use rhythm and can't be mistaken for a touch reply.

| role | effect | use for |
|---|---|---|
| `MessageIn` | Notify | a message or notification arrived (two soft bumps) |
| `Reminder` | Heartbeat | needs attention soon: pet nag, low battery, timer follow-up |
| `Warning` | Warning | something is wrong, act now (long-short-long) |
| `Alarm` | Alarm | timer done, alarm (3+3 strong pulses, ~1.5 s) |
| `Connected` | Success | link up: BLE paired, Wi-Fi joined, PC bridge |
| `Disconnected` | ToggleOff | link lost |

### System

| role | effect | use for |
|---|---|---|
| `Boot` | Success | power on |
| `Wake` | Click | wake from sleep |
| `Sleep` | Bump | going to sleep (soft, no brake) |
| `PowerOff` | Fade | power off (hits and dies away) |

### Games

| role | effect | use for |
|---|---|---|
| `Shoot` | Snap | fire, throw, launch |
| `HitLight` | Click | small impact, scrape, enemy hit |
| `HitHeavy` | Thud | big impact, kill, boss hit |
| `Damage` | Error | the player takes damage |
| `Explosion` | Rumble | blast, death, crash |
| `Collect` | Tick | coin, gem, crumb |
| `PowerUp` | Swell | power-up, charge complete, purchase |
| `LevelUp` | Gallop | level clear, evolve, new high score |
| `GameOver` | Fade | lose, game over |
| `Speed` | Tick | one short engine or road pulse; call it repeatedly for a texture (e.g. Racer off-road every 110 ms) |

### Companion / pet

| role | effect | use for |
|---|---|---|
| `Happy` | Heartbeat | affection, play |
| `Eat` | Purr | feeding |
| `Sad` | Fade | "no", refuse, unhappy |

---

## 2. Rules behind the table

1. **The more often it happens, the shorter and quieter it is.** Scroll, Adjust and Back fire
   dozens of times a minute, so they get Tick. Save the strong effects for rare events.
2. **Good is rising, bad is hard and repeated.** Confirm and ToggleOn go weak then strong.
   Failed is three equal hard pulses. Refused is three *fading* knocks: "no, but nothing broke".
3. **Unattended alerts need rhythm.** A single pulse while you're not looking feels like a phantom
   tap. MessageIn, Reminder, Warning and Alarm are all patterns.
4. **One meaning, one feel.** If two apps do the same thing (next track, next photo, next quote),
   they use the same role.
5. **Don't stack buzzes.** One role per event. Calling two in a row queues them back to back.
   A higher-priority effect (an alarm) cuts off a lower one (a tick), never the other way round.
6. **No blocking.** `haptic()` returns at once. Never `delay()` to space out pulses; use an
   effect with the gap built in.
7. **Power.** Full drive held for more than ~200 ms can brown out the power latch on a low
   battery. The driver caps any drive above level 105 once it has run for 150 ms, so continuous
   effects (Buzz, Swell, Engine) stay safe.

### When no role fits
- **Close enough?** Use the nearest role. Consistency beats a perfect match.
- **New kind of event that several apps would share?** Add a role:
  - add it to the `HapticRole` enum
  - add a row in `kRoles` in the same position (a static_assert checks the order)
  - add it to the table above
- **Truly app-specific feel** (a game's signature effect): build a `HapticEffect` from
  `HapticStep`s and call `hapticPlay(myEffect)`. See section 4.
- `hapticBuzz(intensity, ms)` still works for old code. Don't use it in new code.

---

## 3. Per-app plan

**Done** (on `feat/haptics`): everything below is converted. That covers the system UI, the
built-in apps and every module and game, except:
- **Write** and **Stocks:** other branches are editing them; convert them there using the rows
  below.
- **BLE Controls `buzz` command:** stays `hapticBuzz`, because the phone/cloud sends the duration.
- **Tunnel speed pulse:** stays `hapticBuzz`, because its strength tracks the speed on purpose.

Cleanups that came with it:
- Starfox's hand-timed double buzz and Pet's back-to-back buzzes are now single roles.
- Claude Face's esp_timer pulse sequencer is gone. Its statuses map to Warning (blocked on
  permission), Confirm (long turn done), Reminder (usage crossed 80/95 %) and ToggleOn
  (limit window reset).

The table shows what each call was and the role it now uses. Line numbers are from before the
change.

| app | where | event | now | role |
|---|---|---|---|---|
| Write | write_view.cpp:186 | forget learned letters | 160/120 | `Destructive` |
| | :241 | edit gesture (space, enter, backspace) | 90/25 | `Tap` |
| | :276 | letter rejected, "draw again" | 140/60 | `Refused` |
| | :290, :301 | letter recognised / case switch | 90/20 | `Scroll` / `ModeChange` |
| | :311 | text saved | 120/40 | `Confirm` |
| | :327 | correction learned | 110/30 | `Confirm` |
| Claude Face | claude_face.cpp:105 | status patterns (short/medium pulse player) | custom | `Warning` blocked, `Confirm` done, `Reminder` usage crossed, `ToggleOn` window reset |
| | :201, :432 | open/close detail | 20/22 | `Open` / `Back` |
| | :373, :383 | select / acknowledge | 24/30 | `Tap` / `Confirm` |
| Spotify | spotify_view.cpp:459 | volume step | 50/35 | `Adjust` |
| | :511-:537 | next / previous | 60/45 | `Scroll` |
| | :539 | play/pause | 80/55 | `Tap` |
| Books | bookreader.cpp:633 | can't open book | 200/60 | `Failed` |
| | :639, :676 | delete armed / deleted | 200/50, 160/40 | `LongPress` / `Destructive` |
| | :965, :1018 | control buttons / open controls | 120/20, 100/20 | `Tap` / `Open` |
| | :1023 | auto-scroll play/pause | 140/25 | `ToggleOn`/`ToggleOff` by state |
| Stocks | stocks.cpp:394, :413 | next ticker / zoom | 60/25 | `Scroll` / `Adjust` |
| Pet | petview.cpp:403, :429 | needs attention nag (two buzzes) | 70/70 x2 | `Reminder` |
| | :425 | evolved (three buzzes) | 150 x3 | `LevelUp` |
| | :427 | treat | 120 x2 | `Collect` |
| | :473, :475, :477 | feed / play / clean | 130, 120x2, 100 | `Eat` / `Happy` / `Confirm` |
| | :484 | not hungry / too tired | 50/180 | `Sad` |
| | :486, :487 | fell asleep / woke | 60/150, 90/60 | `Sleep` / `Wake` |
| | :500, :531 | poke / pet | 90/50, 60/40 | `Happy` |
| | :505, :506 | menu up/down | 40/40 | `Scroll` |
| FlyPet | flypet.cpp:479 | hello | 70/40 | `Connected` |
| | :553 | PC link flips | 90/50 | `Connected` / `Disconnected` by state |
| | :567 | ate a crumb | 120/70 | `Eat` |
| | :580 | action | 80/40 | `Tap` |
| Doom | doom.cpp:931 | player hurt | 200/70 | `Damage` |
| | :988 | kill | 220/90 | `HitHeavy` |
| | :999 | rocket / explosion | 255/120 | `Explosion` |
| | :1058, :1073 | enemy hit / burn tick | 140/30, 90/20 | `HitLight` |
| | :1082 | shot | 110-170 | `Shoot` |
| | :1120 | weapon switch | 80/20 | `ModeChange` |
| | :1139, :1829 | no ammo / not enough $ | 60 | `Refused` |
| | :1144 | rail charge start | 60/30 | `PowerUp` |
| | :1394 | coin / item pickup | 70/15, 120/40 | `Collect` / `PowerUp` |
| | :1845 | bought | 120/40 | `PowerUp` |
| | :1949 | level set | 90/30 | `Confirm` |
| | :2047 | level clear | 150/120 | `LevelUp` |
| | :2162 | hold-to-exit | 200/60 | `LongPress` |
| Racer | racer_view.cpp:256 | race start | 220/70 | `ToggleOn` |
| | :270 | crash | 255/280 | `Explosion` (also stays inside the brownout limit) |
| | :330 | off-road rumble, every 110 ms | 90/35 | `Speed` |
| Tunnel | tunnel.cpp:214 | start | 70/45 | `ToggleOn` |
| | :238, :239 | clean pass / scrape | 90/22, 45/12 | `Collect` / `HitLight` |
| | :267 | speed pulse | rising | kept as `hapticBuzz` (strength = speed) |
| | :461 | crash | 190/170 | `Explosion` |
| Starfox | starfox_shooter.cpp:332 | game over | 255/320 | `GameOver` |
| | :342, :524 | hand-timed double buzz | 170/30 x2 | `HitHeavy` (second-buzz timer removed) |
| | :359, :369 | hit by enemy / by obstacle | 220/110, 150/60 | `Damage` |
| | :375, :404 | shot / enemy down | 80/12, 110/16 | `Shoot` / `HitLight` |
| | :415 | barrel roll | 140/40 | `ModeChange` |
| | :487 | ring | 150/40 | `Collect` |
| Home Assistant | homeassistant.cpp:376, :382 | toggle sent / can't reach HA | 180/25, 90/15 | `Tap` / `Failed` |
| Print Stream | printstream_view.cpp:286, :301 | cancel confirm | 60/20 | `Back` |
| | :298 | command sent | 180/40 | `Confirm` |
| | :311, :314, :333 | page change | 60/15 | `Scroll` |
| | :323, :326, :329 | ask to confirm | 100/25 | `Tap` |
| Hidctl (Notes) | hidctl.cpp:439, :444 | cleared / saved (or failed) | 150/25, 180 or 80 | `Destructive` / `Confirm` or `Failed` |
| LLM Term | llmterm.cpp:331 | prompt chosen | 180/25 | `Tap` |
| Macropad | macropad.cpp:127 | key sent | 200/60 | `Tap` |
| Mouse | btmouse.cpp:749 | setting changed / pair | 60/30 | `Adjust` / `Tap` |
| BLE Controls | blecontrols.cpp:168 | cloud "buzz" command (ms from the app) | 200/ms | keep `hapticBuzz`, it's a remote API |
| | :328 | notify | 60/30 | `MessageIn` |
| Motion | motion_engine.cpp:198 | gesture recognised | 40/35 | `Tap` |
| | motion_view.cpp:202, :210 | capture label | 24/25 | `Scroll` |
| USB Host | usbhost.cpp:176 | start scan | 120/40 | `ToggleOn` |
| Screenshare / Livestream | screenshare.cpp:247, :252; livestream.cpp:141 | exit / start | 120/20, 40/30 | `Back` / `Connected` |
| Wake Soak | wake_soak.cpp:230, :326, :331 | answer / reset / toggle | various | `Confirm` / `Destructive` / `ToggleOn`/`Off` (test tool, low priority) |

**Ideas the apps don't do yet:**
- **Messages / eWatch Cloud:** `MessageIn` on arrival, `Confirm` on send, `Failed` on send error.
- **Stocks:** `Warning` when a watched price crosses a threshold. `Scroll` per detent while
  zooming the chart.
- **Stopwatch:** a `Collect` tick on each lap, if laps are added.
- **Watch face:** `Reminder` on the hour, if Ewan wants an hourly chime (off by default).
- **Low battery:** `Reminder` at 15 %, `Warning` at 5 %, once each.
- **Pedometer:** `LevelUp` when the daily step goal is hit.

---

## 4. Custom effects (apps that want their own feel)

```cpp
#include "haptic.h"
// level -127 (brake) .. 0 (coast) .. 127 (full); {from, to, ms} ramps.
static const HapticStep kLaser[] = {
  {127, 127, 35},     // kick: this motor needs 30-70 ms to spin up
  {110, 50, 120},     // ramp down
  {-127, -127, 20},   // brake about 1/3 of the kick to stop dead
};
static const HapticEffect kLaserFx = { "Laser", kLaser, 3, /*priority*/ 1 };
hapticPlay(kLaserFx);
```

Motor facts that matter, measured on the watch (details in the Knowledge Base,
`firmware/knowledge/drv2603-erm-haptics.md`):
- **Spin-up is slow.** Under ~30 ms of full drive barely registers: 25 ms is ~50 mg,
  60 ms is ~330 mg.
- **Stall threshold.** The motor doesn't turn below a level of ~35. Ramps should start around 45.
- **Brake.** Full reverse for ~1/3 of the kick stops it in ~25 ms; without it, it coasts ~100 ms.
- **Priorities.** 0 = UI, 1 = normal, 2 = message, 3 = alarm. Higher cuts off lower.
- **Check by measuring, not by guessing.**
  - `hap raw 127 40 -127 15` stores a custom effect
  - `hap meter raw` measures it
  - the Knowledge Base's `hapmeter.py` charts everything
