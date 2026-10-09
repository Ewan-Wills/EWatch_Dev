// Display driver — ST7789 240×280 IPS panel via FSPI + PWM backlight.
//
// Panel:
//   * 240 px wide, 280 px tall, RGB565 native (Arduino_GFX driver).
//   * Spec'd col offset 0, row offset 20 for this FPC.
//   * 60 MHz SPI clock (`gfx->begin(60000000)`); displayBegin() handles init.
//
// Backlight:
//   * Driven through an S8050 NPN; PWM input is non-inverted
//     (0 = off, 255 = full brightness).
//   * Uses LEDC channel 0 at 12 kHz, 8-bit resolution.
//   * displayBegin() leaves the backlight OFF so the user never sees the
//     panel's power-on garbage. Caller must enable it after the first paint.
//
// Frame canvas:
//   * frameCanvas() lazily allocates a single shared 240×280 Arduino_Canvas
//     (≈134 KiB in PSRAM) for views that need a double-buffered frame.
//     Returns nullptr if the allocation fails. Only one view is active at a
//     time so sharing one canvas keeps SRAM pressure manageable.
#pragma once
#include <Arduino_GFX_Library.h>

extern Arduino_GFX *gfx;                  // null until displayBegin() succeeds

bool             displayBegin();          // returns true on success
void             backlightSet(uint8_t duty);
uint8_t          backlightGet();
void             backlightFadeTo(uint8_t target, uint16_t ms);  // blocking
void             backlightOn();
void             backlightOff();

// Panel power: DISPOFF/DISPON retain panel RAM and return immediately (the
// light-sleep fast-wake path). panelSleepIn() = DISPOFF + SLPIN + 120 ms and
// is only for the deep-sleep path (wake is a full reboot).
void             panelDispOff();
void             panelDispOn();
void             panelSleepIn();

Arduino_Canvas  *frameCanvas();
