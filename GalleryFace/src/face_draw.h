// The analog dial, drawn over the painting wall. Depends only on Arduino_GFX
// and a plain struct, so the host harness (test/render.sh) can draw it too.
#pragma once
#include <Arduino_GFX.h>

struct FaceInput {
  bool    timeOk;       // false: show "syncing..." instead of hands
  uint8_t h, m, s;      // 24 h clock
  float   secFrac;      // 0..1 within the current second (sweep)
  uint8_t weekday;      // 0 = Sun
  uint8_t day, month;   // month 1..12
  const char *title;    // painting under the dial centre ("" = none)
};

void drawFace(Arduino_GFX *g, const FaceInput &in);
