#include "clock_draw.h"
#include <stdio.h>
#include <string.h>
#include "font_data.h"

static const int W = 240, H = 280;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// RGB565 blend of c over bg with alpha a (0..255).
static inline uint16_t mix(uint16_t bg, uint16_t c, uint32_t a) {
  uint32_t k = (a + 4) >> 3;                   // 0..32
  uint32_t fb = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
  uint32_t fc = (c | ((uint32_t)c << 16)) & 0x07E0F81F;
  uint32_t r = ((fb * (32 - k) + fc * k) >> 5) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

static const AAGlyph *glyph(const AAFont &f, char ch) {
  for (int i = 0; i < f.count; i++) if (f.glyphs[i].ch == ch) return &f.glyphs[i];
  return nullptr;
}

static int advance(const AAFont &f, const AAGlyph *g, bool tabular) {
  if (tabular && g->ch >= '0' && g->ch <= '9') return f.digitAdv;
  return g->adv;
}

int textWidth(bool big, const char *s, int tracking) {
  const AAFont &f = big ? FONT_BIG : FONT_SMALL;
  int w = 0, n = 0;
  for (; *s; s++) {
    const AAGlyph *g = glyph(f, *s);
    if (!g) continue;
    w += advance(f, g, big) + tracking; n++;
  }
  return n ? w - tracking : 0;
}

// Draw s with its left edge at x and baseline at y; opacity scales alpha.
static void drawText(uint16_t *fb, const AAFont &f, bool tabular, int x, int y,
                     const char *s, uint16_t c, int tracking, uint32_t opacity) {
  for (; *s; s++) {
    const AAGlyph *g = glyph(f, *s);
    if (!g) continue;
    int adv = advance(f, g, tabular);
    int gx = x + g->x + (adv - g->adv) / 2;    // centre narrow digits in their cell
    const uint8_t *a = f.alpha + g->off;
    for (int r = 0; r < g->h; r++) {
      int py = y + g->y + r;
      if (py < 0 || py >= H) continue;
      for (int q = 0; q < g->w; q++) {
        int px = gx + q;
        if (px < 0 || px >= W) continue;
        uint32_t al = a[r * g->w + q] * opacity / 255;
        if (al) fb[py * W + px] = mix(fb[py * W + px], c, al);
      }
    }
    x += adv + tracking;
  }
}

// Text with a soft dark shadow so it reads over bright skin and bone.
static void shadowText(uint16_t *fb, bool big, int cx, int y, const char *s,
                       uint16_t c, int tracking) {
  const AAFont &f = big ? FONT_BIG : FONT_SMALL;
  int x = cx - textWidth(big, s, tracking) / 2;
  static const int8_t off[][2] = {{0, 2}, {1, 2}, {-1, 2}, {0, 3}, {1, 1}, {-1, 1}};
  for (auto &o : off) drawText(fb, f, big, x + o[0], y + o[1], s, 0, tracking, big ? 70 : 90);
  drawText(fb, f, big, x, y, s, c, tracking, 255);
}

static const uint16_t kLayerColour[4] = {
  rgb(0xE8, 0xA8, 0x8E),                       // skin
  rgb(0xE6, 0xD9, 0xBC),                       // skull
  rgb(0xEE, 0x92, 0x9C),                       // brain
  rgb(0x4A, 0x96, 0xFF),                       // ventricles
};

void drawClock(uint16_t *fb, const ClockInput &in) {
  static const char *kWd[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *kMon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const int cx = W / 2;

  if (in.month >= 1 && in.month <= 12) {
    char d[20];
    snprintf(d, sizeof d, "%s %u %s", kWd[in.weekday % 7], in.day, kMon[in.month - 1]);
    shadowText(fb, false, cx, 22, d, rgb(0xB8, 0xC4, 0xD6), 2);
  }

  char t[6];
  if (in.timeOk) snprintf(t, sizeof t, "%02u:%02u", in.h % 24, in.m % 60);
  else strcpy(t, "--:--");
  shadowText(fb, true, cx, 72, t, 0xFFFF, 1);

  if (in.layer >= 0 && in.layer < 4 && in.layerName) {
    int tw = textWidth(false, in.layerName, 3);
    int x = cx - (tw + 10) / 2;
    // A small dot in the layer's colour, then the name.
    for (int dy = -3; dy <= 3; dy++)
      for (int dx = -3; dx <= 3; dx++) {
        int d2 = dx * dx + dy * dy;
        if (d2 > 12) continue;
        int px = x + 3 + dx, py = H - 20 + dy;
        fb[py * W + px] = mix(fb[py * W + px], kLayerColour[in.layer], d2 > 8 ? 120 : 255);
      }
    drawText(fb, FONT_SMALL, false, x + 10, H - 15, in.layerName, rgb(0xC8, 0xCC, 0xD4), 3, 255);
  }
}
