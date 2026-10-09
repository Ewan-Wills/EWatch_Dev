#pragma once
#include "Arduino_GFX.h"
#include <vector>
// Framebuffer-backed Arduino_GFX for host rendering, rotation 0, any size.
// Every primitive falls back to Arduino_GFX's own software implementation ->
// pixel-accurate text/arcs. This is the INDEPENDENT reference for the
// landscape face: it draws straight into a 280x240 upright image with no
// rotation code involved.
class HostGFX : public Arduino_GFX {
public:
  int16_t fbW, fbH;
  std::vector<uint16_t> fb;
  HostGFX(int16_t w, int16_t h) : Arduino_GFX(w, h), fbW(w), fbH(h), fb((size_t)w * h, 0) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { fb[(size_t)y * fbW + x] = c; }
  void writePNG(const char *path) const;
};
// RGB565 buffer -> PNG
void writePNG565(const char *path, const uint16_t *px, int w, int h);
