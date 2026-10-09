// Claude Face — landscape orientation maths (pure; no Arduino / hardware).
//
// The watch sits on its side at the desk, plugged into USB-C. The Claude face
// is drawn LANDSCAPE, 280 wide x 240 tall, reading upright when the watch's
// physical TOP edge (panel row py = 0 of the native 240x280 portrait panel)
// points to the viewer's RIGHT. The panel itself is 240x280 portrait.
//
//   viewer (vx, vy), vx in [0,280), vy in [0,240)
//   panel  (px, py), px in [0,240), py in [0,280)
//
//       px = vy                 vx = 279 - py
//       py = 279 - vx           vy = px
//
// Check with the corners: the viewer's right edge (vx = 279) is panel row
// py = 0 (the physical top edge, pointing right as required); the viewer's
// top edge (vy = 0) is panel column px = 0 (the physical left edge — turning
// the watch 90 deg clockwise brings its left edge to the top).
//
// This is exactly Arduino_Canvas rotation 3 (GFX Library 1.4.7,
// Arduino_Canvas::writePixelPreclipped case 3: fb[(_max_x - x) * _height + y]
// with _max_x = 279, _height = 240 -> panel row 279 - x, column y). The
// framebuffer layout never changes with rotation — it stays the native
// 240x280 portrait panel image — so flush() and region pushes from it are
// native, and only the drawing coordinates are landscape.
//
// Everything here is shared by the firmware glue (claude_face.cpp), the
// drawing module (claude_face_draw.cpp) and the host tests.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CF_PANEL_W 240          // native portrait panel / canvas framebuffer
#define CF_PANEL_H 280
#define CF_VIEW_W  280          // landscape logical canvas the face draws in
#define CF_VIEW_H  240
#define CF_CANVAS_ROTATION 3    // Arduino_Canvas rotation giving the mapping above

// Landscape layout grid (logical coordinates). Single source for the region
// rectangles (claude_face_draw.cpp) and the swipe zones (cf_policy.h).
//
//   +--------------------+-----------------------+  y 0
//   |                    | STATUS  HH:MM  bat/lnk |
//   |   HERO             +-----------------------+  y CF_STATUS_H
//   |   5h / 7d rings    | ACTIVITY project,     |
//   |   (+ CENTRE disc)  | timer, tool, ctx bar, |
//   |   caption          | subagent dots         |
//   +--------------------+-----------------------+  y CF_FOOTER_Y
//   | FOOTER  other-session pills / cache cold    |
//   +---------------------------------------------+  y 240
//   x 0                  x CF_COL_X          x 280
#define CF_COL_X     136        // right column starts here; HERO is [0, CF_COL_X)
#define CF_STATUS_H   68        // STATUS is the right column above this y
#define CF_FOOTER_Y  210        // FOOTER is full width from here to the bottom

struct CFRect { int16_t x, y, w, h; };

// ---- points -----------------------------------------------------------------
static inline void cfPanelToView(int16_t px, int16_t py, int16_t &vx, int16_t &vy) {
#ifdef CF_TEST_BREAK_ORIENT
  vx = py; vy = px;                                // wrong: mirrored landscape
#else
  vx = (int16_t)(CF_PANEL_H - 1 - py); vy = px;
#endif
}
static inline void cfViewToPanel(int16_t vx, int16_t vy, int16_t &px, int16_t &py) {
  px = vy; py = (int16_t)(CF_PANEL_H - 1 - vx);
}

// ---- rectangles ---------------------------------------------------------------
// Logical rect -> the panel rect holding exactly the same pixels.
// Columns [x, x+w) become rows [280-x-w, 280-x); rows [y, y+h) become columns.
static inline CFRect cfViewRectToPanel(CFRect r) {
  CFRect p;
  p.x = r.y;
#ifdef CF_TEST_BREAK_RECT
  p.y = (int16_t)(CF_PANEL_H - 1 - r.x - r.w);     // wrong: one row off
#else
  p.y = (int16_t)(CF_PANEL_H - r.x - r.w);
#endif
  p.w = r.h;
  p.h = r.w;
  return p;
}

