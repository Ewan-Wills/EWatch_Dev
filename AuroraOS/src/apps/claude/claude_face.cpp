// Claude Face — watch-face glue. See claude_face.h for the contract.
//
// Rendering: the face draws into AuroraOS' shared PSRAM frame canvas
// (display.h frameCanvas()) and pushes ONLY the regions whose cfRegionKey()
// changed — full-width row bands straight out of the canvas framebuffer, the
// hero centre disc via a small scratch copy. The animated region is redrawn
// at cfAnimation().frameMs, the clock when its key (HH:MM) changes. Nothing
// is ever cleared on the panel before being redrawn, so there is no flicker.
//
// Gestures (decided with WatchFaceView; see cf_policy.h for the platform
// quirks around swipe-right):
//   swipe L/R starting at y >= 175 -> cycle primary session (SELECT_SESSION)
//   swipe L/R starting above      -> unchanged (left = next face,
//                                    right = launcher, as on every face)
//   swipe up / down               -> unchanged (launcher / control centre)
//   tap                           -> ACK when primary is DONE / BLOCKED
//   long press (hold 0.7 s)       -> detail page; tap / swipe / button closes
//   side button                   -> unchanged (launcher), closes detail
// The WiFi corner tap of the other faces is not active on this face (its
// footer occupies that corner, and this feature doesn't use WiFi).
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
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "view.h"
#include "power_mgr.h"
#include "apptimer.h"       // rtcEpochSec (seconds since 2000, local time)
#include "ewlog.h"

static const int16_t kW = 240, kH = 280;

// ---------------------------------------------------------------------------
// Render-task state
// ---------------------------------------------------------------------------
static CFLinkSnap sSnap;                 // this task's copy of the link snapshot
static uint32_t   sLastStateRxCount = 0;

// Haptics
static CFHapticState sHs;
static bool          sHsInit = false;
static uint32_t      sLastHapticTickMs = 0;
static volatile uint8_t sTestBuzz = 0;   // console -> render task

// Power / backlight
static bool     sHeld = false;
static bool     sPolicyActive = false;
static bool     sBrightTrig = false;
static uint32_t sLastBrightMs = 0;
static bool     sHaveIdent = false;
static uint32_t sStateIdent = 0;
static uint32_t sHoldCount = 0;

// Face rendering
static const uint8_t kRegions[4] = { CF_RGN_STATUS, CF_RGN_HERO,
                                     CF_RGN_ACTIVITY, CF_RGN_FOOTER };
static uint32_t sKeys[4];
static bool     sForce = true;
static uint32_t sLastAnimMs = 0;
static uint16_t sAnimFrameMs = 0;
static uint16_t *sScratch = nullptr;     // centre-disc push buffer
static size_t    sScratchPx = 0;
static uint32_t  sPushes = 0, sFullFlushes = 0;

// Detail page
static bool     sDetail = false;
static uint32_t sDetailKey = 0;
static uint32_t sDetailOpenedMs = 0;
static const uint32_t kDetailTimeoutMs = 30000;

// Gesture tracking
static bool     sTouchSeen = false;
static uint32_t sTouchDownMs = 0;
static uint16_t sDownX = 0, sDownY = 0;
static bool     sBtnSinceTouch = false;
static bool     sLongFired = false;

// ---------------------------------------------------------------------------
// Haptic pattern player. hapticBuzz() queues back-to-back with no gap (and
// only 4 deep), so multi-pulse patterns are sequenced by a one-shot
// esp_timer — timing stays right even while the render task is busy.
// ---------------------------------------------------------------------------
struct CFPulse { uint8_t intensity; uint16_t onMs; uint16_t gapMs; };
static const CFPulse kShort  = { 200,  70, 140 };
static const CFPulse kMedium = { 230, 240,   0 };
static CFPulse           sSeq[3];
static volatile uint8_t  sSeqN = 0, sSeqI = 0;
static esp_timer_handle_t sSeqTimer = nullptr;

static void seqStep(void *) {
  if (sSeqI >= sSeqN) return;
  CFPulse p = sSeq[sSeqI];
  sSeqI = (uint8_t)(sSeqI + 1);
  hapticBuzz(p.intensity, p.onMs);        // strength / silent scaling inside
  if (sSeqI < sSeqN && sSeqTimer)
    esp_timer_start_once(sSeqTimer, (uint64_t)(p.onMs + p.gapMs) * 1000ULL);
}

