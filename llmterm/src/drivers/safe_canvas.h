// SafeCanvas — Arduino_Canvas that never writes outside its framebuffer.
//
// Arduino_GFX 1.4.9 drawChar (both GFXfont and the built-in glcdfont) only
// rejects a glyph that is ENTIRELY off-screen; a glyph partly above y=0 or
// left of x=0 is drawn through writePixelPreclipped / writeFillRectPreclipped
// with negative coordinates, and Arduino_Canvas writes those straight into
// memory before the framebuffer. That heap corruption crashed the bookreader
// (LoadProhibited in the GPIO ISR, 2026-10-05). Clipping every write path
// here makes any view safe, whatever it draws or wherever it scrolls.
#pragma once
#include <Arduino_GFX_Library.h>

class SafeCanvas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    Arduino_Canvas::writePixelPreclipped(x, y, color);
  }
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    if (!clipRect(x, y, w, h)) return;
    Arduino_Canvas::writeFillRectPreclipped(x, y, w, h, color);
  }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    int16_t w = 1;
    if (!clipRect(x, y, w, h)) return;
    Arduino_Canvas::writeFastVLine(x, y, h, color);
  }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    int16_t h = 1;
    if (!clipRect(x, y, w, h)) return;
    Arduino_Canvas::writeFastHLine(x, y, w, color);
  }

private:
  bool clipRect(int16_t &x, int16_t &y, int16_t &w, int16_t &h) const {
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > _width)  w = _width - x;
    if (y + h > _height) h = _height - y;
    return w > 0 && h > 0;
  }
};
