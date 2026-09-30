// Shared theme + chrome helpers — EWatchOS visual language.
//
// Every legacy settings / diagnostics page calls drawTitleBar()/drawBackButton()
// for its frame, so restyling here restyles all of them at once: soft chevron
// back affordance, left-aligned title in the UI font, hairline divider.
// Geometry (title Y, divider Y, hit zones) is unchanged so page layouts hold.
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "display.h"
#include "ui_kit.h"
#include "ui_style.h"

ThemeColors theme() {
  ThemeColors t;
  ModelLock lk;
  t.bg     = model.bgColor;
  t.fg     = model.fgColor;
  t.accent = model.accentColor;
  t.line   = model.lineColor;
  return t;
}

// Picks WHITE or BLACK depending on which is more readable against `bg`.
uint16_t contrastFor(uint16_t bg) {
  uint8_t r = (bg >> 11) & 0x1F;
  uint8_t g = (bg >>  5) & 0x3F;
  uint8_t b = (bg      ) & 0x1F;
  uint16_t lum = (uint16_t)((r << 3) * 3 + (g << 2) * 6 + (b << 3) * 1) / 10;
  return (lum < 128) ? WHITE : BLACK;
}

void uiClearAll() {
  if (!gfx) return;
  gfx->fillScreen(auraTheme().bg);
}

// Back affordance: chevron glyph, generous invisible hit zone (tappedBack).
void drawBackButton() {
  if (!gfx) return;
  auraBackChevron(gfx, auraTheme().accent);
}

bool tappedBack(uint16_t x, uint16_t y) {
  return auraTappedBack(x, y);
}

void drawTitleBar(const char *title,
                  int16_t titleY, int16_t lineY,
                  int16_t lineX, int16_t lineW) {
  if (!gfx) return;
  AuraTheme th = auraTheme();
  gfx->fillScreen(th.bg);
  drawBackButton();
  const GFXfont *uiFont = currentUiFont();
  if (uiFont) {
    auraFontCentered(gfx, uiFont, 120, titleY + 16, title, th.text);
  } else {
    auraTextCentered(gfx, 120, titleY, title, 2, th.text);
  }
  gfx->drawFastHLine(lineX, lineY, lineW, th.line);
}