static void playBuzz(CFBuzz b) {
  if (b == CF_BUZZ_NONE) return;
  if (!sSeqTimer) {
    esp_timer_create_args_t a = {};
    a.callback = &seqStep;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "cfbuzz";
    if (esp_timer_create(&a, &sSeqTimer) != ESP_OK) sSeqTimer = nullptr;
  }
  if (sSeqI < sSeqN) return;               // a pattern is still playing
  uint8_t n = 0;
  switch (b) {
    case CF_BUZZ_BLOCKED: sSeq[0] = sSeq[1] = sSeq[2] = kShort; n = 3; break;
    case CF_BUZZ_DONE:    sSeq[0] = kMedium; n = 1; break;
    case CF_BUZZ_CROSS:   sSeq[0] = sSeq[1] = kShort; n = 2; break;
    case CF_BUZZ_RESET:   sSeq[0] = kShort; n = 1; break;
    default: return;
  }
  sSeqI = 0;
  sSeqN = n;
  seqStep(nullptr);                        // first pulse now, rest on the timer
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

  uint16_t yr; uint8_t mo, dy, hh, mm, ss; bool ok; int16_t tz;
  { ModelLock lk;
    ok = model.rtcOk; yr = model.year; mo = model.month; dy = model.day;
    hh = model.hour; mm = model.minute; ss = model.second;
    tz = model.tzOffsetMin;
    in.batPct = model.batPct; in.batOk = model.batOk; }
  in.rtcOk = ok;
  in.hour = hh;
  in.minute = mm;
  in.utcOffsetMin = tz;
  if (ok) {
    // RTC holds LOCAL time counted from 2000 (see cf_link.cpp / NTP sync).
    int64_t utc = (int64_t)rtcEpochSec(yr, mo, dy, hh, mm, ss) +
                  (int64_t)CF_UNIX_2000 - (int64_t)tz * 60;
    in.nowEpoch = utc > 0 ? (uint32_t)utc : 0;
  }
  in.nowMs = now;
}

static void closeDetail() {
  if (!sDetail) return;
  sDetail = false;
  sForce = true;
}

static void toggleDetail(uint32_t now) {
  sDetail = !sDetail;
  sDetailOpenedMs = now;
  sForce = true;
  hapticBuzz(20, 22);
}

