// Dial: baked by tools/build_dial.py (dial_data.h). Hands: blued-steel sword
// hands with a lit and a shaded facet and a soft shadow, filled here with
// edge-distance anti-aliasing. Angles are clockwise from 12 about (120,140),
// the same convention the dial's numerals and minute track were drawn with.
#include "tank_draw.h"
#include <math.h>
#include <string.h>
#include "dial_data.h"

namespace tank {

bool testBreakHour = false;

static const float kCx = W / 2.0f, kCy = H / 2.0f;
static const float kDeg = 3.14159265f / 180.0f;
static const uint16_t kBlueDark  = 0x1128;   // #142444 shaded facet
static const uint16_t kBlueLight = 0x2A96;   // #2850B0 lit facet
static const uint16_t kBlueHub   = 0x19AE;
static const uint16_t kHubPin    = 0x9D7B;   // pale steel pin
static const float kLightX = -0.6f, kLightY = -0.8f;   // light from the top left, as the dial

float hourAngle(const Input &in) {
  float mins = testBreakHour ? 0.0f : in.m + in.s / 60.0f;
  return ((in.h % 12) + mins / 60.0f) * 30.0f;
}
float minuteAngle(const Input &in) { return (in.m + in.s / 60.0f) * 6.0f; }
float secondAngle(const Input &in) { return (in.s + in.secFrac) * 6.0f; }

static inline void blend(uint16_t *p, uint16_t c, float a) {
  if (a <= 0.0f) return;
  if (a >= 1.0f) { *p = c; return; }
  int ia = (int)(a * 256.0f);
  uint16_t d = *p;
  int r = ((d >> 11) * (256 - ia) + (c >> 11) * ia) >> 8;
  int g = (((d >> 5) & 63) * (256 - ia) + ((c >> 5) & 63) * ia) >> 8;
  int b = ((d & 31) * (256 - ia) + (c & 31) * ia) >> 8;
  *p = (uint16_t)((r << 11) | (g << 5) | b);
}

struct Pt { float x, y; };

// Anti-aliased convex polygon: coverage = distance inside the nearest edge.
static void fillConvex(uint16_t *fb, const Pt *p, int n, uint16_t c, float alpha) {
  float area = 0, x0 = p[0].x, x1 = x0, y0 = p[0].y, y1 = y0;
  for (int i = 0; i < n; i++) {
    const Pt &a = p[i], &b = p[(i + 1) % n];
    area += a.x * b.y - b.x * a.y;
    x0 = fminf(x0, a.x); x1 = fmaxf(x1, a.x); y0 = fminf(y0, a.y); y1 = fmaxf(y1, a.y);
  }
  float sign = area >= 0 ? 1.0f : -1.0f;
  float ex[8], ey[8], ec[8];                       // inside distance = ex*x + ey*y + ec
  if (n > 8) return;
  for (int i = 0; i < n; i++) {
    const Pt &a = p[i], &b = p[(i + 1) % n];
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { ex[i] = ey[i] = 0; ec[i] = 1e6f; continue; }
    ex[i] = -dy / len * sign; ey[i] = dx / len * sign;
    ec[i] = -(ex[i] * a.x + ey[i] * a.y);
  }
  int ix0 = (int)floorf(x0 - 1), ix1 = (int)ceilf(x1 + 1), iy0 = (int)floorf(y0 - 1), iy1 = (int)ceilf(y1 + 1);
  if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > W - 1) ix1 = W - 1; if (iy1 > H - 1) iy1 = H - 1;
  for (int y = iy0; y <= iy1; y++) {
    for (int x = ix0; x <= ix1; x++) {
      float px = x + 0.5f, py = y + 0.5f, d = 1e6f;
      for (int i = 0; i < n; i++) { float e = ex[i] * px + ey[i] * py + ec[i]; if (e < d) d = e; }
      float cov = d + 0.5f;
      if (cov <= 0.0f) continue;
      blend(&fb[y * W + x], c, (cov > 1.0f ? 1.0f : cov) * alpha);
    }
  }
}

