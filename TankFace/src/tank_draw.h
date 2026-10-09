// The Tank dial and its hands. Draws straight into a 240x280 RGB565
// framebuffer with its own anti-aliased fill, so it needs no display library
// and the host harness (test/render.sh) runs exactly this code.
#pragma once
#include <stdint.h>

namespace tank {

const int W = 240, H = 280;

struct Input {
  bool    timeOk;     // false: bare dial, no hands
  uint8_t h, m, s;    // 24 h clock
  float   secFrac;    // 0..1 within the current second (seconds-hand sweep)
  bool    seconds;    // draw the seconds hand
};

// Hand angles in degrees clockwise from 12 (what draw() uses).
float hourAngle(const Input &in);
float minuteAngle(const Input &in);
float secondAngle(const Input &in);

void draw(uint16_t *fb, const Input &in);

// Checker control for the host test: the hour hand ignores the minutes.
extern bool testBreakHour;

}  // namespace tank
