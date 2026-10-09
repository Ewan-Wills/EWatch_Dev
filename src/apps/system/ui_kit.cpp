// Shared theme + chrome helpers. Every screen reads its colours from the
// model so a single Settings → Display change repaints the whole UI on the
// next render. These thin wrappers take the lock once and cache locally so
// views aren't reading `model.bgColor` ×N per frame.
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "display.h"
#include "ui_kit.h"

ThemeColors theme() {
  ThemeColors t;
  ModelLock lk;
  t.bg     = model.bgColor;
  t.fg     = model.fgColor;
  t.accent = model.accentColor;
  t.line   = model.lineColor;
  return t;
}

// Picks WHITE or BLACK depending on which is more readable against `bg` —
// used so button labels stay legible when the accent or line is light.
uint16_t contrastFor(uint16_t bg) {
  // RGB565 luminance approximation: r+g+b weighted ~3:6:1.
  uint8_t r = (bg >> 11) & 0x1F;
  uint8_t g = (bg >>  5) & 0x3F;
  uint8_t b = (bg      ) & 0x1F;
  uint16_t lum = (uint16_t)((r << 3) * 3 + (g << 2) * 6 + (b << 3) * 1) / 10;
  return (lum < 128) ? WHITE : BLACK;
}

void uiClearAll() {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
}

// Top-left back chevron. Wider/taller for easier tapping. Painted with the
// accent colour so it visually links to the SAVE/action bars below it.
static const int16_t BACK_W = 60, BACK_H = 42;
void drawBackButton() {
  if (!gfx) return;
  ThemeColors t = theme();
  uint16_t txt = contrastFor(t.accent);
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, t.accent);
  gfx->setTextColor(txt, t.accent);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}

// Generous hit zone so a thumb tap registers reliably.
bool tappedBack(uint16_t x, uint16_t y) {
  return uiInRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}

// title: text to centre below the back button
// titleY / lineY: cursor Y for the title and the divider's Y. Defaults match
// the standard settings layout; diagnostic pages override to push the title
// up so they have more vertical room for their content.
void drawTitleBar(const char *title,
                  int16_t titleY, int16_t lineY,
                  int16_t lineX, int16_t lineW) {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawBackButton();
  gfx->setTextColor(t.fg, t.bg);
  const GFXfont *uiFont = currentUiFont();
  if (uiFont) {
    // Adafruit FreeFont path. Baseline is roughly 14 px below the bitmap's
    // top-left at this size, so shift the cursor accordingly.
    gfx->setFont(uiFont);
    gfx->setTextSize(1);
    int16_t x1, y1; uint16_t tw, th;
    gfx->getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
    int16_t baseline = titleY + 16;
    gfx->setCursor((240 - (int16_t)tw) / 2 - x1, baseline);
    gfx->print(title);
    gfx->setFont(nullptr);              // back to bitmap for everything else
  } else {
    gfx->setTextSize(2);
    int16_t tw = (int16_t)strlen(title) * 12;
    gfx->setCursor((240 - tw) / 2, titleY);
    gfx->print(title);
  }
  gfx->drawFastHLine(lineX, lineY, lineW, t.line);
}
