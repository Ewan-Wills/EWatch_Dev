// DrawView: finger painting.
//
// Layout: the drawing is the top 240x240 (that square is what Send uploads);
// a slim toolbar pill sits in the bottom 40 rows: colour swatch, brush size,
// eraser, undo, clear, send. Colour, size and clear open a small popover
// above the bar; a touch anywhere else on the canvas closes it.
//
// Drawing model (undo by replay, no frame copies per step):
//   * sBase   240x240 RGB565 (PSRAM): everything older than the undo window.
//   * sOps    the last kUndo edits (strokes and clears), each a compact list
//             of 2-byte points in the sPts pool.
//   The picture = sBase + replay(sOps). A new edit beyond kUndo folds the
//   oldest op into sBase. Undo drops the newest op and recomposes.
//
// Strokes: touch samples (~33 Hz) are joined with quadratic curves through
// the sample midpoints and stamped with a round brush every r/2 px, so fast
// strokes stay continuous and smooth. The live stroke is stamped straight
// into the frame canvas and only the rectangle it touched is pushed to the
// panel. Replay uses the exact same piece functions, so an undo or a redraw
// is pixel-identical to what was drawn live (checked by the logged sums).
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "draw_view.h"
#include "draw.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "ui_style.h"
#include "cloud_msg.h"

namespace {

// ---------------------------------------------------------------- layout
constexpr int16_t W = 240, H = 280;
constexpr int16_t CH = 240;                          // drawing rows 0..239
constexpr int16_t BAR_Y = CH;                        // toolbar rows 240..279
constexpr int16_t BAR_X = 8, BAR_W = 224;            // pill inset clears the panel's round corners
constexpr int16_t BAR_PY = 242, BAR_PH = 36;
constexpr int16_t ICON_Y = BAR_PY + BAR_PH / 2;      // 260
constexpr int     kSlots = 6;                        // 37 px wide x 40 px tall targets
enum Slot : int { S_COLOR, S_SIZE, S_ERASER, S_UNDO, S_CLEAR, S_SEND };

inline int16_t slotX(int i) { return BAR_X + (BAR_W * (2 * i + 1)) / (2 * kSlots); }
inline int slotAt(int x) {
  int s = (x - BAR_X) * kSlots / BAR_W;
  return s < 0 ? 0 : s >= kSlots ? kSlots - 1 : s;
}

// Popovers (all end at y = 236, just above the bar pill).
constexpr int16_t POP_BOT = 236;
constexpr int16_t COLPOP_Y = 146;                    // 2 rows x 6 swatches
constexpr int16_t SIZEPOP_Y = 176;                   // 4 dots
constexpr int16_t CLRPOP_Y = 100;                    // confirm card
constexpr int16_t CLR_PILL_Y = 150, CLR_PILL_H = 38;
constexpr int16_t CLR_CANCEL_Y = 194, CLR_CANCEL_H = 36;

// ---------------------------------------------------------------- brushes
constexpr int     kSizes = 4;
constexpr uint8_t kBrushR[kSizes] = { 2, 4, 7, 11 };
constexpr uint8_t kEraseR[kSizes] = { 5, 8, 12, 16 };
constexpr int     kMaxR = 16;

constexpr int      kColors = 12;
constexpr uint16_t kPalette[kColors] = {
  0x0000, 0xFFFF, aura::kTextDim, 0xA285 /* sienna */, aura::kRed, aura::kOrange,
  aura::kYellow, aura::kGreen, aura::kTeal, aura::kBlue, aura::kPurple, aura::kPink,
};

// ---------------------------------------------------------------- drawing state
constexpr int kUndo   = 16;                          // undo steps (replayable ops)
constexpr int kMaxOps = kUndo + 1;                   // + the stroke going down
constexpr int kMaxPts = 12288;                       // 2 B each: 24 KB pool

enum class OpKind : uint8_t { Stroke, Clear };
struct Op { OpKind kind; uint8_t r; uint16_t color; uint16_t first, n; };
struct Pt { uint8_t x, y; };
struct Rect {
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  bool empty() const { return x1 < x0 || y1 < y0; }
  void add(int ax0, int ay0, int ax1, int ay1) {
    if (ax0 < x0) x0 = ax0; if (ay0 < y0) y0 = ay0;
    if (ax1 > x1) x1 = ax1; if (ay1 > y1) y1 = ay1;
  }
  void add(const Rect &o) { if (!o.empty()) add(o.x0, o.y0, o.x1, o.y1); }
};

Op        sOps[kMaxOps];
int       sNOps = 0;
Pt       *sPts = nullptr;
int       sNPts = 0;
uint16_t *sBase = nullptr;
uint16_t  sBaseBg = 0x0000;
uint8_t   sSpan[kMaxR + 1][kMaxR + 1];               // disc half-widths per radius, |dy|

int       sColorIdx = 1;                             // white on a black page
int       sSizeIdx = 1;
bool      sEraser = false;
bool      sInStroke = false;
int       sPieces = 0;                               // pieces of the live stroke on screen
uint32_t  sLastInputMs = 0;

enum class Pop : uint8_t { None, Color, Size, Clear };
Pop       sPop = Pop::None;

// What the finger went down on.
enum class Tgt : uint8_t { None, Swallow, Stroke, Bar, PopItem };
Tgt       sTgt = Tgt::None;
int       sTgtIdx = -1;

// Render requests (render task only).
bool      sCompose = false;                          // rebuild the drawing from base + ops
bool      sFlushAll = false;                         // push the whole frame
Rect      sPush;                                     // else push this
const char *sLogAfter = nullptr;                     // log the sum after the next compose
char      sBanner[40] = "";
uint32_t  sBannerUntil = 0;

// Stroke frame timing (render work + panel push per incremental frame).
uint32_t  sFrames = 0, sFrameUsSum = 0, sFrameUsMax = 0;

uint16_t curColor() { return kPalette[sColorIdx]; }
uint16_t curBg() {
  for (int i = sNOps - 1; i >= 0; i--) if (sOps[i].kind == OpKind::Clear) return sOps[i].color;
  return sBaseBg;
}
bool liveOp(int i) { return sInStroke && i == sNOps - 1; }

// ---------------------------------------------------------------- raster
void initSpans() {
  for (int r = 0; r <= kMaxR; r++)
    for (int dy = 0; dy <= r; dy++) {
      float rr = (r + 0.5f) * (r + 0.5f) - (float)(dy * dy);
      sSpan[r][dy] = (uint8_t)(rr > 0 ? floorf(sqrtf(rr)) : 0);
    }
}

// Round brush stamp into a 240-wide, CH-tall RGB565 buffer (clipped).
inline void disc(uint16_t *fb, int cx, int cy, int r, uint16_t c, Rect *dirty) {
  for (int dy = -r; dy <= r; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= CH) continue;
    int hw = sSpan[r][dy < 0 ? -dy : dy];
    int x0 = cx - hw, x1 = cx + hw;
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x0 > x1) continue;
    uint16_t *p = fb + y * W + x0;
    for (int x = x0; x <= x1; x++) *p++ = c;
  }
  if (dirty) dirty->add(cx - r, cy - r, cx + r, cy + r);
}

