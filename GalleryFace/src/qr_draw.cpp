#include "qr_draw.h"
#include <string.h>
#include <qrcode.h>

static const int16_t W = 240, H = 280;

static void centered(Arduino_GFX *g, int16_t y, const char *s, uint8_t size, uint16_t c) {
  g->setFont(nullptr);
  g->setTextSize(size);
  g->setTextColor(c);
  g->setCursor((W - (int16_t)strlen(s) * 6 * size) / 2, y);
  g->print(s);
}

bool drawQR(Arduino_GFX *g, const char *title, const char *subtitle, const char *url,
            int page, int pages) {
  g->fillScreen(0xFFFF);
  static uint8_t buf[400];                      // >= qrcode_getBufferSize(10) = 367
  QRCode qr;
  // Smallest version whose byte-mode capacity at ECC M holds the URL (ISO
  // 18004 table). qrcode_initText() does NOT report overflow: it returned 0
  // for a 42-byte URL at version 1 and drew an unreadable code.
  static const uint8_t kCapM[11] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
  size_t n = strlen(url);
  int ver = 1;
  while (ver <= 10 && n > kCapM[ver]) ver++;
  if (ver > 10 || qrcode_initText(&qr, buf, ver, ECC_MEDIUM, url) != 0) {
    centered(g, 130, "QR too long", 2, 0xF800);
    return false;
  }

  centered(g, 14, title, 2, 0x0000);
  const int16_t top = 38, bottom = H - 44;      // QR area incl. quiet zone
  int16_t avail = bottom - top;
  if (avail > W - 16) avail = W - 16;
  int module = avail / (qr.size + 4);           // 2-module quiet zone each side
  int16_t qrPx = module * qr.size;
  int16_t x0 = (W - qrPx) / 2, y0 = top + (bottom - top - qrPx) / 2;
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        g->fillRect(x0 + x * module, y0 + y * module, module, module, 0x0000);

  centered(g, H - 36, subtitle, 1, 0x4208);
  for (int i = 0; i < pages; i++) {             // page dots
    int16_t cx = W / 2 + (i - (pages - 1) / 2.0f) * 14;
    if (i == page) g->fillCircle(cx, H - 14, 4, 0x0000);
    else           g->drawCircle(cx, H - 14, 4, 0x8410);
  }
  return true;
}
