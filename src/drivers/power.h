// Power latch: SW2 momentarily turns LDO U15 on, then the firmware must
// drive GPIO17 high before the user lets go. Pulling it low later powers
// the watch off.
#pragma once
#include <Arduino.h>
#include "pins.h"

// MUST be the first call in setup() — every microsecond before this, the
// rail is held up only by the user's finger on SW2.
static inline void latchPower() {
  digitalWrite(PIN_LDO_LATCH, HIGH);  // pre-load output register HIGH first,
  pinMode(PIN_LDO_LATCH, OUTPUT);     // then enable driver — no LOW glitch.
}

// Turn the watch off. The chip stops getting power as soon as this returns
// (assuming the user isn't pressing SW2).
static inline void unlatchPower() {
  digitalWrite(PIN_LDO_LATCH, LOW);
}

// True while the user is physically holding SW2. Useful for power-off gestures.
static inline bool buttonPressed() {
  return digitalRead(PIN_BTN) == HIGH;
}