inline float stepFor(int r) { float s = r * 0.5f; return s < 1.f ? 1.f : s; }

// Straight run of stamps, both ends included.
void segment(uint16_t *fb, float ax, float ay, float bx, float by, int r, uint16_t c, Rect *d) {
  float dx = bx - ax, dy = by - ay;
  int n = (int)ceilf(sqrtf(dx * dx + dy * dy) / stepFor(r));
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    disc(fb, (int)lroundf(ax + dx * t), (int)lroundf(ay + dy * t), r, c, d);
  }
}

// Quadratic Bezier a -> b with control k.
void quad(uint16_t *fb, float ax, float ay, float kx, float ky, float bx, float by,
          int r, uint16_t c, Rect *d) {
  float l = sqrtf((kx - ax) * (kx - ax) + (ky - ay) * (ky - ay)) +
            sqrtf((bx - kx) * (bx - kx) + (by - ky) * (by - ky));
  int n = (int)ceilf(l / stepFor(r));
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n, u = 1.f - t;
    float x = u * u * ax + 2.f * u * t * kx + t * t * bx;
    float y = u * u * ay + 2.f * u * t * ky + t * t * by;
    disc(fb, (int)lroundf(x), (int)lroundf(y), r, c, d);
  }
}

// Stroke pieces over points p[0..n-1] (m(i,j) = midpoint):
//   piece 0          p0 -> m(0,1)                        (once 2 points exist)
//   piece i, 1..n-2  m(i-1,i) ~ p_i ~ m(i,i+1)           (once i+2 points exist)
//   tail             m(n-2,n-1) -> p(n-1); a dot when n == 1 (at finger up)
// The start dot p0 is painted at touch-down; every piece/tail starting at p0
// repaints it, so live drawing and replay produce the same pixels.
void piece(uint16_t *fb, const Op &o, int i, Rect *d) {
  const Pt *p = sPts + o.first;
  if (i == 0) {
    segment(fb, p[0].x, p[0].y, (p[0].x + p[1].x) * 0.5f, (p[0].y + p[1].y) * 0.5f, o.r, o.color, d);
  } else {
    quad(fb, (p[i - 1].x + p[i].x) * 0.5f, (p[i - 1].y + p[i].y) * 0.5f, p[i].x, p[i].y,
         (p[i].x + p[i + 1].x) * 0.5f, (p[i].y + p[i + 1].y) * 0.5f, o.r, o.color, d);
  }
}
void tail(uint16_t *fb, const Op &o, Rect *d) {
  const Pt *p = sPts + o.first;
  int n = o.n;
  if (n <= 0) return;
  if (n == 1) { disc(fb, p[0].x, p[0].y, o.r, o.color, d); return; }
  segment(fb, (p[n - 2].x + p[n - 1].x) * 0.5f, (p[n - 2].y + p[n - 1].y) * 0.5f,
          p[n - 1].x, p[n - 1].y, o.r, o.color, d);
}

