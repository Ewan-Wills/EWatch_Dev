// ClaudeFace on EWatchOS: the board services the face code expects, mapped
// onto the OS (the stand-alone firmware had its own board.cpp).
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "display.h"   // gfx, backlightSet / backlightGet / backlightFadeTo (blocking here)
#include "haptic.h"    // hapticBuzz

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, weekday, hour, minute, second;
};
void rtcRequestSet(const RtcTime &t);          // -> requestSetRTC (the I/O task owns Wire)
static inline void backlightService() {}       // OS fades are blocking: nothing to advance
// frameCanvas(): the OS's full-frame PSRAM canvas (display.h)
