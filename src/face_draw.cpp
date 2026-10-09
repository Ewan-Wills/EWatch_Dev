// Geometry and colours follow the AuroraOS "Analog" face (faceAnalog in
// AuroraOS/src/apps/system/watchface.cpp): 108 px tick ring, hour/minute/
// second hands 56/84/96 px, Aurora-blue second hand and hub. Over a painting
// every mark gets a dark outline so the dial reads on light and dark paint.
#include "face_draw.h"
#include <math.h>
#include <string.h>

static const int16_t W = 240, H = 280;
static const uint16_t kText   = 0xFFFF;
static const uint16_t kShadow = 0x0000;
static const uint16_t kBlue   = 0x04BF;   // AuroraOS kBlue #0A84FF
static const uint16_t kDim    = 0xD69A;   // light grey for the small text

// A straight bar of width w from (x0,y0) to (x1,y1) with round ends.
static void bar(Arduino_GFX *g, float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) { g->fillCircle((int16_t)x0, (int16_t)y0, (int16_t)(w / 2), c); return; }
  float nx = -dy / len * w / 2, ny = dx / len * w / 2;
  int16_t ax = lroundf(x0 + nx), ay = lroundf(y0 + ny), bx = lroundf(x0 - nx), by = lroundf(y0 - ny);
  int16_t cx = lroundf(x1 - nx), cy = lroundf(y1 - ny), ex = lroundf(x1 + nx), ey = lroundf(y1 + ny);
  g->fillTriangle(ax, ay, bx, by, cx, cy, c);
  g->fillTriangle(ax, ay, cx, cy, ex, ey, c);
  if (w >= 3) {
    g->fillCircle(lroundf(x0), lroundf(y0), (int16_t)(w / 2), c);
    g->fillCircle(lroundf(x1), lroundf(y1), (int16_t)(w / 2), c);
  } else {
    g->drawLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), c);
  }
}

// Default 6x8 font, scale 1, centred, with a 1 px dark halo.
static void haloText(Arduino_GFX *g, int16_t cx, int16_t y, const char *s, uint16_t c) {
  int16_t w = (int16_t)strlen(s) * 6;
  int16_t x = cx - w / 2;
  g->setFont(nullptr);
  g->setTextSize(1);
  g->setTextColor(kShadow);
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      if (dx || dy) { g->setCursor(x + dx, y + dy); g->print(s); }
  g->setTextColor(c);
  g->setCursor(x, y);
  g->print(s);
}

void drawFace(Arduino_GFX *g, const FaceInput &in) {
  const float cx = W / 2, cy = H / 2, R = 108;
  const float PI_F = 3.14159265f;

  // Tick ring: two passes so no outline covers a neighbouring tick.
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 60; i++) {
      float a = i * PI_F / 30.0f;
      bool major = (i % 5) == 0;
      float r0 = R - (major ? 12 : 6);
      float x0 = cx + sinf(a) * r0, y0 = cy - cosf(a) * r0;
      float x1 = cx + sinf(a) * R,  y1 = cy - cosf(a) * R;
      float w = major ? 3.0f : 1.0f;
      if (pass == 0) bar(g, x0, y0, x1, y1, w + 2.0f, kShadow);
      else           bar(g, x0, y0, x1, y1, w, kText);
    }
  }

  if (!in.timeOk) {
    haloText(g, (int16_t)cx, (int16_t)cy - 4, "syncing...", kText);
  } else {
    float sf = in.s + in.secFrac;
    float mf = in.m + sf / 60.0f;
    float hf = (in.h % 12) + mf / 60.0f;
    struct Hand { float frac, len, w; uint16_t c; } hands[3] = {
      { hf / 12.0f, 56, 6, kText },
      { mf / 60.0f, 84, 4, kText },
      { sf / 60.0f, 96, 2, kBlue },
    };
    for (auto &hd : hands) {            // outline, then fill, per hand
      float a = hd.frac * 2.0f * PI_F;
      float x1 = cx + sinf(a) * hd.len, y1 = cy - cosf(a) * hd.len;
      bar(g, cx, cy, x1, y1, hd.w + 3.0f, kShadow);
      bar(g, cx, cy, x1, y1, hd.w, hd.c);
    }
    g->fillCircle((int16_t)cx, (int16_t)cy, 6, kShadow);
    g->fillCircle((int16_t)cx, (int16_t)cy, 5, kBlue);
    g->fillCircle((int16_t)cx, (int16_t)cy, 2, kShadow);

    static const char *kWd[7]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *kMon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (in.month >= 1 && in.month <= 12) {
      char d[20];
      snprintf(d, sizeof d, "%s %u %s", kWd[in.weekday % 7], in.day, kMon[in.month - 1]);
      haloText(g, (int16_t)cx, (int16_t)cy + 46, d, kText);
    }
  }

  // Painting title, bottom centre (clipped to the screen width).
  if (in.title && in.title[0]) {
    char t[40];
    strncpy(t, in.title, sizeof t - 1); t[sizeof t - 1] = 0;
    if (strlen(t) > 36) strcpy(t + 33, "...");
    haloText(g, (int16_t)cx, H - 14, t, kDim);
  }
}
