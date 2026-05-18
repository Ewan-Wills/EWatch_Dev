// Touch driver — CST816S capacitive touchscreen.
//
// Shares the main I2C bus with the RTC + accelerometer; the chip's INT line
// is wired to a GPIO and raised whenever a touch frame or gesture is ready.
// `touchpad` is the fbiego/CST816S library object; the I/O task polls FingerNum
// directly (the chip stops emitting interrupts while a finger is held still,
// which broke the original ISR-only design).
//
// Contract:
//   * touchBegin() initialises the chip, restores the bus to 400 kHz (the
//     library re-init drops it), drains any pending touches that fired
//     during power-up, then attaches the ISR.
//   * touchPending is set true by the ISR — the I/O task clears it and
//     reads touchpad.data for gesture/coords.
//   * Live finger-down state comes from polling, NOT from this flag.
#pragma once
#include <CST816S.h>

extern CST816S       touchpad;
extern volatile bool touchPending;        // set true by ISR, cleared by reader

void touchBegin();
