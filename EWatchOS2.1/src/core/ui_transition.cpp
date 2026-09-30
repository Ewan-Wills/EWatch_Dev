// EWatchOS screen transitions — slide the outgoing frame away over the
// incoming one, Apple-style, using two PSRAM snapshots of the 240x280 frame.
//
// Contract: both the current and the target view implement paintTo(canvas)
// (paint a full frame, no flush). Flow:
//   1. outgoing paints into the canvas -> copy to snapA
//   2. registry switch (onExit/onEnter, model.screen update)
//   3. incoming paints into the canvas -> copy to snapB
//   4. ~250 ms: each frame compose snapA/snapB into the canvas at the eased
//      offset, flush. Rubber-free, tear-free (single flush per frame).
//   5. leave the incoming frame in the canvas; the view continues event-driven.
//
// If either view can't paintTo, or the snapshots can't be allocated, this
// degrades to a plain switchTo() cut — never a broken half-state.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include "display.h"
#include "view.h"
#include "model.h"
#include "ui_motion.h"
#include "ewlog.h"
#include "power_mgr.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const int16_t W = 240, H = 280;
static const uint32_t TRANS_MS = 240;

static uint16_t *snapA = nullptr;    // outgoing frame
static uint16_t *snapB = nullptr;    // incoming frame

static bool snapsReady() {
  if (snapA && snapB) return true;
  if (!snapA) snapA = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (!snapB) snapB = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (!snapA || !snapB) {
    EWLOGW("UI", "transition snapshots alloc failed (psram)");
    return false;
  }
  return true;
}

// Compose one transition frame into the canvas at progress p (0..1).
static void compose(uint16_t *fb, Trans t, float p) {
  int16_t off;
  switch (t) {
    case Trans::PushLeft: {
      // Incoming slides in from the right; outgoing slides out left at half
      // speed (parallax) and dims slightly — reads as depth.
      off = (int16_t)(W * (1.0f - p));                    // incoming x-origin
      int16_t offOut = -(int16_t)(W * p * 0.5f);          // outgoing x-origin
      for (int y = 0; y < H; y++) {
        uint16_t *row = fb + y * W;
        const uint16_t *ra = snapA + y * W;
        const uint16_t *rb = snapB + y * W;
        // Outgoing occupies [0, off): sample A shifted by offOut.
        for (int x = 0; x < off; x++) {
          int sx = x - offOut;
          row[x] = (sx < W) ? ra[sx] : 0;
        }
        // Incoming occupies [off, W).
        memcpy(row + off, rb, (size_t)(W - off) * 2);
      }
      break;
    }
    case Trans::PopRight: {
      // Mirror: outgoing slides right, incoming un-parallaxes from the left.
      off = (int16_t)(W * p);                             // outgoing x-origin
      int16_t offIn = -(int16_t)(W * (1.0f - p) * 0.5f);
      for (int y = 0; y < H; y++) {
        uint16_t *row = fb + y * W;
        const uint16_t *ra = snapA + y * W;
        const uint16_t *rb = snapB + y * W;
        for (int x = 0; x < off; x++) {
          int sx = x - offIn;
          row[x] = (sx < W) ? rb[sx] : 0;
        }
        memcpy(row + off, ra, (size_t)(W - off) * 2);
      }
      break;
    }
    case Trans::SheetUp: {
      // Incoming rises from the bottom over a static outgoing frame.
      off = (int16_t)(H * (1.0f - p));                    // incoming y-origin
      if (off > 0) memcpy(fb, snapA, (size_t)off * W * 2);
      memcpy(fb + off * W, snapB, (size_t)(H - off) * W * 2);
      break;
    }
    case Trans::SheetDown: {
      // Incoming drops from the top (control centre).
      off = (int16_t)(H * p);                             // rows of incoming visible
      memcpy(fb, snapB + (H - off) * W, (size_t)off * W * 2);
      if (off < H) memcpy(fb + off * W, snapA + off * W, (size_t)(H - off) * W * 2);
      break;
    }
    case Trans::CurtainUp: {
      // Outgoing lifts up and away; incoming sits beneath (quickset dismiss).
      off = (int16_t)(H * p);                             // rows of A gone
      if (off < H) memcpy(fb, snapA + off * W, (size_t)(H - off) * W * 2);
      if (off > 0) memcpy(fb + (H - off) * W, snapB + (H - off) * W, (size_t)off * W * 2);
      break;
    }
    case Trans::CurtainDown: {
      // Outgoing drops down and away; incoming beneath (launcher dismiss).
      off = (int16_t)(H * p);                             // rows of A pushed down
      if (off > 0) memcpy(fb, snapB, (size_t)off * W * 2);
      if (off < H) memcpy(fb + off * W, snapA, (size_t)(H - off) * W * 2);
      break;
    }
    default: break;
  }
}

void switchToAnimated(Screen s, Trans t) {
  Arduino_Canvas *cv = frameCanvas();
  View *from = currentView;
  View *to   = viewFor(s);
  if (t == Trans::None || !cv || !from || !to || to == from) {
    switchTo(s);
    return;
  }
  if (!snapsReady()) { switchTo(s); return; }

  // Breadcrumbs: EWLOG mirrors to RTC RAM, so if anything below hard-crashes
  // (the historical failure mode was a render-stack overflow — silent, no
  // backtrace) the next boot's mirror replay names the last stage reached.
  // hwm = this task's stack high-water mark in bytes free; if it trends
  // toward 0 the stack budget in controllerStartTasks() needs raising.

  // 1) Outgoing frame.
  if (!from->paintTo(cv)) { switchTo(s); return; }
  memcpy(snapA, cv->getFramebuffer(), (size_t)W * H * 2);

  // 2) The actual switch (identical bookkeeping to switchTo).
  from->onExit();
  { ModelLock lk; model.screen = s; model.revision++; }
  currentView = to;
  to->onEnter();

  // 3) Incoming frame. If the new view can't pre-paint (unexpected — caller
  // should only animate between aura views), fall back to its normal render.
  if (!to->paintTo(cv)) { to->render(); return; }
  memcpy(snapB, cv->getFramebuffer(), (size_t)W * H * 2);

  // 4) Animate. Blocking is fine: this runs on the render task, exactly like
  // the in-view animation loops, and feeds the WDT each frame. Hold 240 MHz —
  // entered from a settled 80 MHz screen, the governor won't wake up for us.
  powerPerfDemand(true);
  uint16_t *fb = cv->getFramebuffer();
  uint32_t t0 = millis();
  for (;;) {
    esp_task_wdt_reset();
    uint32_t el = millis() - t0;
    float p = el >= TRANS_MS ? 1.0f : easeOutCubic((float)el / TRANS_MS);
    compose(fb, t, p);
    cv->flush();
    if (p >= 1.0f) break;
  }
  // Canvas now holds the incoming frame verbatim (p=1 path memcpy'd B).
}
