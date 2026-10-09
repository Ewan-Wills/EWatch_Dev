#include <math.h>
#include "ui_icons.h"

// s is the nominal glyph size (height-ish). Helpers below scale from it.
static inline int16_t sc(int16_t s, int num, int den) { return (int16_t)((int32_t)s * num / den); }

static void thickLine(Arduino_GFX *d, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t t, uint16_t c) {
  // Cheap thick stroke: parallel offsets on the minor axis.
  int16_t dx = abs(x1 - x0), dy = abs(y1 - y0);
  for (int i = -(t / 2); i <= t / 2; i++) {
    if (dx >= dy) d->drawLine(x0, y0 + i, x1, y1 + i, c);
    else          d->drawLine(x0 + i, y0, x1 + i, y1, c);
  }
}

static void ring(Arduino_GFX *d, int16_t cx, int16_t cy, int16_t r, int16_t t, uint16_t c) {
  for (int i = 0; i < t; i++) d->drawCircle(cx, cy, r - i, c);
}

void drawIcon(Arduino_GFX *d, Icon ic, int16_t cx, int16_t cy, int16_t s,
              uint16_t color, uint16_t bg) {
  int16_t h = s / 2;                 // half-box
  int16_t t = s >= 40 ? 3 : 2;      // stroke weight

  switch (ic) {
    case Icon::Stopwatch: {
      int16_t r = sc(s, 5, 12);
      ring(d, cx, cy + sc(s, 1, 12), r, t, color);
      thickLine(d, cx, cy + sc(s, 1, 12), cx, cy + sc(s, 1, 12) - sc(s, 3, 12), t, color);   // hand
      d->fillRect(cx - sc(s, 1, 8), cy - h + sc(s, 0, 12), sc(s, 1, 4), sc(s, 1, 10), color); // crown
      break;
    }
    case Icon::Timer: {   // hourglass
      int16_t w = sc(s, 4, 12), hh = sc(s, 5, 12);
      d->fillTriangle(cx - w, cy - hh, cx + w, cy - hh, cx, cy, color);
      d->fillTriangle(cx - w, cy + hh, cx + w, cy + hh, cx, cy, color);
      d->fillRect(cx - w - 1, cy - hh - t, (w + 1) * 2, t, color);
      d->fillRect(cx - w - 1, cy + hh, (w + 1) * 2, t, color);
      break;
    }
    case Icon::Music: {   // beamed eighth-notes
      int16_t r = sc(s, 2, 12);
      int16_t x0 = cx - sc(s, 3, 12), x1 = cx + sc(s, 4, 12);
      int16_t yb = cy + sc(s, 4, 12);
      d->fillCircle(x0, yb, r, color);
      d->fillCircle(x1, yb - sc(s, 1, 12), r, color);
      thickLine(d, x0 + r, yb, x0 + r, cy - sc(s, 4, 12), t, color);
      thickLine(d, x1 + r, yb - sc(s, 1, 12), x1 + r, cy - sc(s, 5, 12), t, color);
      thickLine(d, x0 + r, cy - sc(s, 4, 12), x1 + r, cy - sc(s, 5, 12), t + 1, color);
      break;
    }
    case Icon::QR: {
      int16_t b = sc(s, 3, 12), g = sc(s, 1, 12);
      auto finder = [&](int16_t x, int16_t y) {
        d->fillRect(x, y, b, b, color);
        d->fillRect(x + g, y + g, b - 2 * g, b - 2 * g, bg);
        d->fillRect(x + g + (b - 2 * g) / 3, y + g + (b - 2 * g) / 3,
                    (b - 2 * g) / 3 + 1, (b - 2 * g) / 3 + 1, color);
      };
      finder(cx - h + g, cy - h + g);
      finder(cx + h - b - g, cy - h + g);
      finder(cx - h + g, cy + h - b - g);
      d->fillRect(cx + sc(s, 1, 12), cy + sc(s, 1, 12), g * 2, g * 2, color);
      d->fillRect(cx + sc(s, 3, 12), cy + sc(s, 3, 12), g * 2, g * 2, color);
      break;
    }
    case Icon::Photo: {   // mountain-in-frame
      int16_t w = sc(s, 5, 12), hh = sc(s, 4, 12);
      d->drawRoundRect(cx - w, cy - hh, w * 2, hh * 2, sc(s, 1, 8), color);
      d->fillTriangle(cx - w + 2, cy + hh - 2, cx - sc(s, 1, 12), cy - sc(s, 1, 12),
                      cx + sc(s, 2, 12), cy + hh - 2, color);
      d->fillCircle(cx + sc(s, 2, 12), cy - sc(s, 2, 12), sc(s, 1, 10) + 1, color);
      break;
    }
    case Icon::Cube: {
      int16_t r = sc(s, 5, 12);
      int16_t xs[6], ys[6];
      for (int i = 0; i < 6; i++) {
        float a = (float)M_PI / 3.0f * i - (float)M_PI / 6.0f;
        xs[i] = cx + (int16_t)(cosf(a) * r);
        ys[i] = cy + (int16_t)(sinf(a) * r);
      }
      for (int i = 0; i < 6; i++)
        thickLine(d, xs[i], ys[i], xs[(i + 1) % 6], ys[(i + 1) % 6], t - 1, color);
      for (int i = 0; i < 6; i += 2) thickLine(d, cx, cy, xs[i], ys[i], t - 1, color);
      break;
    }
    case Icon::Sparkle: {
      thickLine(d, cx - h + 2, cy, cx + h - 2, cy, t, color);
      thickLine(d, cx, cy - h + 2, cx, cy + h - 2, t, color);
      int16_t dgn = sc(s, 3, 12);
      d->drawLine(cx - dgn, cy - dgn, cx + dgn, cy + dgn, color);
      d->drawLine(cx - dgn, cy + dgn, cx + dgn, cy - dgn, color);
      d->fillCircle(cx, cy, sc(s, 1, 10), color);
      break;
    }
    case Icon::Bird: {    // two soaring chevrons
      for (int b = 0; b < 2; b++) {
        int16_t ox = (b ? sc(s, 2, 12) : -sc(s, 3, 12));
        int16_t oy = (b ? sc(s, 2, 12) : -sc(s, 1, 12));
        int16_t ww = b ? sc(s, 3, 12) : sc(s, 4, 12);
        thickLine(d, cx + ox - ww, cy + oy, cx + ox, cy + oy - sc(s, 2, 12), t - 1, color);
        thickLine(d, cx + ox, cy + oy - sc(s, 2, 12), cx + ox + ww, cy + oy, t - 1, color);
      }
      break;
    }
    case Icon::Skull: {
      int16_t r = sc(s, 5, 12);
      d->fillCircle(cx, cy - sc(s, 1, 12), r, color);
      d->fillRect(cx - sc(s, 3, 12), cy + sc(s, 1, 12), sc(s, 6, 12), sc(s, 3, 12), color);
      d->fillCircle(cx - sc(s, 2, 12), cy - sc(s, 2, 12), sc(s, 1, 8), bg);   // eyes
      d->fillCircle(cx + sc(s, 2, 12), cy - sc(s, 2, 12), sc(s, 1, 8), bg);
      d->fillTriangle(cx, cy, cx - sc(s, 1, 12), cy + sc(s, 1, 10),
                      cx + sc(s, 1, 12), cy + sc(s, 1, 10), bg);              // nose
      for (int i = -1; i <= 1; i++)                                            // teeth
        d->drawFastVLine(cx + i * sc(s, 1, 8), cy + sc(s, 2, 12), sc(s, 2, 12), bg);
      break;
    }
    case Icon::Tunnel: {
      for (int i = 0; i < 3; i++) {
        int16_t r = sc(s, 5 - i, 12);
        ring(d, cx + i * sc(s, 1, 24), cy, r, 1, color);
      }
      d->fillCircle(cx + sc(s, 2, 24), cy, sc(s, 1, 12), color);
      break;
    }
    case Icon::Ship: {    // arwing-ish dart
      d->fillTriangle(cx, cy - h + 2, cx - sc(s, 4, 12), cy + sc(s, 4, 12),
                      cx, cy + sc(s, 1, 12), color);
      d->fillTriangle(cx, cy - h + 2, cx + sc(s, 4, 12), cy + sc(s, 4, 12),
                      cx, cy + sc(s, 1, 12), color);
      thickLine(d, cx - sc(s, 5, 12), cy + sc(s, 5, 12), cx - sc(s, 2, 12), cy + sc(s, 2, 12), t - 1, color);
      thickLine(d, cx + sc(s, 5, 12), cy + sc(s, 5, 12), cx + sc(s, 2, 12), cy + sc(s, 2, 12), t - 1, color);
      break;
    }
    case Icon::Car: {
      int16_t w = sc(s, 5, 12), hh = sc(s, 2, 12);
      d->fillRoundRect(cx - w, cy - hh + sc(s, 1, 12), w * 2, hh * 2, hh, color);
      d->fillRoundRect(cx - sc(s, 3, 12), cy - sc(s, 4, 12) + sc(s, 1, 12),
                       sc(s, 6, 12), sc(s, 3, 12), sc(s, 1, 12), color);
      d->fillCircle(cx - sc(s, 3, 12), cy + sc(s, 3, 12), sc(s, 1, 8), bg);
      d->fillCircle(cx + sc(s, 3, 12), cy + sc(s, 3, 12), sc(s, 1, 8), bg);
      d->drawCircle(cx - sc(s, 3, 12), cy + sc(s, 3, 12), sc(s, 1, 8), color);
      d->drawCircle(cx + sc(s, 3, 12), cy + sc(s, 3, 12), sc(s, 1, 8), color);
      break;
    }
    case Icon::Paw: {
      int16_t r = sc(s, 3, 12);
      d->fillCircle(cx, cy + sc(s, 2, 12), r + sc(s, 1, 24), color);          // pad
      d->fillCircle(cx - sc(s, 3, 12), cy - sc(s, 1, 12), sc(s, 1, 8), color);
      d->fillCircle(cx - sc(s, 1, 12), cy - sc(s, 3, 12), sc(s, 1, 8), color);
      d->fillCircle(cx + sc(s, 1, 12), cy - sc(s, 3, 12), sc(s, 1, 8), color);
      d->fillCircle(cx + sc(s, 3, 12), cy - sc(s, 1, 12), sc(s, 1, 8), color);
      break;
    }
    case Icon::Island: {  // palm on a mound
      d->fillCircle(cx, cy + sc(s, 4, 12), sc(s, 4, 12), color);              // mound (top half visible)
      d->fillRect(cx - sc(s, 5, 12), cy + sc(s, 4, 12), sc(s, 10, 12), sc(s, 3, 12), bg);
      thickLine(d, cx, cy + sc(s, 3, 12), cx + sc(s, 1, 12), cy - sc(s, 2, 12), t - 1, color);
      for (int i = -1; i <= 1; i++) {                                         // fronds
        thickLine(d, cx + sc(s, 1, 12), cy - sc(s, 2, 12),
                  cx + sc(s, 1, 12) + sc(s, 4, 12) * i, cy - sc(s, 4, 12) + abs(i) * sc(s, 1, 12), t - 1, color);
      }
      break;
    }
    case Icon::Gear: {
      int16_t r = sc(s, 4, 12);
      for (int i = 0; i < 8; i++) {
        float a = (float)M_PI / 4.0f * i;
        int16_t x0 = cx + (int16_t)(cosf(a) * r), y0 = cy + (int16_t)(sinf(a) * r);
        int16_t x1 = cx + (int16_t)(cosf(a) * (r + sc(s, 2, 12)));
        int16_t y1 = cy + (int16_t)(sinf(a) * (r + sc(s, 2, 12)));
        thickLine(d, x0, y0, x1, y1, t, color);
      }
      ring(d, cx, cy, r, t, color);
      d->fillCircle(cx, cy, sc(s, 1, 8), color);
      break;
    }
    case Icon::Wrench: {
      thickLine(d, cx - sc(s, 4, 12), cy + sc(s, 4, 12), cx + sc(s, 2, 12), cy - sc(s, 2, 12), t + 1, color);
      ring(d, cx + sc(s, 3, 12), cy - sc(s, 3, 12), sc(s, 3, 12), t, color);
      d->fillRect(cx + sc(s, 2, 12), cy - sc(s, 4, 12), sc(s, 3, 12), sc(s, 2, 12), bg);
      break;
    }
    case Icon::Info: {
      ring(d, cx, cy, h - 2, t, color);
      d->fillCircle(cx, cy - sc(s, 2, 12), sc(s, 1, 12) + 1, color);
      d->fillRect(cx - sc(s, 1, 24) - 1, cy - sc(s, 0, 12), sc(s, 1, 12) + 2, sc(s, 3, 12), color);
      break;
    }
    case Icon::Power: {
      ring(d, cx, cy + sc(s, 1, 24), h - 3, t, color);
      d->fillRect(cx - t, cy - h + 1, t * 2 - 1, sc(s, 4, 12), bg);
      thickLine(d, cx, cy - h + 1, cx, cy - sc(s, 1, 12), t, color);
      break;
    }
    case Icon::Moon: {
      d->fillCircle(cx, cy, h - 2, color);
      d->fillCircle(cx + sc(s, 3, 12), cy - sc(s, 2, 12), h - 3, bg);
      break;
    }
    case Icon::Sun: {
      d->fillCircle(cx, cy, sc(s, 3, 12), color);
      for (int i = 0; i < 8; i++) {
        float a = (float)M_PI / 4.0f * i;
        int16_t x0 = cx + (int16_t)(cosf(a) * sc(s, 4, 12));
        int16_t y0 = cy + (int16_t)(sinf(a) * sc(s, 4, 12));
        int16_t x1 = cx + (int16_t)(cosf(a) * (h - 1));
        int16_t y1 = cy + (int16_t)(sinf(a) * (h - 1));
        d->drawLine(x0, y0, x1, y1, color);
      }
      break;
    }
    case Icon::Torch: {
      d->fillRoundRect(cx - sc(s, 2, 12), cy - sc(s, 1, 12), sc(s, 4, 12), sc(s, 6, 12), sc(s, 1, 12), color);
      d->fillTriangle(cx - sc(s, 4, 12), cy - h + 2, cx + sc(s, 4, 12), cy - h + 2,
                      cx + sc(s, 2, 12), cy - sc(s, 1, 12), color);
      d->fillTriangle(cx - sc(s, 4, 12), cy - h + 2, cx - sc(s, 2, 12), cy - sc(s, 1, 12),
                      cx + sc(s, 2, 12), cy - sc(s, 1, 12), color);
      d->fillCircle(cx, cy + sc(s, 1, 12), sc(s, 1, 12), bg);
      break;
    }
    case Icon::Wifi: {
      for (int i = 0; i < 3; i++) {
        int16_t r = sc(s, 5 - i, 12) + 1;
        // Upper arc approximated with pixels along the circle.
        for (int adeg = 225; adeg <= 315; adeg += 3) {
          float a = adeg * (float)M_PI / 180.0f;
          d->fillCircle(cx + (int16_t)(cosf(a) * r),
                        cy + sc(s, 3, 12) + (int16_t)(sinf(a) * r), 1, color);
        }
      }
      d->fillCircle(cx, cy + sc(s, 3, 12), 2, color);
      break;
    }
    case Icon::Play: {
      d->fillTriangle(cx - sc(s, 3, 12), cy - h + 2, cx - sc(s, 3, 12), cy + h - 2,
                      cx + h - 2, cy, color);
      break;
    }
    case Icon::Pause: {
      int16_t bw = sc(s, 3, 12) - 1;
      d->fillRoundRect(cx - bw - sc(s, 1, 12), cy - h + 2, bw, s - 4, 2, color);
      d->fillRoundRect(cx + sc(s, 1, 12) + 1, cy - h + 2, bw, s - 4, 2, color);
      break;
    }
    case Icon::Next: {
      d->fillTriangle(cx - h + 2, cy - h + 3, cx - h + 2, cy + h - 3, cx + sc(s, 1, 12), cy, color);
      d->fillRect(cx + sc(s, 2, 12), cy - h + 3, t + 1, s - 6, color);
      break;
    }
    case Icon::Prev: {
      d->fillTriangle(cx + h - 2, cy - h + 3, cx + h - 2, cy + h - 3, cx - sc(s, 1, 12), cy, color);
      d->fillRect(cx - sc(s, 2, 12) - t - 1, cy - h + 3, t + 1, s - 6, color);
      break;
    }
    case Icon::Check: {
      thickLine(d, cx - h + 3, cy, cx - sc(s, 1, 12), cy + sc(s, 3, 12), t, color);
      thickLine(d, cx - sc(s, 1, 12), cy + sc(s, 3, 12), cx + h - 2, cy - sc(s, 3, 12), t, color);
      break;
    }
    case Icon::Cross: {
      thickLine(d, cx - sc(s, 3, 12), cy - sc(s, 3, 12), cx + sc(s, 3, 12), cy + sc(s, 3, 12), t, color);
      thickLine(d, cx - sc(s, 3, 12), cy + sc(s, 3, 12), cx + sc(s, 3, 12), cy - sc(s, 3, 12), t, color);
      break;
    }
    case Icon::Clock: {
      ring(d, cx, cy, h - 2, t, color);
      thickLine(d, cx, cy, cx, cy - sc(s, 3, 12), t - 1, color);
      thickLine(d, cx, cy, cx + sc(s, 2, 12), cy + sc(s, 1, 12), t - 1, color);
      break;
    }
    case Icon::Vibrate: {
      d->fillRoundRect(cx - sc(s, 2, 12), cy - sc(s, 4, 12), sc(s, 4, 12), sc(s, 8, 12), sc(s, 1, 12), color);
      for (int side = -1; side <= 1; side += 2) {
        int16_t x = cx + side * sc(s, 4, 12);
        d->drawLine(x, cy - sc(s, 3, 12), x + side * 2, cy - sc(s, 1, 12), color);
        d->drawLine(x + side * 2, cy - sc(s, 1, 12), x, cy + sc(s, 1, 12), color);
        d->drawLine(x, cy + sc(s, 1, 12), x + side * 2, cy + sc(s, 3, 12), color);
      }
      break;
    }
    case Icon::Memory: {
      int16_t w = sc(s, 4, 12);
      d->drawRoundRect(cx - w, cy - w, w * 2, w * 2, 3, color);
      d->fillRect(cx - sc(s, 2, 12), cy - sc(s, 2, 12), sc(s, 4, 12), sc(s, 4, 12), color);
      for (int i = -1; i <= 1; i++) {
        d->drawFastVLine(cx + i * sc(s, 2, 12), cy - w - sc(s, 1, 12), sc(s, 1, 12), color);
        d->drawFastVLine(cx + i * sc(s, 2, 12), cy + w, sc(s, 1, 12), color);
        d->drawFastHLine(cx - w - sc(s, 1, 12), cy + i * sc(s, 2, 12), sc(s, 1, 12), color);
        d->drawFastHLine(cx + w, cy + i * sc(s, 2, 12), sc(s, 1, 12), color);
      }
      break;
    }
  }
}

void drawIconTile(Arduino_GFX *d, Icon ic, int16_t x, int16_t y, int16_t box,
                  uint16_t tile, uint16_t glyph) {
  int16_t r = box * 5 / 16;                       // squircle-ish radius
  d->fillRoundRect(x, y, box, box, r, tile);
  drawIcon(d, ic, x + box / 2, y + box / 2, box * 7 / 12, glyph, tile);
}
