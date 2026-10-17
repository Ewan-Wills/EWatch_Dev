// Claude Face — the face for the stand-alone firmware. See claude_face.h.
//
// LANDSCAPE: the watch sits on its side at the desk (physical top edge
// pointing RIGHT). The face draws a logical 280x240 image into a native
// 240x280 panel; see cf_orient.h for the maths.
//
// Rendering: the face draws into the full-frame PSRAM canvas with the canvas
// rotation set to CF_CANVAS_ROTATION for the duration of each draw call ONLY
// (RotationScope below). The framebuffer itself is always the native
// portrait panel image, so a full frame is a plain cv->flush(), and a dirty
// logical region is pushed as its panel rect (cfViewRectToPanel) through a
// small scratch buffer in bands of rows (cfPushPanelRect). Only regions whose
// cfRegionKey() changed are redrawn and pushed; the animated centre disc
// stays a small region. Nothing is ever cleared on the panel before being
// redrawn, so there is no flicker.
//
// Input. Touch points and CST816S swipe directions arrive in NATIVE portrait
// terms; they are converted into the viewer's frame first
// (cfChipGestureToPanelDir -> cfPanelDirToView, cf_policy.h / cf_orient.h).
//
//   chip gesture (AuroraOS name)  viewer swipe  action
//   ----------------------------  ------------  --------------------------------
//   0x03 SwipeLeft                UP            open the detail page
//   0x04 SwipeRight               DOWN          nothing
//   0x01 SwipeDown                LEFT          next session (SELECT_SESSION +1)
//   0x02 SwipeUp                  RIGHT         previous session (SELECT_SESSION -1)
//   any swipe, detail page open                 close the detail page
//   tap (touch-up within 14 px / 600 ms)        detail open: close it; else ACK
//                                               when the primary is DONE (unacked)
//                                               or anything is BLOCKED
//   long press (hold 0.7 s, or chip 0x0C)       toggle the detail page
//   side button, short press (< 1 s)            toggle the detail page
//   side button, hold 3 s                       power off (main.cpp)
//
// Session cycling needs a live link and at least two sessions; otherwise the
// swipe does nothing. The detail page closes itself after 30 s.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "claude_face.h"
#include "claude_face_draw.h"
#include "cf_link.h"
#include "cf_logic.h"
#include "cf_policy.h"
#include "cf_util.h"
#include "cf_board.h"
#include "cf_settings.h"
#include "haptic_roles.h"

// ---------------------------------------------------------------------------
// UI-loop state
// ---------------------------------------------------------------------------
static Arduino_Canvas *sCanvas = nullptr;
static CFEnv      sEnv;                  // last RTC / battery from the UI loop
static CFLinkSnap sSnap;                 // this loop's copy of the link snapshot
static uint32_t   sLastStateRxCount = 0;

// Haptics
static CFHapticState sHs;
static bool          sHsInit = false;
static uint32_t      sLastHapticTickMs = 0;
static volatile uint8_t sTestBuzz = 0;   // console -> UI loop

// Backlight
static bool     sBrightTrig = false;
static uint32_t sLastBrightMs = 0;
static bool     sHaveIdent = false;
static uint32_t sStateIdent = 0;

// Rendering
static const uint8_t kRegions[4] = { CF_RGN_STATUS, CF_RGN_HERO,
                                     CF_RGN_ACTIVITY, CF_RGN_FOOTER };
static uint32_t sKeys[4];
static bool     sForce = true;
static bool     sDrawn = false;
static uint32_t sLastAnimMs = 0;
static uint16_t sAnimFrameMs = 0;
static uint16_t *sScratch = nullptr;     // region push buffer (centre-disc sized)
static size_t    sScratchPx = 0;
static uint32_t  sPushes = 0, sFullFlushes = 0;

// Detail page
static bool     sDetail = false;
static uint32_t sDetailKey = 0;
static uint32_t sDetailOpenedMs = 0;

// Touch tracking (native panel coordinates)
static bool     sTouchSeen = false;
static uint32_t sTouchDownMs = 0;
static uint16_t sDownX = 0, sDownY = 0;
static bool     sLongFired = false;

// Counters for `cf stats`
static uint32_t sTaps = 0, sAcks = 0, sSelects = 0, sSwipes = 0;

// ---------------------------------------------------------------------------
// Status buzzes -> haptic roles (core/haptic_roles.cpp). Each role is one
// effect with its own rhythm, so no pulse sequencer is needed here.
// ---------------------------------------------------------------------------
static void playBuzz(CFBuzz b) {
  switch (b) {
    case CF_BUZZ_BLOCKED: haptic(HapticRole::Warning);  break;   // needs your permission
    case CF_BUZZ_DONE:    haptic(HapticRole::Confirm);  break;   // long turn finished
    case CF_BUZZ_CROSS:   haptic(HapticRole::Reminder); break;   // 5 h usage crossed 80 / 95 %
    case CF_BUZZ_RESET:   haptic(HapticRole::ToggleOn); break;   // limit window reset
    default: break;
  }
}

