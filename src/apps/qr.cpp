// QR share app. Static payload list, fullscreen QR render. Uses the
// ricmoo/QRCode library — black-on-white modules drawn as fillRect blocks
// for whatever pixel scale fits the screen.
#include <Arduino_GFX_Library.h>
#include <qrcode.h>

#include "qr.h"
#include "display.h"
#include "haptic.h"

// ---------- chrome ----------
static const int16_t W = 240, H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
static bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}
static void drawBackButton(uint16_t fg, uint16_t bg) {
  if (!gfx) return;
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(fg, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
  (void)bg;
}

// ---------- payloads ----------
struct QRPayload {
  const char *name;
  const char *desc;
  const char *url;
  uint16_t    color;
};
static const QRPayload kPayloads[] = {
  { "Phone",
    "07361 792063",
    "tel:07361792063",
    NAVY },
  { "Email",
    "contact@ewanwills.co.uk",
    "mailto:contact@ewanwills.co.uk"
    "?subject=Hello"
    "&body=I%20scanned%20the%20QR%20code%20on%20your%20watch",
    DARKGREEN },
  { "Website",
    "ewanwills.co.uk",
    "https://ewanwills.co.uk",
    PURPLE },
};
static const int kPayloadCount =
    sizeof(kPayloads) / sizeof(kPayloads[0]);

// ---------- list layout ----------
static const int16_t TILE_Y0     = 60;
static const int16_t TILE_H      = 56;
static const int16_t TILE_STRIDE = 64;

// ---------- view methods ----------

void QRCodeView::onEnter() {
  if (gfx) gfx->fillScreen(BLACK);
  mode = Mode::List;
  selectedIdx = -1;
  needsRedraw = true;
}

void QRCodeView::render() {
  if (!gfx || !needsRedraw) return;
  if (mode == Mode::List) renderList();
  else                    renderQR();
  needsRedraw = false;
}

void QRCodeView::onEvent(const Event &e) {
  if (mode == Mode::Display) {
    // Tap anywhere or back returns to the list.
    if (e.type == EventType::ButtonShort ||
        e.type == EventType::Touch) {
      mode = Mode::List;
      if (gfx) gfx->fillScreen(BLACK);
      needsRedraw = true;
    }
    return;
  }
  // List mode.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }
  if (e.type == EventType::Touch) {
    for (int i = 0; i < kPayloadCount; i++) {
      int16_t y = TILE_Y0 + i * TILE_STRIDE;
      if (inRect(e.x, e.y, 16, y, W - 32, TILE_H)) {
        hapticBuzz(80, 70);
        selectedIdx = i;
        mode = Mode::Display;
        needsRedraw = true;
        return;
      }
    }
  }
}

// ---------- rendering ----------

void QRCodeView::renderList() {
  gfx->fillScreen(BLACK);
  drawBackButton(WHITE, BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(96, 14);
  gfx->print("Share");
  gfx->drawFastHLine(20, 46, 200, DARKGREY);

  for (int i = 0; i < kPayloadCount; i++) {
    int16_t y = TILE_Y0 + i * TILE_STRIDE;
    uint16_t fg = kPayloads[i].color;
    gfx->fillRoundRect(16, y, W - 32, TILE_H, 8, fg);
    gfx->drawRoundRect(16, y, W - 32, TILE_H, 8, WHITE);
    gfx->setTextColor(WHITE, fg);
    gfx->setTextSize(3);
    gfx->setCursor(28, y + 6);
    gfx->print(kPayloads[i].name);
    gfx->setTextSize(1);
    gfx->setCursor(28, y + 38);
    gfx->print(kPayloads[i].desc);
  }
}

void QRCodeView::renderQR() {
  if (selectedIdx < 0 || selectedIdx >= kPayloadCount) return;
  const QRPayload &p = kPayloads[selectedIdx];

  // Version 6 + ECC_LOW = byte capacity 134, enough for the email URL (~99
  // chars) with margin. 41x41 modules → 5 px per module fits comfortably.
  const uint8_t version = 6;
  uint8_t buf[400];               // qrcode_getBufferSize(6) = 211 bytes
  QRCode qr;
  if (qrcode_initText(&qr, buf, version, ECC_LOW, p.url) != 0) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(RED, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(40, 130);
    gfx->print("QR encode failed");
    return;
  }

  gfx->fillScreen(WHITE);

  // Black-on-white back chevron, title text.
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');

  gfx->setTextSize(2);
  gfx->setTextColor(BLACK, WHITE);
  gfx->setCursor(BACK_W + 22, 14);
  gfx->print(p.name);

  // Compute module pixel size that fits below the title bar.
  const int16_t HEADER = 56;
  const int16_t reservedH = H - HEADER - 10;          // 10 px bottom padding
  const int16_t reservedW = W - 8;                    // 4 px side padding
  int module = (reservedW < reservedH ? reservedW : reservedH) / qr.size;
  if (module < 1) module = 1;
  int qrPx = module * qr.size;
  int x0 = (W - qrPx) / 2;
  int y0 = HEADER + (reservedH - qrPx) / 2;

  for (int y = 0; y < qr.size; y++) {
    for (int x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        gfx->fillRect(x0 + x * module, y0 + y * module, module, module, BLACK);
      }
    }
  }
}
