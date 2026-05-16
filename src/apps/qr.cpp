// QRCodeView — full-screen swipe carousel of preset QR payloads. Uses the
// ricmoo/QRCode library; modules are drawn as fillRect blocks at whatever
// integer pixel scale fits below the title bar.
#include <Arduino_GFX_Library.h>
#include <qrcode.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>

#include "qr.h"
#include "display.h"
#include "haptic.h"
#include "event.h"

static const int16_t W = 240, H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

struct QRPayload {
  const char *name;
  const char *url;
};
static const QRPayload kPayloads[] = {
  { "Phone",
    "tel:07361792063" },
  { "Email",
    "mailto:contact@ewanwills.co.uk"
    "?subject=Hello"
    "&body=I%20scanned%20the%20QR%20code%20on%20your%20watch" },
  { "Website",
    "https://ewanwills.co.uk" },
  { "LinkedIn",
    "https://www.linkedin.com/in/ewanwills/" },
};
static const int kPayloadCount = sizeof(kPayloads) / sizeof(kPayloads[0]);

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
// `tappedBack` is shared via view.h; uses the same BACK_W/BACK_H sized hit
// zone so the QR view inherits identical tap geometry.

static void renderQR(int idx) {
  const QRPayload &p = kPayloads[idx];

  // Version 6 + ECC_LOW = byte capacity 134, plenty for the email URL (~99
  // chars). 41x41 modules -> ~5 px per module fits comfortably.
  uint8_t buf[400];                 // qrcode_getBufferSize(6) = 211 bytes
  QRCode qr;
  gfx->fillScreen(WHITE);
  if (qrcode_initText(&qr, buf, 6, ECC_LOW, p.url) != 0) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(RED, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(40, 130);
    gfx->print("QR encode failed");
    return;
  }

  // Black-on-white back chevron + title + position counter.
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');

  gfx->setTextSize(2);
  gfx->setTextColor(BLACK, WHITE);
  gfx->setCursor(BACK_W + 18, 6);
  gfx->print(p.name);

  char pos[8];
  snprintf(pos, sizeof(pos), "%d/%d", idx + 1, kPayloadCount);
  gfx->setTextSize(1);
  gfx->setTextColor(0x8410, WHITE);          // mid-grey on white
  gfx->setCursor(BACK_W + 18, 28);
  gfx->print(pos);

  // QR modules below the title bar.
  const int16_t HEADER    = 56;
  const int16_t reservedH = H - HEADER - 10;
  const int16_t reservedW = W - 8;
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

void QRCodeView::onEnter() {
  index = 0;
}

void QRCodeView::render() {
  if (!gfx) return;
  renderQR(index);

  Screen exitTarget = Screen::AppList;
  for (;;) {
    Event e;
    if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(250)) != pdPASS) continue;

    // Exit: physical button, back-swipe (arrives as ButtonShort via the
    // controller), or a tap on the back chevron. NOT a plain tap.
    if (e.type == EventType::ButtonShort) break;
    if (e.type == EventType::TimerExpired) { exitTarget = Screen::Timer; break; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) break;

    if (e.type == EventType::Gesture) {
      if (e.gesture == Gesture::SwipeUp) {
        index = (index + 1) % kPayloadCount;
        hapticBuzz(50, 50);
        renderQR(index);
      } else if (e.gesture == Gesture::SwipeDown) {
        index = (index + kPayloadCount - 1) % kPayloadCount;
        hapticBuzz(50, 50);
        renderQR(index);
      }
    }
    // Plain Touch is intentionally ignored.
  }
  switchTo(exitTarget);
}
