// Haptic driver: DRV2603 in ERM mode (LRA/~ERM tied to GND) driving a coin
// ERM motor.
//
// How the chip works (TI SLOS754, section 8.4.2, ERM mode):
//   PWM duty 100 % = full forward, 50 % = no drive (coast), 0 % = full
//   REVERSE. Reverse is the brake: it stops the spinning mass in a few ms
//   instead of letting it run down for ~80 ms. EN must stay high while
//   braking; EN low = outputs off (Hi-Z). Start-up from EN high is 1.3 ms.
//   Open-load full-scale output is 3.6 V, regulated over battery voltage.
//
// So this driver works in signed "levels": -127 (full brake/reverse) ..
// 0 (coast) .. +127 (full forward). An effect is a list of steps, each a
// level ramp over some milliseconds. A dedicated task plays them with 1 ms
// resolution. Crisp effects = overdrive kick + short brake; soft effects =
// ramps with no brake; textures = amplitude modulation (the ERM's own
// spin frequency is fixed by its speed, so rhythm and envelope are what
// make effects feel different).
//
// Contract:
//   * hapticBegin() before anything else. Every call below is non-blocking
//     and safe from any task.
//   * Requests queue (depth 6) and play back to back. A request with a
//     higher priority than the one playing cuts it off (brakes first).
//   * The user strength (0..100 %) scales every forward/brake level.
//     0 = silent: nothing is queued.
#pragma once
#include <stdint.h>

struct HapticStep {
  int8_t   from;   // level at the start of the step (-127..127)
  int8_t   to;     // level at the end (== from for a flat step)
  uint16_t ms;
};

struct HapticEffect {
  const char       *name;
  const HapticStep *steps;
  uint8_t           count;
  uint8_t           priority;   // 0 = UI tick .. 3 = alarm
};

// The library. Names are what the Haptics lab and the `hap` console show.
enum class Haptic : uint8_t {
  // crisp, short
  Tick,         // lightest detent: list scroll, digit change
  Click,        // a firm button press
  DoubleClick,  // two clicks
  Snap,         // sharp, heavy click
  Thud,         // deep, heavy hit (no high-frequency edge)
  // soft, shaped
  Bump,         // cushioned bump, no brake
  Swell,        // ramps up then stops dead
  Fade,         // hits hard and dies away
  Wave,         // up-and-down swell
  // textures
  Buzz,         // plain steady buzz
  Purr,         // 20 Hz pulsing, feels grainy
  Rumble,       // irregular, gravelly
  Engine,       // revs up: pulsing that speeds up
  // rhythms
  Heartbeat,    // lub-dub, twice
  Knock,        // knock-knock-knock, fading
  Gallop,       // da-da-DUM
  // messages
  Success,      // soft then firm: "done"
  Error,        // three hard fast pulses: "no"
  Warning,      // long-short-long
  Notify,       // gentle double bump
  Alarm,        // urgent pattern, ~1.2 s
  LongPress,    // swell into a click: "held"
  ToggleOn,     // rising pair
  ToggleOff,    // falling pair
  Count
};

void     hapticBegin();
void     hapticPlay(Haptic h);
void     hapticPlay(const HapticEffect &e);   // custom effect (keep it alive)
void     hapticStop();                         // brake + clear the queue
bool     hapticBusy();
uint32_t hapticPwmFreq();                      // LEDC timer freq actually running (bench check)
const HapticEffect &hapticEffect(Haptic h);
int      hapticFind(const char *name);        // -1 if unknown (case-insensitive)

// Legacy: one forward pulse. intensity 0..255 = how strong, duration in ms.
// Now kicks the motor up to speed and brakes it at the end, so even short
// buzzes start and stop cleanly.
void     hapticBuzz(uint8_t intensity, uint16_t duration_ms);

void     hapticSetStrengthPct(uint8_t pct);
uint8_t  hapticStrengthPct();          // the current multiplier (0 = silent)
