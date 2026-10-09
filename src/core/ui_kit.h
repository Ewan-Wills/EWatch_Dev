// Shared UI kit — small helpers every view uses. The heavier theme helpers
// (theme(), drawTitleBar(), ...) are declared in view.h and implemented in
// apps/system/ui_kit.cpp; this header adds the inline geometry helpers and
// the hold-to-ramp mixin so each view file doesn't re-declare its own copies.
#pragma once
#include <Arduino.h>
#include "view.h"
#include "haptic.h"

// Hit test against a rectangle. Touch coordinates are uint16_t; rect is
// signed so callers can pass computed positions without casts.
static inline bool uiInRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                            int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}

// X that centres a bitmap-font string of `size` on the 240 px screen.
// GFX glyph box = 6 px wide * size (approximate — ignores the trailing gap).
static inline int16_t uiCenterX(const char *s, uint8_t size) {
  int16_t w = (int16_t)strlen(s) * 6 * size;
  return (240 - w) / 2;
}

// The UI font of the currently-selected watch face style (nullptr = bitmap).
// Defined in apps/system/watchface.cpp next to the style table.
const GFXfont *currentUiFont();

// Fill the panel with the theme background.
void uiClearAll();

// Mixin helper bag for views with +/- columns that ramp while held.
// startHold() on the initial tap, tickHold() on every TouchHold event
// (returns true when a step should fire), stopHold() on TouchUp.
struct RampPlusMinus {
  int      holdCol     = -1;
  int8_t   holdSign    = 0;
  uint32_t holdStartMs = 0;
  uint32_t nextFireMs  = 0;

  void startHold(int col, int8_t sign) {
    holdCol = col; holdSign = sign;
    holdStartMs = millis();
    nextFireMs = holdStartMs + 400;
  }
  void stopHold() { holdCol = -1; holdSign = 0; }

  // Whether a ramp step should fire now; the period shortens the longer the
  // button is held. Caller keeps calling on each TouchHold to sustain it.
  bool tickHold(int col, int8_t sign) {
    if (holdCol < 0) return false;
    if (col != holdCol || sign != holdSign) { stopHold(); return false; }
    uint32_t now = millis();
    if (now < nextFireMs) return false;
    uint32_t held = now - holdStartMs;
    uint32_t period = held < 800 ? 400 :
                      held < 1800 ? 250 :
                      held < 3000 ? 150 :
                      held < 4500 ?  80 : 50;
    nextFireMs = now + period;
    if (period >= 150) hapticBuzz(30, 25);
    return true;
  }
};