void applyOp(uint16_t *fb, int i) {
  const Op &o = sOps[i];
  if (o.kind == OpKind::Clear) {
    for (int k = 0; k < W * CH; k++) fb[k] = o.color;
    return;
  }
  if (o.n <= 0) return;
  disc(fb, sPts[o.first].x, sPts[o.first].y, o.r, o.color, nullptr);
  for (int k = 0; k + 1 < o.n; k++) piece(fb, o, k, nullptr);
  if (liveOp(i)) sPieces = o.n - 1;      // the live stroke's tail comes at finger up
  else tail(fb, o, nullptr);
}

// Drawing area of fb := base + every op.
void compose(uint16_t *fb) {
  memcpy(fb, sBase, (size_t)W * CH * 2);
  for (int i = 0; i < sNOps; i++) applyOp(fb, i);
}

// Oldest op -> base (frees an undo slot / pool space).
void foldOldest() {
  if (sNOps <= 0) return;
  bool wasLive = liveOp(0);
  if (wasLive) return;                   // never fold the stroke going down
  const Op &o = sOps[0];
  if (o.kind == OpKind::Clear) sBaseBg = o.color;
  applyOp(sBase, 0);
  int drop = o.n;
  if (drop > 0) {
    memmove(sPts, sPts + drop, (size_t)(sNPts - drop) * sizeof(Pt));
    sNPts -= drop;
  }
  memmove(sOps, sOps + 1, (size_t)(sNOps - 1) * sizeof(Op));
  sNOps--;
  for (int i = 0; i < sNOps; i++) if (sOps[i].kind == OpKind::Stroke) sOps[i].first -= drop;
}

void trimUndo() { while (sNOps > kUndo) foldOldest(); }

// ---------------------------------------------------------------- panel push
void pushRect(Arduino_Canvas *cv, int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
  if (x1 < x0 || y1 < y0) return;
  uint16_t *fb = cv->getFramebuffer();
  if (x0 == 0 && x1 == W - 1) {          // full-width band: rows are contiguous
    gfx->draw16bitRGBBitmap(0, y0, fb + y0 * W, W, y1 - y0 + 1);
    return;
  }
  for (int y = y0; y <= y1; y++) gfx->draw16bitRGBBitmap(x0, y, fb + y * W + x0, x1 - x0 + 1, 1);
}

void logSum(const char *what) {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) return;
  EWLOGI("DRAW", "%s ops=%d pts=%d bg=%04x sum=%08lx", what, sNOps, sNPts, curBg(),
         (unsigned long)drawChecksum(cv->getFramebuffer(), (uint32_t)W * CH));
}

// ---------------------------------------------------------------- toolbar + popovers
void thick(Arduino_GFX *d, float x0, float y0, float x1, float y1, int r, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)ceilf(sqrtf(dx * dx + dy * dy)) + 1;
  for (int i = 0; i <= n; i++) d->fillCircle((int16_t)lroundf(x0 + dx * i / n), (int16_t)lroundf(y0 + dy * i / n), r, c);
}