// ---- swipe directions ---------------------------------------------------------
// Directions of finger travel. Panel "up" is toward py = 0; since
// vx = 279 - py, travel toward smaller py is travel toward larger vx.
//   panel UP    (-py) -> viewer RIGHT (+vx)
//   panel DOWN  (+py) -> viewer LEFT  (-vx)
//   panel LEFT  (-px) -> viewer UP    (-vy)
//   panel RIGHT (+px) -> viewer DOWN  (+vy)
enum CFDir : uint8_t { CF_DIR_NONE = 0, CF_DIR_UP, CF_DIR_DOWN, CF_DIR_LEFT, CF_DIR_RIGHT };

static inline uint8_t cfPanelDirToView(uint8_t d) {
#ifdef CF_TEST_BREAK_DIR
  // wrong: the rotation-1 table (watch top pointing LEFT)
  switch (d) {
    case CF_DIR_UP:    return CF_DIR_LEFT;
    case CF_DIR_DOWN:  return CF_DIR_RIGHT;
    case CF_DIR_LEFT:  return CF_DIR_DOWN;
    case CF_DIR_RIGHT: return CF_DIR_UP;
    default:           return CF_DIR_NONE;
  }
#endif
  switch (d) {
    case CF_DIR_UP:    return CF_DIR_RIGHT;
    case CF_DIR_DOWN:  return CF_DIR_LEFT;
    case CF_DIR_LEFT:  return CF_DIR_UP;
    case CF_DIR_RIGHT: return CF_DIR_DOWN;
    default:           return CF_DIR_NONE;
  }
}

// ---- region push ----------------------------------------------------------------
// Put a panel-space rect of the native framebuffer (stride CF_PANEL_W) on the
// panel: sink(x, y, pixels, w, h) is called with contiguous blocks. A
// full-width rect goes straight out of the framebuffer; anything narrower is
// copied through `scratch` (cap pixels) in bands of whole rows, so the
// scratch buffer can stay small whatever the rect size. Without a usable
// scratch buffer the covering full-width band is pushed instead (correct,
// just more pixels). Returns the number of sink calls.
template <typename Sink>
static inline uint32_t cfPushPanelRect(uint16_t *fb, CFRect r, uint16_t *scratch,
                                       size_t cap, Sink sink) {
  if (r.x < 0) { r.w = (int16_t)(r.w + r.x); r.x = 0; }
  if (r.y < 0) { r.h = (int16_t)(r.h + r.y); r.y = 0; }
  if (r.x + r.w > CF_PANEL_W) r.w = (int16_t)(CF_PANEL_W - r.x);
  if (r.y + r.h > CF_PANEL_H) r.h = (int16_t)(CF_PANEL_H - r.y);
  if (r.w <= 0 || r.h <= 0) return 0;
  if (r.w == CF_PANEL_W || !scratch || cap < (size_t)r.w) {
    sink((int16_t)0, r.y, fb + (size_t)r.y * CF_PANEL_W, (int16_t)CF_PANEL_W, r.h);
    return 1;
  }
  int16_t rowsPer = (int16_t)(cap / (size_t)r.w);
  uint32_t calls = 0;
  for (int16_t y0 = 0; y0 < r.h; y0 = (int16_t)(y0 + rowsPer)) {
    int16_t n = (int16_t)(r.h - y0 < rowsPer ? r.h - y0 : rowsPer);
    for (int16_t k = 0; k < n; k++) {
      const uint16_t *src = fb + (size_t)(r.y + y0 + k) * CF_PANEL_W + r.x;
      uint16_t *dst = scratch + (size_t)k * r.w;
      for (int16_t i = 0; i < r.w; i++) dst[i] = src[i];
    }
    sink(r.x, (int16_t)(r.y + y0), scratch, r.w, n);
    calls++;
  }
  return calls;
}