// ---------------------------------------------------------------------------
// Tick — every render-task loop, whatever the screen.
// ---------------------------------------------------------------------------
void claudeFaceTick() {
  uint32_t now = millis();
  if (!sHsInit) { cfHapticInit(sHs); sHsInit = true; }

  cfLinkRead(sSnap);
  bool live = cfLinkLive(sSnap, now);
  bool mismatch = cfLinkVersionMismatch(sSnap, now);

  CFHapticCfg cfg;
  uint8_t hour, ambient, face, brightness;
  bool rtcOk;
  Screen scr;
  { ModelLock lk;
    cfg.enabled = model.cfHaptics;
    cfg.quietStart = model.cfQuietStart;
    cfg.quietEnd = model.cfQuietEnd;
    hour = model.hour; rtcOk = model.rtcOk;
    ambient = model.cfAmbientPct;
    face = model.watchFaceStyle;
    brightness = model.brightness;
    scr = model.screen; }
  // Unknown time: don't guess quiet hours (hour 12 is outside any sane
  // quiet window, so alerts still buzz).
  uint8_t localHour = rtcOk ? hour : 12;

  // ---- haptics (notifier: runs on every screen) ----
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

  // ---- power hold + backlight (only while the Claude face shows) ----
  bool onClaude = (scr == Screen::Watch) && (face == CF_FACE_INDEX);
  bool hold = cfShouldHold(onClaude, sSnap.everValid, now, sSnap.lastValidMs,
                           CF_LINK_HOLD_MS);
  if (hold != sHeld) {
    sHeld = hold;
    powerHoldAwake(hold);
    if (hold) { sHoldCount++; noteBright(now); }
    EWLOGI("CF", "hold_awake=%d", hold ? 1 : 0);
  }
  if (hold) {
    sPolicyActive = true;
    if (powerState() == PowerState::Active) {
      bool blocked = live && !mismatch && sSnap.haveState &&
                     (sSnap.st.global_state == CF_STATE_BLOCKED ||
                      sSnap.st.p_state == CF_STATE_BLOCKED ||
                      sSnap.st.blocked_count > 0);
      uint8_t duty = cfBacklightDuty(now, sLastBrightMs, sBrightTrig, blocked, ambient);
      uint8_t cur = backlightGet();
      if (duty > cur)      backlightSet(duty);          // brighten instantly
      else if (duty < cur) backlightFadeTo(duty, 250);  // settle gently
    }
  } else if (sPolicyActive) {
    sPolicyActive = false;
    // Hand the backlight back to AuroraOS at the user's level; the normal
    // dim / screensaver / off progression takes over from here.
    if (powerState() == PowerState::Active) backlightSet(brightness);
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void pushRect(Arduino_Canvas *cv, CFRect r) {
  if (!gfx) return;
  if (r.x < 0) { r.w += r.x; r.x = 0; }
  if (r.y < 0) { r.h += r.y; r.y = 0; }
  if (r.x + r.w > kW) r.w = kW - r.x;
  if (r.y + r.h > kH) r.h = kH - r.y;
  if (r.w <= 0 || r.h <= 0) return;
  uint16_t *fb = cv->getFramebuffer();
  sPushes++;
  if (r.x == 0 && r.w == kW) {             // contiguous row band
    gfx->draw16bitRGBBitmap(0, r.y, fb + (size_t)r.y * kW, kW, r.h);
    return;
  }
  size_t px = (size_t)r.w * (size_t)r.h;
  if (px > sScratchPx) {
    if (sScratch) heap_caps_free(sScratch);
    sScratch = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sScratch) sScratch = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    sScratchPx = sScratch ? px : 0;
  }
  if (!sScratch) {                         // no buffer: push the full-width band
    gfx->draw16bitRGBBitmap(0, r.y, fb + (size_t)r.y * kW, kW, r.h);
    return;
  }
  for (int16_t y = 0; y < r.h; y++)
    memcpy(sScratch + (size_t)y * r.w, fb + (size_t)(r.y + y) * kW + r.x,
           (size_t)r.w * 2);
  gfx->draw16bitRGBBitmap(r.x, r.y, sScratch, r.w, r.h);
}

static uint32_t detailKey(const CFDrawInput &in) {
  uint32_t k = 2166136261UL;
  uint32_t v[7] = { sSnap.stateRxCount, sSnap.ss.seq, in.minute, in.hour,
                    in.link, in.versionMismatch, in.rtcOk };
  for (int i = 0; i < 7; i++) { k ^= v[i]; k *= 16777619UL; }
  return k;
}

void claudeFaceRender(Arduino_Canvas *cv) {
  uint32_t now = millis();
  Arduino_GFX *target = cv ? (Arduino_GFX *)cv : gfx;
  if (!target) return;
  CFDrawInput in;
  buildInput(in, now);

  if (sDetail && (uint32_t)(now - sDetailOpenedMs) > kDetailTimeoutMs) closeDetail();
  if (sDetail) {
    sAnimFrameMs = 0;
    uint32_t k = detailKey(in);
    if (sForce || k != sDetailKey) {
      cfDrawDetail(target, in);
      if (cv) { cv->flush(); sFullFlushes++; }
      sDetailKey = k;
      sForce = false;
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
  // 15 ms slack: the render loop's own pacing lands frames a hair early.
  if (a.region && (uint32_t)(now - sLastAnimMs) + 15 >= a.frameMs) mask |= a.region;
  if (a.region && (mask & (a.region | CF_RGN_HERO))) sLastAnimMs = now;
  if (!mask) return;

  cfDrawRegions(target, in, mask);
  if (!cv) { sForce = false; return; }     // direct-to-panel fallback
  if (sForce) {
    cv->flush();
    sFullFlushes++;
  } else {
    if (mask & CF_RGN_STATUS)   pushRect(cv, cfRegionRect(CF_RGN_STATUS));
    if (mask & CF_RGN_HERO)     pushRect(cv, cfRegionRect(CF_RGN_HERO));
    else if (mask & CF_RGN_CENTRE) pushRect(cv, cfRegionRect(CF_RGN_CENTRE));
    if (mask & CF_RGN_ACTIVITY) pushRect(cv, cfRegionRect(CF_RGN_ACTIVITY));
    if (mask & CF_RGN_FOOTER)   pushRect(cv, cfRegionRect(CF_RGN_FOOTER));
  }
  sForce = false;
}

bool claudeFacePaintTo(Arduino_Canvas *cv) {
  if (!cv) return false;
  uint32_t now = millis();
  CFDrawInput in;
  buildInput(in, now);
  if (sDetail) {
    cfDrawDetail(cv, in);
    sDetailKey = detailKey(in);
  } else {
    cfDrawRegions(cv, in, CF_RGN_ALL);
    for (int i = 0; i < 4; i++) sKeys[i] = cfRegionKey(in, kRegions[i]);
    sLastAnimMs = now;
  }
  // The caller (transition / WatchFaceView) puts this exact frame on the
  // panel, so subsequent renders may push deltas only.
  sForce = false;
  return true;
}

uint16_t claudeFaceFrameMs() { return sDetail ? 0 : sAnimFrameMs; }

void claudeFaceOnEnter() {
  sForce = true;
  sDetail = false;
  sTouchSeen = false;
  sLongFired = false;
  noteBright(millis());
}

void claudeFaceOnExit() {
  sDetail = false;
  sForce = true;
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
  hapticBuzz(24, 30);
}

static void onTap(uint32_t now) {
  if (sDetail) { closeDetail(); return; }
  if (!linkLiveNow(now) || !sSnap.haveState || !cfAckable(sSnap.st)) return;
  cfHapticAck(sHs);                        // stop BLOCKED repeats now
  cfLinkSendEvent(CF_EVENTCODE_ACK, 0);
  hapticBuzz(24, 30);
}

bool claudeFaceOnEvent(const Event &e) {
  uint32_t now = millis();
  switch (e.type) {
    case EventType::Touch:
      sTouchSeen = true;
      sTouchDownMs = now;
      sDownX = e.x; sDownY = e.y;
      sBtnSinceTouch = false;
      sLongFired = false;
      noteBright(now);
      return true;

    case EventType::TouchHold: {
      noteBright(now);
      int16_t dx = (int16_t)e.x - (int16_t)sDownX;
      int16_t dy = (int16_t)e.y - (int16_t)sDownY;
      if (sTouchSeen && !sLongFired &&
          (uint32_t)(now - sTouchDownMs) >= CF_LONGPRESS_MS &&
          cfIsTap(dx, dy, 0)) {
        sLongFired = true;
        toggleDetail(now);
      }
      return true;
    }

    case EventType::TouchUp: {
      if (sLongFired || !sTouchSeen) return true;
      int16_t dx = (int16_t)e.x - (int16_t)sDownX;
      int16_t dy = (int16_t)e.y - (int16_t)sDownY;
      if (cfIsTap(dx, dy, now - sTouchDownMs)) onTap(now);
      return true;
    }

    case EventType::Gesture: {
      noteBright(now);
      uint16_t startY = cfGestureStartY(sTouchSeen, now, sTouchDownMs, sDownY, e.y);
      switch (e.gesture) {
        case Gesture::LongPress:
          if (!sLongFired) { sLongFired = true; toggleDetail(now); }
          return true;
        case Gesture::SingleTap:
        case Gesture::DoubleTap:
          return true;                     // taps are decided on TouchUp
        case Gesture::SwipeLeft:
          if (sDetail) { closeDetail(); return true; }
          // Session zone only once a bridge has ever spoken — until then
          // this is a plain clock and every swipe keeps its face behaviour.
          if (sSnap.everValid && cfInSessionZone(startY)) {
            cycleSession(+1, now);
            return true;
          }
          return false;                    // WatchFaceView: next face
        case Gesture::SwipeUp:
        case Gesture::SwipeDown:
          if (sDetail) { closeDetail(); return true; }
          return false;                    // launcher / control centre
        default:
          return sDetail;
      }
    }

    case EventType::ButtonDown:
      sBtnSinceTouch = true;
      noteBright(now);
      return false;

    case EventType::ButtonShort: {
      // Either the side button or a converted swipe-right (controller.cpp).
      bool swipe = cfButtonShortIsSwipe(sTouchSeen, sBtnSinceTouch, now, sTouchDownMs);
      if (sDetail) { closeDetail(); return true; }
      if (swipe && sSnap.everValid && cfInSessionZone(sDownY)) {
        cycleSession(-1, now);
        return true;
      }
      return false;                        // WatchFaceView: launcher
    }

    case EventType::ImuMotion:
      noteBright(now);                     // wrist raise -> 100 % for 10 s
      return false;

    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
void claudeFaceStatsPrint(Stream &out) {
  out.printf("  face held=%d holds=%lu policy=%d backlight=%u detail=%d "
             "anim_ms=%u pushes=%lu full_flushes=%lu\n",
             sHeld ? 1 : 0, (unsigned long)sHoldCount, sPolicyActive ? 1 : 0,
             backlightGet(), sDetail ? 1 : 0, sAnimFrameMs,
             (unsigned long)sPushes, (unsigned long)sFullFlushes);
  out.printf("  haptics blocked=%lu done=%lu cross=%lu reset=%lu suppressed=%lu "
             "blk_active=%d blk_acked=%d blk_fires=%u\n",
             (unsigned long)sHs.played[CF_BUZZ_BLOCKED],
             (unsigned long)sHs.played[CF_BUZZ_DONE],
             (unsigned long)sHs.played[CF_BUZZ_CROSS],
             (unsigned long)sHs.played[CF_BUZZ_RESET],
             (unsigned long)sHs.suppressed, sHs.blkActive ? 1 : 0,
             sHs.blkAcked ? 1 : 0, sHs.blkFires);
}