static void fillDisc(uint16_t *fb, float cx, float cy, float r, uint16_t c, float alpha) {
  for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
    for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      float cov = r - hypotf(x + 0.5f - cx, y + 0.5f - cy) + 0.5f;
      if (cov > 0.0f) blend(&fb[y * W + x], c, (cov > 1.0f ? 1.0f : cov) * alpha);
    }
}

// A sword hand along `deg`: a short tail, widest at 30% of its length, then a
// long taper to the point. (ox,oy) offsets it for the shadow pass.
struct Hand { float len, tail, wBase, wMax; };

static void handPts(const Hand &h, float deg, float ox, float oy, Pt full[5], Pt lit[4]) {
  float ux = sinf(deg * kDeg), uy = -cosf(deg * kDeg);     // along the hand
  float nx = -uy, ny = ux;                                 // across it
  float cx = kCx + ox, cy = kCy + oy;
  auto at = [&](float along, float side) { return Pt{cx + ux * along + nx * side, cy + uy * along + ny * side}; };
  float belly = h.len * 0.30f;
  full[0] = at(-h.tail, -h.wBase); full[1] = at(belly, -h.wMax); full[2] = at(h.len, 0);
  full[3] = at(belly, h.wMax);     full[4] = at(-h.tail, h.wBase);
  float s = (nx * kLightX + ny * kLightY) >= 0 ? 1.0f : -1.0f;   // the facet that faces the light
  lit[0] = at(-h.tail, 0); lit[1] = at(-h.tail, s * h.wBase); lit[2] = at(belly, s * h.wMax); lit[3] = at(h.len, 0);
}

static void drawHand(uint16_t *fb, const Hand &h, float deg, float lift) {
  Pt full[5], lit[4];
  handPts(h, deg, 1.0f * lift, 1.6f * lift, full, lit);
  fillConvex(fb, full, 5, 0x0000, 0.22f);                  // shadow on the dial
  handPts(h, deg, 0, 0, full, lit);
  fillConvex(fb, full, 5, kBlueDark, 1.0f);
  fillConvex(fb, lit, 4, kBlueLight, 1.0f);
}

void draw(uint16_t *fb, const Input &in) {
  memcpy(fb, kDialPixels, sizeof(uint16_t) * W * H);
  if (in.timeOk) {
    static const Hand kHour = {45.0f, 8.0f, 2.0f, 5.2f}, kMinute = {66.0f, 9.0f, 1.8f, 4.3f};
    drawHand(fb, kHour, hourAngle(in), 1.0f);
    drawHand(fb, kMinute, minuteAngle(in), 1.6f);
    if (in.seconds) {
      float a = secondAngle(in) * kDeg, ux = sinf(a), uy = -cosf(a), nx = -uy, ny = ux, w = 0.75f;
      for (int pass = 0; pass < 2; pass++) {               // shadow, then the hand
        float cx = kCx + (pass ? 0.0f : 2.0f), cy = kCy + (pass ? 0.0f : 3.2f);
        Pt q[4] = {{cx - ux * 15 - nx * w, cy - uy * 15 - ny * w}, {cx + ux * 68 - nx * w, cy + uy * 68 - ny * w},
                   {cx + ux * 68 + nx * w, cy + uy * 68 + ny * w}, {cx - ux * 15 + nx * w, cy - uy * 15 + ny * w}};
        fillConvex(fb, q, 4, pass ? kBlueHub : 0x0000, pass ? 1.0f : 0.18f);
      }
    }
  }
  fillDisc(fb, kCx + 1.2f, kCy + 2.0f, 5.5f, 0x0000, 0.20f);
  fillDisc(fb, kCx, kCy, 5.2f, kBlueHub, 1.0f);
  fillDisc(fb, kCx - 0.6f, kCy - 0.8f, 1.7f, kHubPin, 1.0f);
}

}  // namespace tank
