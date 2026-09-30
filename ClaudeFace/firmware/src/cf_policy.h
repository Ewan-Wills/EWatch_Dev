// Claude Face — pure integration policy (no Arduino / FreeRTOS / hardware).
// Everything here is decided by plain inputs so the host tests in
// test/integration/ can exercise it with clang++ -std=c++11:
//
//   * CFDoubleBuffer  — single-writer double-buffered snapshot, publish by
//                       index flip under a caller-supplied lock
//   * gesture routing — CST816S gesture id -> viewer direction -> action
//   * side button     — short press / 3 s power-off hold classifier
//   * session cycling — next SESSIONS row for SELECT_SESSION
//   * backlight       — duty from the bright triggers and BLOCKED
#pragma once
#include <stdint.h>
#include <string.h>
#include "cf_protocol.h"

// ---------------------------------------------------------------------------
// Double-buffered snapshot. ONE writer (the serial link task feeding #CF lines),
// any number of readers (the UI loop). The writer mutates back() — never
// visible to readers — then publish() flips the front index under the lock.
// read() copies the front buffer out under the same lock, so a reader can
// never see a half-written frame and never waits for more than one struct
// copy (the writer holds the lock only for the flip itself).
//
// Mux needs lock()/unlock(); on the watch it wraps a portMUX critical
// section, on the host a std::mutex.
// ---------------------------------------------------------------------------
template <typename T, typename Mux>
class CFDoubleBuffer {
public:
  explicit CFDoubleBuffer(Mux &m) : mux_(m), front_(0), gen_(0) {
    memset(buf_, 0, sizeof buf_);
  }
  // Writer: start a new version from the currently published one. The front
  // buffer is stable here — only this (single) writer ever flips it.
  T &beginWrite() {
    uint8_t b = (uint8_t)(front_ ^ 1u);
    buf_[b] = buf_[front_];
    return buf_[b];
  }
  // Writer: make the back buffer the published one.
  void publish() {
    mux_.lock();
#ifndef CF_TEST_BREAK_SNAPSHOT
    front_ = (uint8_t)(front_ ^ 1u);
#endif
    gen_++;
    mux_.unlock();
  }
  // Reader: copy the published snapshot; returns its generation (bumps on
  // every publish).
  uint32_t read(T &out) {
    mux_.lock();
    out = buf_[front_];
    uint32_t g = gen_;
    mux_.unlock();
    return g;
  }
  uint32_t generation() {
    mux_.lock();
    uint32_t g = gen_;
    mux_.unlock();
    return g;
  }

private:
  Mux &mux_;
  T buf_[2];
  volatile uint8_t  front_;
  volatile uint32_t gen_;
};

// ---------------------------------------------------------------------------
// Input on the Claude face (landscape — see cf_orient.h).
//
// The UI loop polls the CST816S itself (main.cpp): touch points and gesture
// directions arrive in NATIVE portrait panel terms and are converted to the
// viewer's landscape frame here. Every swipe direction arrives as a gesture
// (there is no system "back" conversion in this firmware).
//
// CST816S gesture ids as AuroraOS names them (core/event.h) — which is what
// the AuroraOS-hosted face mapped and what runs on the watch. NOTE the
// fbiego library header names 0x01/0x02 the other way round (SWIPE_UP /
// SWIPE_DOWN); AuroraOS' names are the ones verified against finger travel.
// ---------------------------------------------------------------------------
#include "cf_orient.h"

enum CFChipGesture : uint8_t {
  CF_G_NONE        = 0x00,
  CF_G_SWIPE_DOWN  = 0x01,   // finger travel toward panel row 279 (+py)
  CF_G_SWIPE_UP    = 0x02,   // toward row 0 (-py)
  CF_G_SWIPE_LEFT  = 0x03,   // toward column 0 (-px)
  CF_G_SWIPE_RIGHT = 0x04,   // toward column 239 (+px)
  CF_G_SINGLE_TAP  = 0x05,
  CF_G_DOUBLE_TAP  = 0x0B,
  CF_G_LONG_PRESS  = 0x0C,
};

// Chip gesture id -> native panel direction (CF_DIR_NONE for non-swipes).
static inline uint8_t cfChipGestureToPanelDir(uint8_t g) {
  switch (g) {
    case CF_G_SWIPE_UP:    return CF_DIR_UP;
    case CF_G_SWIPE_DOWN:  return CF_DIR_DOWN;
    case CF_G_SWIPE_LEFT:  return CF_DIR_LEFT;
    case CF_G_SWIPE_RIGHT: return CF_DIR_RIGHT;
    default:               return CF_DIR_NONE;
  }
}

#define CF_TAP_SLOP_PX        14
#define CF_TAP_MAX_MS        600UL
#define CF_LONGPRESS_MS      700UL
#define CF_DETAIL_TIMEOUT_MS 30000UL   // detail page closes itself after this

// What a swipe does, by VIEWER direction.
enum CFSwipeAction : uint8_t {
  CF_ACT_NONE = 0,
  CF_ACT_CLOSE_DETAIL,     // any swipe while the detail page is open
  CF_ACT_SESSION_NEXT,     // SELECT_SESSION primary+1 (needs a live link, >= 2 sessions)
  CF_ACT_SESSION_PREV,     // SELECT_SESSION primary-1
  CF_ACT_OPEN_DETAIL,      // swipe up opens the detail page
};

