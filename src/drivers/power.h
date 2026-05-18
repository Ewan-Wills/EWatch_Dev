// Power driver — soft-latch around LDO U15.
//
// The watch has no hard power switch. SW2 momentarily forces the LDO enable
// pin high (through a diode); within that window the firmware must drive
// PIN_LDO_LATCH high to keep the rail up after the user releases the button.
// Driving the same pin low later cuts power and the chip stops dead.
//
// Layout:
//   * latchPower()   — MUST be the first call in setup(). Every microsecond
//                       before this the rail is held up only by SW2.
//   * unlatchPower() — drop the latch and exit; used by the deliberate
//                       Power-Off action and the panic-loop safety net in
//                       main.cpp.
//   * buttonPressed()— SW2 live state (active-high). Used by long-press
//                       power-off gestures and the wake-from-sleep path.
#pragma once
#include <Arduino.h>
#include "pins.h"

// Pre-load HIGH on the GPIO output register, THEN enable the driver. The
// alternative (pinMode → digitalWrite) flashes LOW for ~1 µs which is enough
// for the rail to droop visibly on cold boot.
static inline void latchPower() {
  digitalWrite(PIN_LDO_LATCH, HIGH);
  pinMode(PIN_LDO_LATCH, OUTPUT);
}

static inline void unlatchPower() {
  digitalWrite(PIN_LDO_LATCH, LOW);
}

static inline bool buttonPressed() {
  return digitalRead(PIN_BTN) == HIGH;
}