bool claudeFaceRequestTestBuzz(const char *which) {
  if (!which) return false;
  if      (strcmp(which, "blocked") == 0) sTestBuzz = CF_BUZZ_BLOCKED;
  else if (strcmp(which, "done")    == 0) sTestBuzz = CF_BUZZ_DONE;
  else if (strcmp(which, "cross")   == 0) sTestBuzz = CF_BUZZ_CROSS;
  else if (strcmp(which, "reset")   == 0) sTestBuzz = CF_BUZZ_RESET;
  else return false;
  return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void noteBright(uint32_t now) { sLastBrightMs = now; sBrightTrig = true; }

static bool linkLiveNow(uint32_t now) {
  return cfLinkLive(sSnap, now) && !cfLinkVersionMismatch(sSnap, now);
}

static void buildInput(CFDrawInput &in, uint32_t now) {
  memset(&in, 0, sizeof in);
  in.st = sSnap.st;
  in.ss = sSnap.ss;
  in.haveState    = sSnap.haveState;
  in.haveSessions = sSnap.haveSessions;
  in.rxAgeMs = sSnap.haveState ? (uint32_t)(now - sSnap.stateRxMs) : 0;
  in.link = !sSnap.everValid ? CF_LINK_NEVER
          : cfLinkLive(sSnap, now) ? CF_LINK_LIVE : CF_LINK_OFFLINE;
  in.versionMismatch = cfLinkVersionMismatch(sSnap, now);

  int16_t tz = settingsGet().tzOffsetMin;
  in.rtcOk = sEnv.rtcOk;
  in.hour = sEnv.hour;
  in.minute = sEnv.minute;
  in.utcOffsetMin = tz;
  in.batPct = sEnv.batPct;
  in.batOk = sEnv.batOk;
  if (sEnv.rtcOk) {
    // RTC holds LOCAL wall time (see cf_link.cpp applyTime).
    int64_t utc = cfEpochFromCivil(sEnv.year, sEnv.month, sEnv.day, sEnv.hour,
                                   sEnv.minute, sEnv.second) - (int64_t)tz * 60;
    in.nowEpoch = utc > 0 ? (uint32_t)utc : 0;
  }
  in.nowMs = now;
}

static void closeDetail() {
  if (!sDetail) return;
  sDetail = false;
  sForce = true;
}

static void openDetail(uint32_t now) {
  sDetail = true;
  sDetailOpenedMs = now;
  sForce = true;
}

static void toggleDetail(uint32_t now) {
  if (sDetail) closeDetail(); else openDetail(now);
  haptic(HapticRole::Open);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Canvas rotation for the duration of one draw call. Restores rotation 0 on
// scope exit, so flush() and the region pushes always see the native
// portrait framebuffer. On the direct-to-panel fallback (no PSRAM canvas)
// this sets the ST7789's own rotation (MADCTL), which Arduino_GFX keeps
// consistent with the canvas mapping.
class RotationScope {
public:
  explicit RotationScope(Arduino_GFX *g) : g_(g) { g_->setRotation(CF_CANVAS_ROTATION); }
  ~RotationScope() { g_->setRotation(0); }
private:
  Arduino_GFX *g_;
};

// Put one dirty LOGICAL region on the panel from the canvas framebuffer.
static void pushRegion(Arduino_Canvas *cv, CFRect logical) {
  if (!gfx) return;
  if (!sScratch) {
    // One buffer the size of the animated centre disc: the per-frame push is
    // a single SPI block; bigger regions go out in bands of rows.
    CFRect c = cfRegionRect(CF_RGN_CENTRE);
    size_t px = (size_t)c.w * (size_t)c.h;
    sScratch = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sScratch) sScratch = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    sScratchPx = sScratch ? px : 0;        // none: full-width bands instead
  }
  sPushes += cfPushPanelRect(cv->getFramebuffer(), cfViewRectToPanel(logical),
                             sScratch, sScratchPx,
                             [](int16_t x, int16_t y, uint16_t *px, int16_t w, int16_t h) {
                               gfx->draw16bitRGBBitmap(x, y, px, w, h);
                             });
}

static uint32_t detailKey(const CFDrawInput &in) {
  uint32_t k = 2166136261UL;
  uint32_t v[7] = { sSnap.stateRxCount, sSnap.ss.seq, in.minute, in.hour,
                    in.link, in.versionMismatch, in.rtcOk };
  for (int i = 0; i < 7; i++) { k ^= v[i]; k *= 16777619UL; }
  return k;
}

static void render(Arduino_Canvas *cv) {
  uint32_t now = millis();
  Arduino_GFX *target = cv ? (Arduino_GFX *)cv : gfx;
  if (!target) return;
  CFDrawInput in;
  buildInput(in, now);

  if (sDetail && (uint32_t)(now - sDetailOpenedMs) > CF_DETAIL_TIMEOUT_MS) closeDetail();
  if (sDetail) {
    sAnimFrameMs = 0;
    uint32_t k = detailKey(in);
    if (sForce || k != sDetailKey) {
      { RotationScope rot(target); cfDrawDetail(target, in); }
      if (cv) { cv->flush(); sFullFlushes++; }
      sDetailKey = k;
      sForce = false;
      sDrawn = true;
    }
    return;
  }

  uint8_t mask = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t k = cfRegionKey(in, kRegions[i]);
    if (sForce || k != sKeys[i]) { mask |= kRegions[i]; sKeys[i] = k; }
  }
  CFAnim a = cfAnimation(in);
  sAnimFrameMs = a.region ? a.frameMs : 0;
  // No slack needed: the UI loop passes every ~10 ms, so a frame lands at
  // most one pass after it is due.
  if (a.region && (uint32_t)(now - sLastAnimMs) >= a.frameMs) mask |= a.region;
  if (a.region && (mask & (a.region | CF_RGN_HERO))) sLastAnimMs = now;
  if (!mask) return;

  { RotationScope rot(target); cfDrawRegions(target, in, mask); }
  sDrawn = true;
  if (!cv) { sForce = false; return; }     // direct-to-panel fallback
  if (sForce) {
    cv->flush();                           // native framebuffer, whole panel
    sFullFlushes++;
  } else {
    CFRect rs[4];
    int n = cfDirtyRects(mask, rs);
    for (int i = 0; i < n; i++) pushRegion(cv, rs[i]);
  }
  sForce = false;
}