//   viewer LEFT  -> next session        viewer UP   -> open the detail page
//   viewer RIGHT -> previous session    viewer DOWN -> nothing
//   detail page open: every swipe closes it.
static inline uint8_t cfRouteSwipe(uint8_t viewDir, bool detail) {
  if (viewDir == CF_DIR_NONE) return CF_ACT_NONE;
  if (detail) return CF_ACT_CLOSE_DETAIL;
  switch (viewDir) {
#ifdef CF_TEST_BREAK_POLICY
    case CF_DIR_LEFT:  return CF_ACT_SESSION_PREV;   // wrong: left/right swapped
    case CF_DIR_RIGHT: return CF_ACT_SESSION_NEXT;
#else
    case CF_DIR_LEFT:  return CF_ACT_SESSION_NEXT;
    case CF_DIR_RIGHT: return CF_ACT_SESSION_PREV;
#endif
    case CF_DIR_UP:    return CF_ACT_OPEN_DETAIL;
    default:           return CF_ACT_NONE;
  }
}

static inline bool cfIsTap(int16_t dx, int16_t dy, uint32_t heldMs) {
  return (int32_t)dx * dx + (int32_t)dy * dy <= CF_TAP_SLOP_PX * CF_TAP_SLOP_PX &&
         heldMs < CF_TAP_MAX_MS;
}

// ---------------------------------------------------------------------------
// Side button (SW2, active high). A press shorter than CF_BTN_SHORT_MS is a
// short press (on release); holding CF_BTN_OFF_MS is power-off (fires once,
// while still held). Presses under CF_BTN_MIN_MS are contact bounce. A press
// already down at boot (the user is still holding SW2 from powering on) is
// ignored until it is released, so it can neither toggle nor power off.
// ---------------------------------------------------------------------------
#define CF_BTN_MIN_MS     30UL
#define CF_BTN_SHORT_MS 1000UL
#define CF_BTN_OFF_MS   3000UL

enum CFButtonEvent : uint8_t { CF_BTN_NONE = 0, CF_BTN_SHORT, CF_BTN_POWER_OFF };

struct CFButton {
  bool     down;
  bool     ignore;       // held since boot: wait for release
  bool     offFired;
  uint32_t downMs;
};

static inline void cfButtonInit(CFButton &b, bool pressedAtBoot) {
  b.down = pressedAtBoot;
#ifdef CF_TEST_BREAK_BUTTON
  b.ignore = false;                           // wrong: the power-on hold counts
#else
  b.ignore = pressedAtBoot;
#endif
  b.offFired = false;
  b.downMs = 0;
}

static inline uint8_t cfButtonStep(CFButton &b, bool pressed, uint32_t nowMs) {
  if (pressed && !b.down) {
    b.down = true;
    b.ignore = false;
    b.offFired = false;
    b.downMs = nowMs;
    return CF_BTN_NONE;
  }
  if (pressed) {
    if (!b.ignore && !b.offFired && (uint32_t)(nowMs - b.downMs) >= CF_BTN_OFF_MS) {
      b.offFired = true;
      return CF_BTN_POWER_OFF;
    }
    return CF_BTN_NONE;
  }
  if (b.down) {                               // released
    b.down = false;
    bool wasIgnored = b.ignore;
    b.ignore = false;
    uint32_t held = (uint32_t)(nowMs - b.downMs);
    if (!wasIgnored && !b.offFired && held >= CF_BTN_MIN_MS && held < CF_BTN_SHORT_MS)
      return CF_BTN_SHORT;
  }
  return CF_BTN_NONE;
}

// Next SESSIONS row for SELECT_SESSION, wrapping. 255 = nothing to cycle
// (fewer than two sessions). An out-of-range current index starts from 0.
static inline uint8_t cfNextSessionIndex(uint8_t current, uint8_t count, int dir) {
  if (count > 4) count = 4;
  if (count < 2) return 255;
  int cur = (current < count) ? current : 0;
  int n = (cur + (dir >= 0 ? 1 : -1) + count) % count;
  return (uint8_t)n;
}

// Is there something a tap acknowledges? (SPEC: tap = ACK DONE/BLOCKED;
// never approves anything.)
static inline bool cfAckable(const CFStateMsg &st) {
  if (st.p_state == CF_STATE_BLOCKED || st.blocked_count > 0) return true;
  if (st.p_state == CF_STATE_DONE && (st.p_flags & CF_PFLAGS_UNACKED_DONE)) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Backlight. The screen never turns off in this firmware (no sleep of any
// kind): it runs at 100 % for CF_BRIGHT_HOLD_MS after the last trigger (a
// visible state change, touch, button, wrist raise, boot) and at the ambient
// level otherwise — with or without a bridge. BLOCKED (live link only)
// forces 100 %.
// ---------------------------------------------------------------------------
#define CF_BRIGHT_HOLD_MS 10000UL

// Duty 0..255.
static inline uint8_t cfBacklightDuty(uint32_t nowMs, uint32_t lastBrightMs,
                                      bool haveBrightTrigger, bool blocked,
                                      uint8_t ambientPct) {
  if (blocked) return 255;
  if (haveBrightTrigger && (uint32_t)(nowMs - lastBrightMs) < CF_BRIGHT_HOLD_MS)
    return 255;
  if (ambientPct > 100) ambientPct = 100;
  uint16_t d = (uint16_t)((uint16_t)ambientPct * 255u / 100u);
  if (ambientPct > 0 && d < 4) d = 4;        // 1 % must still be visible
  return (uint8_t)d;
}

// "Visible state" identity used for the 10 s bright-after-change trigger.
static inline uint32_t cfStateIdentity(const CFStateMsg &st) {
  return (uint32_t)st.global_state | ((uint32_t)st.p_state << 8) |
         ((uint32_t)st.primary_index << 16) |
         ((uint32_t)(st.blocked_count ? 1u : 0u) << 24);
}