void iconEraser(Arduino_GFX *d, int cx, int cy, uint16_t body, uint16_t tip) {
  // block at -45 deg: tip (rubber) lower-left, body upper-right
  const float ux = 0.7071f, uy = -0.7071f, vx = 0.7071f, vy = 0.7071f;
  const float L = 11.f, Wd = 5.5f, cy2 = cy - 1;
  float ax = cx - ux * L, ay = cy2 - uy * L;        // tip end centre
  float mx = cx - ux * 3.f, my = cy2 - uy * 3.f;    // tip/body boundary
  float bx = cx + ux * L, by = cy2 + uy * L;        // body end centre
  auto quadFill = [&](float p0x, float p0y, float p1x, float p1y, uint16_t c) {
    float a1x = p0x + vx * Wd, a1y = p0y + vy * Wd, a2x = p0x - vx * Wd, a2y = p0y - vy * Wd;
    float b1x = p1x + vx * Wd, b1y = p1y + vy * Wd, b2x = p1x - vx * Wd, b2y = p1y - vy * Wd;
    d->fillTriangle(a1x, a1y, a2x, a2y, b1x, b1y, c);
    d->fillTriangle(a2x, a2y, b2x, b2y, b1x, b1y, c);
  };
  quadFill(ax, ay, mx, my, tip);
  quadFill(mx, my, bx, by, body);
  d->drawFastHLine(cx - 10, cy + 11, 20, body);
}

void iconUndo(Arduino_GFX *d, int cx, int cy, uint16_t c) {
  // arc over the top from the left end round to lower right, arrowhead on the left
  const float ocx = cx + 2, ocy = cy + 3, r = 8;
  float px = 0, py = 0;
  for (int a = 180; a >= -40; a -= 10) {
    float rad = a * 0.0174533f;
    float x = ocx + r * cosf(rad), y = ocy - r * sinf(rad);
    if (a != 180) thick(d, px, py, x, y, 1, c);
    px = x; py = y;
  }
  float lx = ocx - r, ly = ocy;
  d->fillTriangle(lx - 6, ly - 2, lx + 6, ly - 2, lx, ly + 6, c);
}

void iconTrash(Arduino_GFX *d, int cx, int cy, uint16_t c, uint16_t bg) {
  d->fillRoundRect(cx - 9, cy - 8, 18, 3, 1, c);         // lid
  d->fillRect(cx - 3, cy - 11, 6, 3, c);                 // handle
  d->fillRoundRect(cx - 7, cy - 4, 14, 15, 2, c);        // can
  for (int o = -3; o <= 3; o += 3) d->drawFastVLine(cx + o, cy - 1, 9, bg);
}

void iconSend(Arduino_GFX *d, int cx, int cy, uint16_t bg, uint16_t fg) {
  d->fillCircle(cx, cy, 15, bg);
  d->fillTriangle(cx, cy - 10, cx - 8, cy - 1, cx + 8, cy - 1, fg);
  d->fillRect(cx - 2, cy - 2, 5, 12, fg);
}

void drawBar(Arduino_GFX *d) {
  d->fillRect(0, BAR_Y, W, H - BAR_Y, aura::kBg);
  auraCard(d, BAR_X, BAR_PY, BAR_W, BAR_PH, BAR_PH / 2, aura::kCard);
  if (sBanner[0] && (int32_t)(millis() - sBannerUntil) < 0) {
    auraTextCentered(d, W / 2, ICON_Y - 4, sBanner, 1, aura::kText);
    return;
  }
  sBanner[0] = 0;
  int pressed = sTgt == Tgt::Bar ? sTgtIdx : -1;
  auto hi = [&](int s, uint16_t c) { d->fillCircle(slotX(s), ICON_Y, 17, c); };
  bool popFor[kSlots] = { sPop == Pop::Color, sPop == Pop::Size, false, false, sPop == Pop::Clear, false };
  for (int s = 0; s < kSlots; s++) if (s == pressed || popFor[s]) hi(s, aura::kCardHi);

  // colour swatch (dimmed ring while the eraser is on)
  int x = slotX(S_COLOR);
  d->fillCircle(x, ICON_Y, 12, sEraser ? aura::kLine : aura::kText);
  d->fillCircle(x, ICON_Y, 10, curColor());
  // size: the brush dot itself
  x = slotX(S_SIZE);
  d->drawCircle(x, ICON_Y, 13, aura::kLine);
  d->fillCircle(x, ICON_Y, kBrushR[sSizeIdx], aura::kText);
  // eraser (blue disc while active)
  x = slotX(S_ERASER);
  if (sEraser) hi(S_ERASER, aura::kBlue);
  iconEraser(d, x, ICON_Y, aura::kText, sEraser ? aura::kText : aura::kPink);
  // undo (dim when there's nothing to undo)
  bool canUndo = sNOps > 0 && !sInStroke;
  iconUndo(d, slotX(S_UNDO), ICON_Y, canUndo ? aura::kText : aura::kLine);
  // clear
  uint16_t under = (pressed == S_CLEAR || sPop == Pop::Clear) ? aura::kCardHi : aura::kCard;
  iconTrash(d, slotX(S_CLEAR), ICON_Y, aura::kText, under);
  // send
  iconSend(d, slotX(S_SEND), ICON_Y, pressed == S_SEND ? aura::kTeal : aura::kBlue, aura::kText);
}

