// Full-screen QR card: title, QR code (smallest version that fits, largest
// whole-pixel module size), subtitle and page dots. Host-renderable.
#pragma once
#include <Arduino_GFX.h>

// Returns false if the text does not fit in a QR code up to version 10.
bool drawQR(Arduino_GFX *g, const char *title, const char *subtitle, const char *url,
            int page, int pages);
