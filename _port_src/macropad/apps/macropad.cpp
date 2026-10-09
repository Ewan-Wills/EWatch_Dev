#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "macropad.h"
#include "macropad_config.h"
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock (via view.h -> model.h)

// Screen is 240 wide x 280 tall.
static const int16_t SCR_W = 240;
static const int16_t SCR_H = 280;

// Title bar occupies the top; tiles live below the divider line (y~46).
static const int16_t GRID_TOP = 54;

// ---- small local helpers (view.cpp's inRect is file-static, so re-do here) --
static inline bool ptInRect(uint16_t px, uint16_t py,
                            int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return (int16_t)px >= rx && (int16_t)px < rx + rw &&
         (int16_t)py >= ry && (int16_t)py < ry + rh;
}

void MacropadView::tileRect(int idx, int16_t &x, int16_t &y) const {
  int col = idx % MACROPAD_COLS;
  int row = idx / MACROPAD_COLS;
  x = areaX + col * (tileW + gap);
  y = areaY + row * (tileH + gap);
}

int MacropadView::tileAt(uint16_t px, uint16_t py) const {
  for (int i = 0; i < MACROPAD_COUNT; i++) {
    int16_t x, y;
    tileRect(i, x, y);
    if (ptInRect(px, py, x, y, tileW, tileH)) return i;
  }
  return -1;
}

// Draw one tile. Normal tiles are a dim panel with an accent border; a pressed
// tile is filled with the accent colour for obvious tap feedback.
void MacropadView::drawTile(int idx, bool pressed) {
  if (!gfx || idx < 0 || idx >= MACROPAD_COUNT) return;
  ThemeColors t = theme();
  int16_t x, y;
  tileRect(idx, x, y);

  uint16_t fill   = pressed ? t.accent : t.bg;
  uint16_t border = t.accent;
  uint16_t txtCol = pressed ? contrastFor(t.accent) : t.fg;

  gfx->fillRoundRect(x, y, tileW, tileH, 8, fill);
  gfx->drawRoundRect(x, y, tileW, tileH, 8, border);
  // double border for a crisper edge
  gfx->drawRoundRect(x + 1, y + 1, tileW - 2, tileH - 2, 7, border);

  const char *label = kMacroLabels[idx];
  int len = (int)strlen(label);
  // Pick a text size that fits the inner width (built-in font = 6px/char).
  int16_t innerW = tileW - 10;
  uint8_t size = 2;
  if (len * 6 * 2 > innerW) size = 1;   // fall back to small font
  int16_t textW = len * 6 * size;
  int16_t textH = 8 * size;
  int16_t tx = x + (tileW - textW) / 2;
  int16_t ty = y + (tileH - textH) / 2;
  if (tx < x + 2) tx = x + 2;
  gfx->setTextSize(size);
  gfx->setTextColor(txtCol, fill);
  gfx->setCursor(tx, ty);
  gfx->print(label);
}

void MacropadView::drawAllTiles() {
  for (int i = 0; i < MACROPAD_COUNT; i++) drawTile(i, i == pressedTile);
}

void MacropadView::onEnter() {
  // Compute grid geometry for the current screen + grid constants.
  gap   = 8;
  areaX = 8;
  areaY = GRID_TOP;
  int16_t areaW = SCR_W - 2 * areaX;
  int16_t areaH = SCR_H - areaY - 6;
  tileW = (areaW - (MACROPAD_COLS - 1) * gap) / MACROPAD_COLS;
  tileH = (areaH - (MACROPAD_ROWS - 1) * gap) / MACROPAD_ROWS;

  pressedTile = -1;
  firstDraw   = true;
  if (gfx) {
    ThemeColors t = theme();
    gfx->fillScreen(t.bg);
  }
}

void MacropadView::render() {
  if (!gfx) return;
  ThemeColors t = theme();
  bool themeChanged = (t.bg != lastBg || t.fg != lastFg ||
                       t.accent != lastAccent || t.line != lastLine);

  if (firstDraw || themeChanged) {
    gfx->fillScreen(t.bg);
    drawTitleBar("Macropad");
    drawAllTiles();
    firstDraw  = false;
    lastBg     = t.bg;
    lastFg     = t.fg;
    lastAccent = t.accent;
    lastLine   = t.line;
  }
  // Non-first renders: nothing to repaint — press feedback is drawn directly
  // from onEvent(), which also runs on the render task.
}

void MacropadView::onEvent(const Event &e) {
  // Back: hardware button (and right-swipe, which maps to ButtonShort).
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  if (e.type == EventType::Touch) {
    // Back chevron (top-left) takes priority over the grid.
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }

    int idx = tileAt(e.x, e.y);
    if (idx >= 0) {
      pressedTile = idx;
      drawTile(idx, true);                 // immediate visual feedback
      hapticBuzz(200, 60);                 // the platform "feel"
      // Serial protocol: one ASCII line per press. The PC listener maps the id.
      Serial.printf("MACRO %d\n", idx);
      Serial.flush();
    }
    return;
  }

  if (e.type == EventType::TouchUp) {
    if (pressedTile >= 0) {
      int idx = pressedTile;
      pressedTile = -1;
      drawTile(idx, false);                // release — restore normal look
    }
    return;
  }
}
