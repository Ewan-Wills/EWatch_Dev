// ST7789 240x280 IPS panel via FSPI, plus PWM-controlled backlight.
// Backlight is non-inverted: 0 = off, 255 = full brightness (S8050 NPN driver).
#pragma once
#include <Arduino_GFX_Library.h>

extern Arduino_GFX *gfx;     // null until displayBegin() succeeds

bool displayBegin();         // returns true on success
void backlightSet(uint8_t duty);   // 0..255 PWM
void backlightOn();
void backlightOff();

// Shared full-screen (240x280) Arduino_Canvas for views that want a double-
// buffered frame. Lazily allocated on first call; returns nullptr if the
// allocation fails. Only one view is active at a time, so sharing one canvas
// across all animated views keeps SRAM pressure manageable.
Arduino_Canvas *frameCanvas();
