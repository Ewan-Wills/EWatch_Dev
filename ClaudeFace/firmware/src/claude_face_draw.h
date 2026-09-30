// Claude Face — pure drawing. Everything in claude_face_draw.cpp depends only
// on an Arduino_GFX* target and the plain input struct below: no FreeRTOS,
// no model, no Serial, no hardware. The host harness in test/render/
// compiles this file against a framebuffer Arduino_GFX to render PNGs.
//
// LANDSCAPE, logical 280 x 240 (see cf_orient.h for the orientation, the
// layout grid constants and the logical -> panel mapping). The caller draws
// into an Arduino_Canvas with rotation CF_CANVAS_ROTATION (or any 280x240
// Arduino_GFX); these functions only ever see logical coordinates.
//
//   HERO      x   0..135, y   0..209  5h (outer) / 7d (inner) rings with pace
//                                     ticks, centre glyph + word, caption
//                                     (label + large value) under the rings
//   CENTRE    (inside HERO) the disc inside the inner ring — the only area
//                                     redrawn per animation frame while
//                                     WORKING/COMPACTING
//   STATUS    x 136..279, y   0.. 67  HH:MM (SpaceGrotesk Bold 40), link
//                                     glyph, battery %
//   ACTIVITY  x 136..279, y  68..209  primary session: project, turn timer,
//                                     tool (2 lines mono), context bar,
//                                     subagent dots
//   FOOTER    x   0..279, y 210..239  other-session pills (+N), cache cold
#pragma once
#include <stdint.h>
#include "cf_protocol.h"
#include "cf_orient.h"

class Arduino_GFX;

enum CFLinkState : uint8_t {
  CF_LINK_NEVER = 0,   // no valid frame since boot
  CF_LINK_LIVE,        // valid frame within 90 s
  CF_LINK_OFFLINE,     // had frames, none for > 90 s
};

struct CFDrawInput {
  // Snapshot of the last valid frames (copied, never shared with the link).
  CFStateMsg    st;
  CFSessionsMsg ss;
  bool     haveState;
  bool     haveSessions;
  uint32_t rxAgeMs;          // ms since the STATE snapshot was received
  uint8_t  link;             // CFLinkState
  bool     versionMismatch;  // latest frame spoke another protocol version

  // Clock (always from the RTC via the model, bridge or not).
  bool     rtcOk;
  uint8_t  hour, minute;     // local
  uint32_t nowEpoch;         // Unix UTC seconds derived from the RTC; 0 = unknown
  int16_t  utcOffsetMin;     // local = UTC + offset

  uint32_t nowMs;            // animation clock (millis)
  uint8_t  batPct;
  bool     batOk;
};

enum CFRegion : uint8_t {
  CF_RGN_STATUS   = 0x01,
  CF_RGN_HERO     = 0x02,    // includes CENTRE
  CF_RGN_CENTRE   = 0x04,
  CF_RGN_ACTIVITY = 0x08,
  CF_RGN_FOOTER   = 0x10,
  CF_RGN_ALL      = 0x1F,
};

CFRect cfRegionRect(uint8_t region);   // logical rect; exactly one region bit
// The logical rects to put on the panel after cfDrawRegions(mask): one per
// dirty region, CENTRE only when HERO (which contains it) is not dirty.
// Returns the count (<= 4).
int cfDirtyRects(uint8_t mask, CFRect out[4]);

// Draw one or more regions (bitmask). Each region paints its own background,
// so the result never depends on what was there before.
void cfDrawRegions(Arduino_GFX *g, const CFDrawInput &in, uint8_t regions);
inline void cfDrawFace(Arduino_GFX *g, const CFDrawInput &in) {
  cfDrawRegions(g, in, CF_RGN_ALL);
}
// Long-press detail page (full screen, landscape).
void cfDrawDetail(Arduino_GFX *g, const CFDrawInput &in);

// Content key per region: changes exactly when that region would draw
// something different (excluding animation frames). The face redraws and
// pushes a region only when its key changes.
uint32_t cfRegionKey(const CFDrawInput &in, uint8_t region);

// Animation needs: 0 = static; else the region to redraw each frame and the
// frame period.
struct CFAnim { uint8_t region; uint16_t frameMs; };
CFAnim cfAnimation(const CFDrawInput &in);

// Effective hero state after link / version gates: a CFState value, or one
// of these pseudo-states.
#define CF_VIS_OFFLINE  0xF0
#define CF_VIS_MISMATCH 0xF1
uint8_t cfVisualState(const CFDrawInput &in);

// Seconds on the live turn timer (ticks locally while WORKING/BLOCKED).
uint32_t cfTurnElapsed(const CFDrawInput &in);
