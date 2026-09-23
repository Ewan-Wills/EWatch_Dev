// Claude Face — pure integration policy (no Arduino / FreeRTOS / hardware).
// Everything here is decided by plain inputs so the host tests in
// test/host/integration/ can exercise it with clang++ -std=c++11:
//
//   * CFDoubleBuffer  — single-writer double-buffered snapshot, publish by
//                       index flip under a caller-supplied lock
//   * gesture routing — which Claude-face action an input maps to
//   * session cycling — next SESSIONS row for SELECT_SESSION
//   * backlight       — duty while the link holds the face awake
//   * hold / wake     — when the face keeps the watch awake, and which STATE
//                       edges are worth lighting a dark screen for
#pragma once
#include <stdint.h>
#include <string.h>
#include "cf_protocol.h"

// ---------------------------------------------------------------------------
// Double-buffered snapshot. ONE writer (the console task feeding #CF lines),
// any number of readers (the render task). The writer mutates back() — never
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
// Gesture routing on the Claude face.
//
// Platform facts this has to live with (controller.cpp taskIO):
//   * a CST816S SwipeRight is converted into ButtonShort (global "back"), so
//     it arrives with NO coordinates — the same event the side button makes;
//   * the physical button always posts ButtonDown before its ButtonShort,
//     a converted swipe never does.
// So: a ButtonShort with no ButtonDown since the last touch-down, shortly
// after that touch-down, is a swipe-right and the touch-down y says where it
// started.
// ---------------------------------------------------------------------------
#ifndef CF_SWIPE_ZONE_Y
#define CF_SWIPE_ZONE_Y      175     // swipes starting at/below this cycle sessions
#endif
#define CF_TOUCH_RECENT_MS  1500UL   // touch-down -> gesture pairing window
#define CF_TAP_SLOP_PX        14
#define CF_TAP_MAX_MS        600UL
#define CF_LONGPRESS_MS      700UL

static inline bool cfButtonShortIsSwipe(bool touchSeen, bool buttonDownSinceTouch,
                                        uint32_t nowMs, uint32_t touchDownMs) {
  return touchSeen && !buttonDownSinceTouch &&
         (uint32_t)(nowMs - touchDownMs) < CF_TOUCH_RECENT_MS;
}

// Start-y for a gesture: the recorded touch-down y when it is recent (the
// CST816S gesture coordinate is wherever the finger was when it latched),
// else the gesture's own y.
static inline uint16_t cfGestureStartY(bool touchSeen, uint32_t nowMs,
                                       uint32_t touchDownMs, uint16_t downY,
                                       uint16_t gestureY) {
  if (touchSeen && (uint32_t)(nowMs - touchDownMs) < CF_TOUCH_RECENT_MS) return downY;
  return gestureY;
}

static inline bool cfInSessionZone(uint16_t startY) { return startY >= CF_SWIPE_ZONE_Y; }

static inline bool cfIsTap(int16_t dx, int16_t dy, uint32_t heldMs) {
  return (int32_t)dx * dx + (int32_t)dy * dy <= CF_TAP_SLOP_PX * CF_TAP_SLOP_PX &&
         heldMs < CF_TAP_MAX_MS;
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
// Power: hold + backlight.
// ---------------------------------------------------------------------------
#define CF_BRIGHT_HOLD_MS 10000UL   // 100 % after state change / touch / raise

// Keep the watch awake (no dim / screensaver / screen-off) while the Claude
// face is showing and the bridge has spoken within CF_LINK_HOLD_MS.
static inline bool cfShouldHold(bool onClaudeFace, bool everValid,
                                uint32_t nowMs, uint32_t lastValidMs,
                                uint32_t holdMs) {
  return onClaudeFace && everValid && (uint32_t)(nowMs - lastValidMs) < holdMs;
}

// Backlight duty (0..255) while held. BLOCKED forces full; otherwise full for
// CF_BRIGHT_HOLD_MS after the last trigger, then the ambient level.
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

// STATE edges worth lighting a dark screen for (link task -> power manager):
// something became BLOCKED, or a long turn just finished (the DONE buzz).
static inline bool cfWakeWorthy(bool havePrev, const CFStateMsg &prev,
                                const CFStateMsg &cur) {
  bool blockedNow  = cur.p_state == CF_STATE_BLOCKED || cur.blocked_count > 0;
  bool blockedPrev = havePrev &&
                     (prev.p_state == CF_STATE_BLOCKED || prev.blocked_count > 0);
  if (blockedNow && !blockedPrev) return true;
  if (cur.p_state == CF_STATE_DONE && cur.p_last_turn_s >= 60 &&
      (!havePrev || prev.p_state != CF_STATE_DONE))
    return true;
  return false;
}