Rect popRect(Pop p) {
  Rect r;
  int16_t y = p == Pop::Color ? COLPOP_Y : p == Pop::Size ? SIZEPOP_Y : CLRPOP_Y;
  if (p != Pop::None) r.add(BAR_X, y, BAR_X + BAR_W - 1, POP_BOT + 6);   // + pointer
  return r;
}

void pointer(Arduino_GFX *d, int slot) {
  int x = slotX(slot);
  d->fillTriangle(x - 8, POP_BOT - 1, x + 8, POP_BOT - 1, x, POP_BOT + 6, aura::kCard);
}

void drawPopover(Arduino_GFX *d) {
  if (sPop == Pop::None) return;
  int pressed = sTgt == Tgt::PopItem ? sTgtIdx : -1;
  if (sPop == Pop::Color) {
    auraCard(d, BAR_X, COLPOP_Y, BAR_W, POP_BOT - COLPOP_Y, aura::kRadius, aura::kCard);
    pointer(d, S_COLOR);
    for (int i = 0; i < kColors; i++) {
      int x = slotX(i % 6), y = COLPOP_Y + 23 + (i / 6) * 44;
      if (i == pressed) d->fillCircle(x, y, 18, aura::kCardHi);
      if (i == sColorIdx && !sEraser) d->fillCircle(x, y, 17, aura::kText);
      d->fillCircle(x, y, 14, i == sColorIdx && !sEraser ? aura::kCard : aura::kLine);
      d->fillCircle(x, y, 13, kPalette[i]);
    }
  } else if (sPop == Pop::Size) {
    auraCard(d, BAR_X, SIZEPOP_Y, BAR_W, POP_BOT - SIZEPOP_Y, aura::kRadius, aura::kCard);
    pointer(d, S_SIZE);
    for (int i = 0; i < kSizes; i++) {
      int x = BAR_X + BAR_W * (2 * i + 1) / (2 * kSizes), y = SIZEPOP_Y + 30;
      if (i == sSizeIdx || i == pressed) d->fillCircle(x, y, 22, i == pressed ? aura::kLine : aura::kCardHi);
      d->fillCircle(x, y, kBrushR[i], sEraser ? aura::kText : curColor() == aura::kCard ? aura::kText : curColor());
      if (!sEraser && curColor() == 0x0000) d->drawCircle(x, y, kBrushR[i], aura::kTextDim);
    }
  } else {
    auraCard(d, BAR_X, CLRPOP_Y, BAR_W, POP_BOT - CLRPOP_Y, aura::kRadius, aura::kCard);
    pointer(d, S_CLEAR);
    auraTextCentered(d, W / 2, CLRPOP_Y + 14, "Clear drawing?", 2, aura::kText);
    auraTextCentered(d, W / 2, CLRPOP_Y + 36, "new page colour", 1, aura::kTextDim);
    // black page / white page / cancel
    d->fillRoundRect(16, CLR_PILL_Y, 100, CLR_PILL_H, CLR_PILL_H / 2, pressed == 0 ? aura::kCardHi : 0x0000);
    d->drawRoundRect(16, CLR_PILL_Y, 100, CLR_PILL_H, CLR_PILL_H / 2, aura::kLine);
    auraTextCentered(d, 66, CLR_PILL_Y + 11, "Black", 2, aura::kText);
    d->fillRoundRect(124, CLR_PILL_Y, 100, CLR_PILL_H, CLR_PILL_H / 2, pressed == 1 ? aura::kTextDim : 0xFFFF);
    auraTextCentered(d, 174, CLR_PILL_Y + 11, "White", 2, 0x0000);
    auraPill(d, 16, CLR_CANCEL_Y, 208, CLR_CANCEL_H, pressed == 2 ? aura::kLine : aura::kCardHi,
             aura::kText, "Cancel", 2);
  }
}

