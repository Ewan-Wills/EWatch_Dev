#include <string.h>
#include <math.h>
#include "ui_style.h"
#include "ui_kit.h"

using namespace aura;

AuraTheme auraTheme() {
  ThemeColors t = theme();     // user's persisted colors (model)
  AuraTheme a;
  a.bg      = t.bg;
  a.text    = t.fg;
  // The old firmware default accent was NAVY (0x000F) — unreadably dark on
  // the new card surfaces. Treat it (and black) as "not customized" and use
  // Aurora blue; any other user choice is respected.
  a.accent  = (t.accent == 0x000F || t.accent == 0x0000) ? kBlue : t.accent;
  // Surfaces derive from bg: on the default black ground use the charcoal
  // cards; on a custom light/tinted bg, keep cards close to bg but offset.
  if (t.bg == 0x0000) {
    a.card = kCard; a.cardHi = kCardHi; a.line = kLine; a.textDim = kTextDim;
  } else {
    // Offset the bg toward the fg by ~12% for cards, 25% for pressed.
    auto mix = [](uint16_t c1, uint16_t c2, uint8_t num, uint8_t den) -> uint16_t {
      int r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
      int r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
      int r = r1 + (r2 - r1) * num / den;
      int g = g1 + (g2 - g1) * num / den;
      int b = b1 + (b2 - b1) * num / den;
      return (uint16_t)((r << 11) | (g << 5) | b);
    };
    a.card    = mix(t.bg, t.fg, 1, 8);
    a.cardHi  = mix(t.bg, t.fg, 1, 4);
    a.line    = mix(t.bg, t.fg, 3, 8);
    a.textDim = mix(t.bg, t.fg, 5, 8);
  }
  return a;
}

void auraCard(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
              int16_t r, uint16_t color) {
  if (r > h / 2) r = h / 2;
  if (r > w / 2) r = w / 2;
  d->fillRoundRect(x, y, w, h, r, color);
}

int16_t auraTextWidth(const char *s, uint8_t size) {
  return (int16_t)strlen(s) * 6 * size;
}

void auraTextCentered(Arduino_GFX *d, int16_t cx, int16_t y, const char *s,
                      uint8_t size, uint16_t color) {
  d->setFont(nullptr);
  d->setTextSize(size);
  d->setTextColor(color);
  d->setCursor(cx - auraTextWidth(s, size) / 2, y);
  d->print(s);
}

void auraPill(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
              uint16_t bg, uint16_t fg, const char *label, uint8_t textSize) {
  d->fillRoundRect(x, y, w, h, h / 2, bg);
  if (label && *label) {
    int16_t th = 8 * textSize;
    auraTextCentered(d, x + w / 2, y + (h - th) / 2 + 1, label, textSize, fg);
  }
}

int16_t auraFontWidth(Arduino_GFX *d, const GFXfont *f, const char *s) {
  d->setFont(f);
  d->setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  d->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  d->setFont(nullptr);
  return (int16_t)tw;
}

void auraFontCentered(Arduino_GFX *d, const GFXfont *f, int16_t cx, int16_t baselineY,
                      const char *s, uint16_t color) {
  d->setFont(f);
  d->setTextSize(1);
  int16_t x1, y1; uint16_t tw, th;
  d->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  d->setTextColor(color);
  d->setCursor(cx - (int16_t)tw / 2 - x1, baselineY);
  d->print(s);
  d->setFont(nullptr);
}

void auraChip(Arduino_GFX *d, int16_t x, int16_t y, uint16_t dotColor,
              const char *label, uint16_t textColor) {
  d->fillCircle(x + 5, y + 4, 4, dotColor);
  d->setFont(nullptr);
  d->setTextSize(1);
  d->setTextColor(textColor);
  d->setCursor(x + 14, y);
  d->print(label);
}

// Battery ring: 270° arc from 135° clockwise; drawn as radial ticks so it
// looks like a smooth ring at this size without a full arc rasterizer.
void auraBatteryRing(Arduino_GFX *d, int16_t cx, int16_t cy, int16_t r,
                     uint8_t pct, bool low) {
  if (pct > 100) pct = 100;
  uint16_t col = low ? kRed : (pct <= 25 ? kOrange : kGreen);
  const float a0 = 135.0f, sweep = 270.0f;
  const int   steps = 27;                       // one tick per 10°
  int lit = (int)((float)steps * pct / 100.0f + 0.5f);
  for (int i = 0; i < steps; i++) {
    float a = (a0 + sweep * i / (steps - 1)) * (float)M_PI / 180.0f;
    float ca = cosf(a), sa = sinf(a);
    int16_t x0 = cx + (int16_t)(ca * (r - 2)), y0 = cy + (int16_t)(sa * (r - 2));
    int16_t x1 = cx + (int16_t)(ca * (r + 2)), y1 = cy + (int16_t)(sa * (r + 2));
    d->drawLine(x0, y0, x1, y1, (i < lit) ? col : kLine);
  }
}

void auraToggle(Arduino_GFX *d, int16_t x, int16_t y, float t, uint16_t accent) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  const int16_t w = 44, h = 26, r = h / 2;
  // Track fades from grey to accent with t.
  int rr = ((kLine >> 11) & 0x1F) + (int)((((accent >> 11) & 0x1F) - ((kLine >> 11) & 0x1F)) * t);
  int gg = ((kLine >> 5) & 0x3F) + (int)((((accent >> 5) & 0x3F) - ((kLine >> 5) & 0x3F)) * t);
  int bb = (kLine & 0x1F) + (int)(((accent & 0x1F) - (kLine & 0x1F)) * t);
  uint16_t track = (uint16_t)((rr << 11) | (gg << 5) | bb);
  d->fillRoundRect(x, y, w, h, r, track);
  int16_t kx = x + r + (int16_t)((w - h) * t);
  d->fillCircle(kx, y + r, r - 3, kText);
}

void auraSlider(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, float val01,
                uint16_t accent) {
  if (val01 < 0) val01 = 0; else if (val01 > 1) val01 = 1;
  const int16_t h = 6;
  d->fillRoundRect(x, y, w, h, h / 2, kLine);
  int16_t fw = (int16_t)(w * val01);
  if (fw > h) d->fillRoundRect(x, y, fw, h, h / 2, accent);
  int16_t kx = x + fw;
  if (kx < x + 9) kx = x + 9;
  if (kx > x + w - 9) kx = x + w - 9;
  d->fillCircle(kx, y + h / 2, 9, kText);
}

// Chevron only — the tap target is generous even though the glyph is light.
static const int16_t BACKW = 64, BACKH = 46;
void auraBackChevron(Arduino_GFX *d, uint16_t color) {
  const int16_t cx = 22, cy = 24;
  for (int t = -1; t <= 1; t++) {              // 3-px stroke
    d->drawLine(cx + 8, cy - 10 + t, cx - 2, cy + t, color);
    d->drawLine(cx - 2, cy + t, cx + 8, cy + 10 + t, color);
  }
}
bool auraTappedBack(uint16_t x, uint16_t y) {
  return x < BACKW && y < BACKH;
}

void auraScrollbar(Arduino_GFX *d, float pos, float maxScroll, int16_t viewH) {
  if (maxScroll <= 0) return;
  float total = maxScroll + viewH;
  int16_t barH = (int16_t)((float)viewH * viewH / total);
  if (barH < 24) barH = 24;
  float p = pos / maxScroll;
  if (p < 0) p = 0; else if (p > 1) p = 1;
  int16_t barY = (int16_t)(p * (viewH - barH));
  d->fillRoundRect(W - 5, barY + 2, 3, barH, 1, kLine);
}
