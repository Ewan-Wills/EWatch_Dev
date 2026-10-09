// The clock over the head: HH:MM, date, and the name of the layer showing.
// Anti-aliased Space Grotesk glyphs (src/font_data.h) alpha-blended straight
// into a 240x280 RGB565 framebuffer, so it needs no display library and the
// host harness (test/render.sh) draws exactly what the watch draws.
#pragma once
#include <stdint.h>

struct ClockInput {
  bool    timeOk;       // false: "--:--"
  uint8_t h, m;         // 24 h clock
  uint8_t weekday;      // 0 = Sun
  uint8_t day, month;   // month 1..12 (0 = no date)
  int8_t  layer;        // 0 skin .. 3 ventricles, -1 = no label
  const char *layerName;
};

void drawClock(uint16_t *fb, const ClockInput &in);

// Width in pixels of s in the big (tabular=true: fixed digit cells) or small font.
int textWidth(bool big, const char *s, int tracking);
