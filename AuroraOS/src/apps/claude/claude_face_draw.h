// Claude Face — pure drawing. Everything in claude_face_draw.cpp depends only
// on an Arduino_GFX* target and the plain input struct below: no FreeRTOS,
// no model, no Serial, no hardware. The host harness in test/host/render/
// compiles this file against a framebuffer Arduino_GFX to render PNGs.
//
// Layout (240 x 280):
//   STATUS    y   0.. 47  HH:MM (SpaceGrotesk Bold 40), battery %, link glyph
//   HERO      y  48..184  5h (outer) / 7d (inner) rings with pace ticks,
//                         centre glyph + word, caption under the rings
//   CENTRE    (inside HERO) the disc inside the inner ring — the only area
//                         redrawn per animation frame while WORKING/COMPACTING
//   ACTIVITY  y 185..253  primary session: project, turn timer, tool, context
//                         bar, subagent dots
//   FOOTER    y 254..279  other-session pills (+N), or "cache cold in 3m"
#pragma once
#include <stdint.h>
#include "cf_protocol.h"

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

struct CFRect { int16_t x, y, w, h; };
CFRect cfRegionRect(uint8_t region);           // exactly one region bit

// Draw one or more regions (bitmask). Each region paints its own background,
// so the result never depends on what was there before.
void cfDrawRegions(Arduino_GFX *g, const CFDrawInput &in, uint8_t regions);
inline void cfDrawFace(Arduino_GFX *g, const CFDrawInput &in) {
  cfDrawRegions(g, in, CF_RGN_ALL);
}
// Long-press detail page (full screen).
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