// Which popover item is at (x, y); -1 inside the card but on no item, -2 outside.
int popItemAt(int x, int y) {
  Rect r = popRect(sPop);
  if (r.empty() || x < r.x0 || x > r.x1 || y < r.y0 || y > POP_BOT) return -2;
  if (sPop == Pop::Color) {
    int row = y < COLPOP_Y + 45 ? 0 : 1;
    return row * 6 + slotAt(x);
  }
  if (sPop == Pop::Size) {
    int i = (x - BAR_X) * kSizes / BAR_W;
    return i < 0 ? 0 : i >= kSizes ? kSizes - 1 : i;
  }
  if (y >= CLR_PILL_Y - 4 && y < CLR_PILL_Y + CLR_PILL_H + 3) return x < W / 2 ? 0 : 1;
  if (y >= CLR_CANCEL_Y - 3 && y < CLR_CANCEL_Y + CLR_CANCEL_H + 4) return 2;
  return -1;
}

// ---------------------------------------------------------------- requests
void wantBar() { sPush.add(0, BAR_Y, W - 1, H - 1); }
void wantPop(Pop p) { sPush.add(popRect(p)); }
void wantAll() { sCompose = true; sFlushAll = true; }

void openPop(Pop p) {
  if (sPop == p) p = Pop::None;                  // tapping the same button closes it
  if (sPop != Pop::None) { wantPop(sPop); sCompose = true; }
  sPop = p;
  wantPop(p);
  wantBar();
}
void closePop() { if (sPop != Pop::None) openPop(sPop); }

void banner(const char *msg) {
  snprintf(sBanner, sizeof sBanner, "%s", msg);
  sBannerUntil = millis() + 2500;
  wantBar();
}

// ---------------------------------------------------------------- edits
void beginStroke(int x, int y);
void addPoint(int x, int y);

void beginStroke(int x, int y) {
  if (!sPts || !sBase) return;
  while (sNOps >= kMaxOps) foldOldest();
  while (sNPts >= kMaxPts - 1 && sNOps > 0) foldOldest();
  if (sNPts >= kMaxPts - 1) return;
  Op &o = sOps[sNOps++];
  o.kind = OpKind::Stroke;
  o.r = sEraser ? kEraseR[sSizeIdx] : kBrushR[sSizeIdx];
  o.color = sEraser ? curBg() : curColor();
  o.first = (uint16_t)sNPts;
  o.n = 0;
  sInStroke = true;
  sPieces = 0;
  sFrames = sFrameUsSum = sFrameUsMax = 0;
  addPoint(x, y);
  wantBar();                                     // undo greys out while drawing
}

void addPoint(int x, int y) {
  if (!sInStroke) return;
  if (x < 0) x = 0; if (x > W - 1) x = W - 1;
  if (y < 0) y = 0; if (y > CH - 1) y = CH - 1;
  Op &o = sOps[sNOps - 1];
  if (o.n > 0) {
    const Pt &l = sPts[o.first + o.n - 1];
    if (abs(l.x - x) + abs(l.y - y) < 2) return;   // no motion
  }
  if (sNPts >= kMaxPts) {
    if (sNOps > 1) foldOldest();
    if (sNPts >= kMaxPts) return;                // one giant stroke: stop extending it
  }
  sPts[sNPts++] = { (uint8_t)x, (uint8_t)y };
  sOps[sNOps - 1].n++;
}

// Stamp what's new of the live stroke (and its tail when finishing) into the
// canvas and push only that rectangle.
void inkLive(bool finish) {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || sNOps == 0) return;
  uint32_t t0 = micros();
  uint16_t *fb = cv->getFramebuffer();
  const Op &o = sOps[sNOps - 1];
  Rect d;
  if (sPieces == 0 && o.n > 0) disc(fb, sPts[o.first].x, sPts[o.first].y, o.r, o.color, &d);
  for (; sPieces + 1 < o.n; sPieces++) piece(fb, o, sPieces, &d);
  if (finish) tail(fb, o, &d);
  if (d.empty()) return;
  pushRect(cv, d.x0, d.y0, d.x1, d.y1);
  uint32_t us = micros() - t0;
  sFrames++; sFrameUsSum += us; if (us > sFrameUsMax) sFrameUsMax = us;
}