// ---------------------------------------------------------------------------
// Tick — every UI-loop pass.
// ---------------------------------------------------------------------------
void claudeFaceBegin(Arduino_Canvas *cv) {
  sCanvas = cv;
  sForce = true;
  noteBright(millis());                    // boot: 100 % for 10 s
}

void claudeFaceTick(const CFEnv &env) {
  uint32_t now = millis();
  sEnv = env;
  if (!sHsInit) { cfHapticInit(sHs); sHsInit = true; }

  cfLinkRead(sSnap);
  bool live = cfLinkLive(sSnap, now);
  bool mismatch = cfLinkVersionMismatch(sSnap, now);

  CFSettings set = settingsGet();
  CFHapticCfg cfg;
  cfg.enabled = set.haptics;
  cfg.quietStart = set.quietStart;
  cfg.quietEnd = set.quietEnd;
  // Unknown time: don't guess quiet hours (hour 12 is outside any sane
  // quiet window, so alerts still buzz).
  uint8_t localHour = env.rtcOk ? env.hour : 12;

  // ---- haptics ----
  CFBuzz bz = CF_BUZZ_NONE;
  if (sSnap.haveState && sSnap.stateRxCount != sLastStateRxCount && !mismatch) {
    sLastStateRxCount = sSnap.stateRxCount;
    bz = cfHapticStep(sHs, &sSnap.st, now, live, localHour, cfg);
    sLastHapticTickMs = now;
    uint32_t ident = cfStateIdentity(sSnap.st);
    if (!sHaveIdent || ident != sStateIdent) {
      sStateIdent = ident;
      sHaveIdent = true;
      noteBright(now);                     // 100 % for 10 s after a change
    }
  } else if ((uint32_t)(now - sLastHapticTickMs) >= 1000) {
    bz = cfHapticStep(sHs, nullptr, now, live, localHour, cfg);   // repeats
    sLastHapticTickMs = now;
  }
  if (sTestBuzz) {                         // `cf buzz ...` (bypasses quiet hours)
    bz = (CFBuzz)sTestBuzz;
    sTestBuzz = 0;
  }
  playBuzz(bz);

  // ---- draw (before the first backlight write: never light garbage) ----
  render(sCanvas);

  // ---- backlight: never off; 100 % after a trigger, else ambient ----
  if (sDrawn) {
    bool blocked = live && !mismatch && sSnap.haveState &&
                   (sSnap.st.global_state == CF_STATE_BLOCKED ||
                    sSnap.st.p_state == CF_STATE_BLOCKED ||
                    sSnap.st.blocked_count > 0);
    uint8_t duty = cfBacklightDuty(now, sLastBrightMs, sBrightTrig, blocked,
                                   set.ambientPct);
    uint8_t cur = backlightGet();
    if (duty > cur)      backlightSet(duty);          // brighten instantly
    else if (duty < cur) backlightFadeTo(duty, 250);  // settle gently
  }
  backlightService();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void cycleSession(int dir, uint32_t now) {
  if (!linkLiveNow(now) || !sSnap.haveSessions) return;
  uint8_t cur = sSnap.haveState ? sSnap.st.primary_index : 255;
  uint8_t idx = cfNextSessionIndex(cur, sSnap.ss.count, dir);
  if (idx == 255) return;
  cfHapticNoteSelect(sHs, now);
  cfLinkSendEvent(CF_EVENTCODE_SELECT_SESSION, idx);
  sSelects++;
  haptic(HapticRole::Tap);
}

static void onTap(uint32_t now) {
  sTaps++;
  if (sDetail) { closeDetail(); return; }
  if (!linkLiveNow(now) || !sSnap.haveState || !cfAckable(sSnap.st)) return;
  cfHapticAck(sHs);                        // stop BLOCKED repeats now
  cfLinkSendEvent(CF_EVENTCODE_ACK, 0);
  sAcks++;
  haptic(HapticRole::Confirm);
}

void claudeFaceTouchDown(uint16_t x, uint16_t y) {
  uint32_t now = millis();
  sTouchSeen = true;
  sTouchDownMs = now;
  sDownX = x; sDownY = y;
  sLongFired = false;
  noteBright(now);
}

void claudeFaceTouchHold(uint16_t x, uint16_t y) {
  uint32_t now = millis();
  noteBright(now);
  // Distances are rotation-invariant: no conversion needed for the slop.
  int16_t dx = (int16_t)x - (int16_t)sDownX;
  int16_t dy = (int16_t)y - (int16_t)sDownY;
  if (sTouchSeen && !sLongFired &&
      (uint32_t)(now - sTouchDownMs) >= CF_LONGPRESS_MS && cfIsTap(dx, dy, 0)) {
    sLongFired = true;
    toggleDetail(now);
  }
}

void claudeFaceTouchUp(uint16_t x, uint16_t y) {
  uint32_t now = millis();
  if (sLongFired || !sTouchSeen) { sTouchSeen = false; return; }
  sTouchSeen = false;
  int16_t dx = (int16_t)x - (int16_t)sDownX;
  int16_t dy = (int16_t)y - (int16_t)sDownY;
  if (cfIsTap(dx, dy, now - sTouchDownMs)) onTap(now);
}

void claudeFaceGesture(uint8_t g) {
  uint32_t now = millis();
  noteBright(now);
  if (g == CF_G_LONG_PRESS) {
    if (!sLongFired) { sLongFired = true; toggleDetail(now); }
    return;
  }
  uint8_t panelDir = cfChipGestureToPanelDir(g);
  if (panelDir == CF_DIR_NONE) return;     // taps are decided on touch-up
  sSwipes++;
  sTouchSeen = false;                      // this touch is spent
  switch (cfRouteSwipe(cfPanelDirToView(panelDir), sDetail)) {
    case CF_ACT_CLOSE_DETAIL: closeDetail(); break;
    case CF_ACT_SESSION_NEXT: cycleSession(+1, now); break;
    case CF_ACT_SESSION_PREV: cycleSession(-1, now); break;
    case CF_ACT_OPEN_DETAIL:  openDetail(now); haptic(HapticRole::Open); break;
    default: break;
  }
}

void claudeFaceButtonShort() {
  uint32_t now = millis();
  noteBright(now);
  toggleDetail(now);
}

void claudeFaceWake() { noteBright(millis()); }

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
void claudeFaceStatsPrint(Stream &out) {
  out.printf("  face backlight=%u detail=%d anim_ms=%u pushes=%lu full_flushes=%lu "
             "taps=%lu acks=%lu selects=%lu swipes=%lu\n",
             backlightGet(), sDetail ? 1 : 0, sAnimFrameMs,
             (unsigned long)sPushes, (unsigned long)sFullFlushes,
             (unsigned long)sTaps, (unsigned long)sAcks,
             (unsigned long)sSelects, (unsigned long)sSwipes);
  out.printf("  haptics blocked=%lu done=%lu cross=%lu reset=%lu suppressed=%lu "
             "blk_active=%d blk_acked=%d blk_fires=%u\n",
             (unsigned long)sHs.played[CF_BUZZ_BLOCKED],
             (unsigned long)sHs.played[CF_BUZZ_DONE],
             (unsigned long)sHs.played[CF_BUZZ_CROSS],
             (unsigned long)sHs.played[CF_BUZZ_RESET],
             (unsigned long)sHs.suppressed, sHs.blkActive ? 1 : 0,
             sHs.blkAcked ? 1 : 0, sHs.blkFires);
}
