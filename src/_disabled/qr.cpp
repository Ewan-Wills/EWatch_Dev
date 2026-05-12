#include <qrcode.h>
#include "display.h"
#include "qr.h"

uint16_t qrDraw(const char *text, int16_t x, int16_t y,
                uint8_t scale, uint8_t version) {
  if (!gfx || !text) return 0;

  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(version)];
  if (qrcode_initText(&qr, buf, version, ECC_LOW, text) != 0) return 0;

  uint16_t side = qr.size * scale;

  // White quiet-zone background (4-module border baked into the box).
  uint16_t pad = 4 * scale;
  gfx->fillRect(x - pad, y - pad, side + 2 * pad, side + 2 * pad, WHITE);

  for (uint8_t row = 0; row < qr.size; row++) {
    for (uint8_t col = 0; col < qr.size; col++) {
      if (qrcode_getModule(&qr, col, row)) {
        gfx->fillRect(x + col * scale, y + row * scale, scale, scale, BLACK);
      }
    }
  }
  return side;
}