void endStroke() {
  if (!sInStroke) return;
  inkLive(true);
  sInStroke = false;
  const Op &o = sOps[sNOps - 1];
  EWLOGI("DRAW", "stroke n=%u r=%u color=%04x frames=%lu avg_us=%lu max_us=%lu", o.n, o.r, o.color,
         (unsigned long)sFrames, (unsigned long)(sFrames ? sFrameUsSum / sFrames : 0),
         (unsigned long)sFrameUsMax);
  if (o.n == 0) sNOps--;
  trimUndo();
  logSum("op=stroke");
  wantBar();
}

void undo() {
  if (sNOps == 0) { hapticBuzz(140, 30); return; }
  sNOps--;
  if (sOps[sNOps].kind == OpKind::Stroke) sNPts = sOps[sNOps].first;
  hapticBuzz(60, 10);
  wantAll();
  sLogAfter = "op=undo";
}

void clearTo(uint16_t bg) {
  while (sNOps >= kMaxOps) foldOldest();
  Op &o = sOps[sNOps++];
  o = { OpKind::Clear, 0, bg, (uint16_t)sNPts, 0 };
  trimUndo();
  hapticBuzz(90, 20);
  wantAll();
  sLogAfter = "op=clear";
}

void onPicked(const CloudContact *c, void *);

void send() {
  EWLOGI("DRAW", "send: pick contact");
  cloudPickContact(onPicked, nullptr, (uint8_t)Screen::Draw);
}

// Runs on the render task (the picker's callback): rebuild the picture in
// the frame canvas (the picker screen used it) and hand over the top 240x240.
void onPicked(const CloudContact *c, void *) {
  wantAll();
  if (!c) { EWLOGI("DRAW", "send: cancelled"); return; }
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || !sBase) return;
  uint16_t *fb = cv->getFramebuffer();
  compose(fb);
  uint32_t sum = drawChecksum(fb, (uint32_t)W * CH);
  bool ok = cloudSendImage(c->id, fb, W, CH, nullptr);
  EWLOGI("DRAW", "send: to=%s w=%d h=%d sum=%08lx queued=%d", c->id, W, CH, (unsigned long)sum, ok ? 1 : 0);
  if (!ok) banner("Not sent: link eWatch Cloud");
  hapticBuzz(ok ? 70 : 140, ok ? 15 : 50);
}

void activateBar(int s) {
  hapticBuzz(50, 8);
  switch (s) {
    case S_COLOR:  openPop(Pop::Color); break;
    case S_SIZE:   openPop(Pop::Size); break;
    case S_CLEAR:  openPop(Pop::Clear); break;
    case S_ERASER:
      closePop();
      sEraser = !sEraser;
      EWLOGI("DRAW", "eraser=%d", sEraser ? 1 : 0);
      wantBar();
      break;
    case S_UNDO:
      closePop();
      undo();
      break;
    case S_SEND:
      closePop();
      send();
      break;
  }
}

void activatePop(int i) {
  Pop p = sPop;
  if (p == Pop::Color && i >= 0 && i < kColors) {
    sColorIdx = i; sEraser = false;
    EWLOGI("DRAW", "color=%04x", curColor());
    closePop();
  } else if (p == Pop::Size && i >= 0 && i < kSizes) {
    sSizeIdx = i;
    EWLOGI("DRAW", "size=%d r=%u", i, kBrushR[i]);
    closePop();
  } else if (p == Pop::Clear && i >= 0) {
    closePop();
    if (i == 0 || i == 1) { clearTo(i == 0 ? 0x0000 : 0xFFFF); EWLOGI("DRAW", "clear bg=%04x", curBg()); }
  }
}

}  // namespace

// ---------------------------------------------------------------- public hooks
uint32_t drawChecksum(const uint16_t *px, uint32_t n) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) { h ^= px[i]; h *= 16777619u; }
  return h;
}

bool drawKeepAwake() { return millis() - sLastInputMs < 60000; }

bool drawConsole(const char *line) {
  if (strcmp(line, "draw state") != 0) return false;
  Serial.printf("DRAWSTATE ops=%d pts=%d undo=%d color=%04x size=%d eraser=%d bg=%04x pop=%d\n",
                sNOps, sNPts, sNOps, curColor(), sSizeIdx, sEraser ? 1 : 0, curBg(), (int)sPop);
  return true;
}

