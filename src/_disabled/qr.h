// Tiny QR-code drawer on top of Arduino_GFX. Uses ricmoo/QRCode for encoding.
#pragma once
#include <stdint.h>

// Draw a QR encoding `text` with top-left at (x,y). Each module is `scale`
// pixels square. `version` 1..40 controls capacity; 3 fits ~30 alphanumerics
// with low ECC and is plenty for "http://192.168.4.1/".
// Returns the side length in pixels (0 on failure).
uint16_t qrDraw(const char *text, int16_t x, int16_t y,
                uint8_t scale = 4, uint8_t version = 3);
