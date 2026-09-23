#pragma once
#include "Arduino_GFX.h"
// Framebuffer-backed Arduino_GFX for host rendering. Every primitive falls
// back to Arduino_GFX's own software implementation -> pixel-accurate text/arcs.
class HostGFX : public Arduino_GFX {
public:
  uint16_t fb[240 * 280];
  HostGFX() : Arduino_GFX(240, 280) { memset(fb, 0, sizeof fb); }
  bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { fb[y * 240 + x] = c; }
  void writePNG(const char *path);
};