// ---------------------------------------------------------------- view
void DrawView::onEnter() {
  if (!sBase) {
    initSpans();
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    sBase = (uint16_t *)heap_caps_malloc((size_t)W * CH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sPts = (Pt *)heap_caps_malloc(sizeof(Pt) * kMaxPts, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sBase) for (int i = 0; i < W * CH; i++) sBase[i] = sBaseBg;
    size_t after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    EWLOGI("DRAW", "alloc base=%u pts=%u ok=%d psram_free=%u->%u", (unsigned)(W * CH * 2),
           (unsigned)(sizeof(Pt) * kMaxPts), sBase && sPts ? 1 : 0, (unsigned)before, (unsigned)after);
  }
  sInStroke = false;
  sPop = Pop::None;
  sTgt = Tgt::None;
  sLastInputMs = millis();
  sPush = Rect();
  wantAll();
  sLogAfter = "op=enter";
  EWLOGI("DRAW", "enter ops=%d pts=%d", sNOps, sNPts);
}

void DrawView::onExit() {
  if (sInStroke) {                               // left mid-stroke: keep what was drawn
    sInStroke = false;
    if (sNOps > 0 && sOps[sNOps - 1].n == 0) sNOps--;
    trimUndo();
  }
  sPop = Pop::None;
}

uint16_t DrawView::desiredFrameMs() const { return sInStroke ? 16 : 50; }

void DrawView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type != EventType::Touch && e.type != EventType::TouchHold && e.type != EventType::TouchUp) return;
  sLastInputMs = millis();
  if (!sBase || !sPts) return;
  int x = e.x, y = e.y;

  switch (e.type) {
    case EventType::Touch: {
      int pi = sPop != Pop::None ? popItemAt(x, y) : -2;
      if (pi != -2) {                            // on the open popover
        sTgt = Tgt::PopItem; sTgtIdx = pi;
        wantPop(sPop);
      } else if (y >= BAR_Y) {
        sTgt = Tgt::Bar; sTgtIdx = slotAt(x);
        wantBar();
      } else if (sPop != Pop::None) {            // canvas tap closes the popover, draws nothing
        closePop();
        sTgt = Tgt::Swallow;
      } else {
        sTgt = Tgt::Stroke;
        beginStroke(x, y);
      }
      break;
    }
    case EventType::TouchHold:
      if (sTgt == Tgt::Stroke) addPoint(x, y);
      break;
    case EventType::TouchUp: {
      Tgt t = sTgt; int idx = sTgtIdx;
      sTgt = Tgt::None; sTgtIdx = -1;
      if (t == Tgt::Stroke) {
        addPoint(x, y);
        endStroke();
      } else if (t == Tgt::Bar) {
        wantBar();
        if (y >= BAR_Y - 12 && slotAt(x) == idx) activateBar(idx);
      } else if (t == Tgt::PopItem) {
        wantPop(sPop);
        if (idx >= 0 && popItemAt(x, y) == idx) activatePop(idx);
      }
      break;
    }
    default: break;
  }
}

void DrawView::render() {
  if (!gfx) return;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || !sBase || !sPts) {
    gfx->fillScreen(aura::kBg);
    auraTextCentered(gfx, W / 2, 130, "Not enough memory", 2, aura::kRed);
    return;
  }
  if (sBanner[0] && (int32_t)(millis() - sBannerUntil) >= 0) wantBar();   // banner expired

  if (sInStroke) inkLive(false);
  if (!sCompose && !sFlushAll && sPush.empty()) return;

  uint32_t t0 = micros();
  bool composed = sCompose, all = sFlushAll;
  if (sCompose) { compose(cv->getFramebuffer()); sCompose = false; }
  drawBar(cv);
  drawPopover(cv);
  if (sFlushAll) cv->flush();
  else pushRect(cv, sPush.x0, sPush.y0, sPush.x1, sPush.y1);
  sFlushAll = false;
  sPush = Rect();
  if (composed && all) EWLOGD("DRAW", "redraw us=%lu", (unsigned long)(micros() - t0));
  // Edits that recompose (undo / clear / enter) log their sum once the
  // drawing area is final (no popover over it).
  if (composed && sLogAfter && sPop == Pop::None) { logSum(sLogAfter); sLogAfter = nullptr; }
}
