#include <Arduino_GFX_Library.h>
#include <string.h>
#include "FreeSansBold24pt7b.h"
#include "scrubmarine.h"
#include "display.h"     // gfx, frameCanvas, backlightSet
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model + ModelLock
#include "assets/smicon_bg.h"     // SMICON_BG   240x280 logo watermark

static const int16_t W = 240;
static const int16_t H = 280;

static const uint16_t COL_TIME = WHITE;
static const uint16_t COL_DATE = 0xC618;   // silver
static const uint16_t COL_BAT  = 0x8410;   // grey

// ---- shared drawing target: prefer the double-buffered canvas, fall back to
// painting straight to the panel if PSRAM is exhausted. ---------------------
static Arduino_GFX *beginFrame(Arduino_Canvas **cvOut) {
  Arduino_Canvas *cv = frameCanvas();
  *cvOut = cv;
  return cv ? (Arduino_GFX *)cv : gfx;
}
static void paintBackground(Arduino_GFX *g, Arduino_Canvas *cv) {
  if (cv) memcpy(cv->getFramebuffer(), SMICON_BG, sizeof(SMICON_BG));
  else    g->draw16bitRGBBitmap(0, 0, SMICON_BG, SMICON_BG_W, SMICON_BG_H);
}
static void endFrame(Arduino_Canvas *cv) {
  if (cv) cv->flush();
}

// Center a bitmap-font string of the given size on the panel width.
static int16_t centerBuiltin(const char *s, uint8_t size) {
  int16_t w = (int16_t)strlen(s) * 6 * size;
  int16_t x = (W - w) / 2;
  return x < 0 ? 0 : x;
}

// =====================================================================
// ScrubMarineFaceView
// =====================================================================
void ScrubMarineFaceView::onEnter() {
  forceDraw = true;
  if (gfx) gfx->fillScreen(BLACK);   // clean base before the first framed paint
}

void ScrubMarineFaceView::render() {
  if (!gfx) return;
  Model snap;
  { ModelLock lk; snap = model; }

  bool changed = forceDraw ||
                 snap.hour != lastH || snap.minute != lastM ||
                 snap.day != lastDay || snap.month != lastMon ||
                 snap.year != lastYear || snap.weekday != lastWd ||
                 snap.rtcOk != lastRtcOk ||
                 snap.batPct != lastBat || snap.batOk != lastBatOk;
  if (!changed) return;

  Arduino_Canvas *cv = nullptr;
  Arduino_GFX *g = beginFrame(&cv);
  paintBackground(g, cv);

  // Big time HH:MM.
  char buf[8];
  if (snap.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", snap.hour, snap.minute);
  else            strcpy(buf, "--:--");
  g->setFont(&FreeSansBold24pt7b);
  g->setTextSize(1);
  g->setTextColor(snap.rtcOk ? COL_TIME : RED);
  int16_t bx, by; uint16_t bw, bh;
  g->getTextBounds(buf, 0, 150, &bx, &by, &bw, &bh);
  g->setCursor((W - (int16_t)bw) / 2 - bx, 150);
  g->print(buf);
  g->setFont(nullptr);

  // Date line.
  if (snap.rtcOk) {
    static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *kMon[]  = { "???","Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec" };
    const char *wd = (snap.weekday < 7) ? kWday[snap.weekday] : "---";
    const char *mn = (snap.month >= 1 && snap.month <= 12) ? kMon[snap.month] : "???";
    char db[24];
    snprintf(db, sizeof(db), "%s %u %s %u", wd, snap.day, mn, snap.year);
    g->setTextSize(2);
    g->setTextColor(COL_DATE);
    g->setCursor(centerBuiltin(db, 2), 196);
    g->print(db);
  }

  // Battery readout, top-right.
  if (snap.batOk) {
    char bb[8];
    snprintf(bb, sizeof(bb), "%u%%", snap.batPct);
    g->setTextSize(1);
    g->setTextColor(COL_BAT);
    g->setCursor(W - (int16_t)strlen(bb) * 6 - 8, 8);
    g->print(bb);
  }

  endFrame(cv);

  forceDraw = false;
  lastH = snap.hour; lastM = snap.minute;
  lastDay = snap.day; lastMon = snap.month; lastYear = snap.year;
  lastWd = snap.weekday; lastRtcOk = snap.rtcOk;
  lastBat = snap.batPct; lastBatOk = snap.batOk;
}

void ScrubMarineFaceView::onEvent(const Event &e) {
  // Swipe left is the intentional way into the launcher (matches the shell).
  if (e.type == EventType::Gesture && e.gesture == Gesture::SwipeLeft) {
    hapticBuzz(60, 70);
    switchTo(Screen::AppList);
  }
}

// =====================================================================
// ScreenSaverView — the same large centred SM logo as the watch face, static,
// no time, and a little dimmer. A static image, so it's painted once per entry.
// =====================================================================

// Backlight while the screensaver is up: a fraction of the user's setting so
// it's noticeably softer than the watch face but not pitch black. Floored so a
// low brightness setting doesn't make it invisible.
static uint8_t saverBrightness() {
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  int dim = (int)br * 6 / 10;          // ~60% of the user's brightness
  if (dim < 24) dim = 24;
  return (uint8_t)dim;
}

// Input protection. Stray CST816S frames tend to be single, momentary blips, so
// we only accept a finger that stays down for a real dwell before dismissing.
static const uint16_t SAVER_GRACE_MS = 500;   // ignore all input just after entry
static const uint16_t SAVER_HOLD_MS  = 180;   // finger must rest this long to wake

void ScreenSaverView::onEnter() {
  drawn        = false;
  entryMs      = millis();
  pressActive  = false;
  pressStartMs = 0;
  if (gfx) gfx->fillScreen(BLACK);
  backlightSet(saverBrightness());
}

void ScreenSaverView::onExit() {
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  backlightSet(br);
}

void ScreenSaverView::render() {
  if (!gfx) return;
  if (drawn) return;                   // static — nothing changes frame to frame
  // Same watermark the watch face draws, just without the time drawn over it.
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)SMICON_BG, SMICON_BG_W, SMICON_BG_H);
  drawn = true;
}

void ScreenSaverView::onEvent(const Event &e) {
  // Swallow everything for a beat after the screensaver appears — this eats the
  // trailing touch frame from whatever the user last did, and any settling blip.
  if (millis() - entryMs < SAVER_GRACE_MS) {
    if (e.type == EventType::TouchUp) pressActive = false;
    return;
  }

  switch (e.type) {
    // The physical button is reliable — a press is always intentional.
    case EventType::ButtonDown:
    case EventType::ButtonShort:
      switchTo(Screen::Watch);
      return;

    // Touch must be *sustained*. A stray frame lands as a lone Touch with no
    // follow-up holds; a real finger keeps generating TouchHold at ~30 Hz.
    case EventType::Touch:
      pressActive  = true;
      pressStartMs = millis();
      return;
    case EventType::TouchHold:
      if (pressActive && (millis() - pressStartMs) >= SAVER_HOLD_MS) {
        switchTo(Screen::Watch);
      }
      return;
    case EventType::TouchUp:
      pressActive = false;
      return;

    // Deliberately ignored: ImuMotion (movement/vibration is not intent) and
    // bare gestures (touch-derived, so they can ride in on a stray frame).
    default:
      return;
  }
}
