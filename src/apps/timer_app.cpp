// TimerView — countdown timer with Setting / Running / Fired modes. The
// countdown is anchored to an RTC-epoch deadline in apptimer.cpp; the
// controller checks expiry and enterDeepSleep() arms a hardware wake-up, so
// the alarm fires even from sleep.
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>

#include "timer_app.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "apptimer.h"

static const int16_t W = 240, H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

// Setting-mode +/- columns.
static const int16_t COL_W = 64;
static int16_t COL_X(int c) { return 16 + c * (COL_W + 12); }
static const int16_t PLUS_Y  = 64,  BTN_H = 44;
static const int16_t DIGITS_Y = 116;
static const int16_t MINUS_Y = 168;
static const int16_t ACT_Y = 228, ACT_H = 44, ACT_X = 20, ACT_W = 200;

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
static bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}
static void drawBackButton() {
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(WHITE, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}
static void drawTitle(const char *t) {
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  int16_t tw = (int16_t)strlen(t) * 12;
  gfx->setCursor((W - tw) / 2, 14);
  gfx->print(t);
  gfx->drawFastHLine(20, 44, 200, DARKGREY);
}
static uint32_t nowEpoch() {
  ModelLock lk;
  if (!model.rtcOk) return 0;
  return rtcEpochSec(model.year, model.month, model.day,
                     model.hour, model.minute, model.second);
}

// ---------- mode transitions ----------
void TimerView::onEnter() {
  if (gfx) gfx->fillScreen(BLACK);
  if      (timerFired()) mode = Mode::Fired;
  else if (timerArmed()) mode = Mode::Running;
  else                   mode = Mode::Setting;

  if (mode == Mode::Setting && timerDurationSec() > 0) {
    uint32_t d = timerDurationSec();
    setH = (uint8_t)(d / 3600);
    setM = (uint8_t)((d % 3600) / 60);
    setS = (uint8_t)(d % 60);
  }
  firstDraw = true;
  lastShownRemain = 0xFFFFFFFF;
  lastBuzzMs = 0;
}
void TimerView::enterSetting() { mode = Mode::Setting; gfx->fillScreen(BLACK); firstDraw = true; }
void TimerView::enterRunning() { mode = Mode::Running; gfx->fillScreen(BLACK); firstDraw = true;
                                 lastShownRemain = 0xFFFFFFFF; }
void TimerView::enterFired()   { mode = Mode::Fired;   gfx->fillScreen(BLACK); firstDraw = true;
                                 lastBuzzMs = 0; }

// ---------- render ----------
void TimerView::render() {
  if (!gfx) return;

  // Transition into the alarm if the timer fired while we were on this page.
  if (mode == Mode::Running && timerFired()) { enterFired(); }

  switch (mode) {
    case Mode::Setting:
      if (firstDraw) { drawSetting(); firstDraw = false; }
      break;
    case Mode::Running: {
      if (firstDraw) { drawRunning(timerRemainingSec(nowEpoch())); firstDraw = false; }
      uint32_t rem = timerRemainingSec(nowEpoch());
      if (rem != lastShownRemain) { drawRunning(rem); }
      break;
    }
    case Mode::Fired:
      drawFired();
      // Pulse the motor roughly once a second until dismissed.
      if (millis() - lastBuzzMs > 900) {
        hapticBuzz(320, 200);
        lastBuzzMs = millis();
      }
      break;
  }
}

// ---------- events ----------
void TimerView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    if (mode == Mode::Fired)   { timerClearFired(); switchTo(Screen::AppList); return; }
    if (mode == Mode::Running) { switchTo(Screen::AppList); return; }   // leave it running
    switchTo(Screen::AppList);
    return;
  }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) {
    if (mode == Mode::Fired) timerClearFired();
    switchTo(Screen::AppList);
    return;
  }

  if (mode == Mode::Setting) {
    // +/- columns.
    for (int c = 0; c < 3; c++) {
      int16_t x = COL_X(c);
      if (inRect(e.x, e.y, x, PLUS_Y,  COL_W, BTN_H)) { bumpField(c, +1); hapticBuzz(35, 45);
                                                        drawSettingDigits(); return; }
      if (inRect(e.x, e.y, x, MINUS_Y, COL_W, BTN_H)) { bumpField(c, -1); hapticBuzz(35, 45);
                                                        drawSettingDigits(); return; }
    }
    // START.
    if (inRect(e.x, e.y, ACT_X, ACT_Y, ACT_W, ACT_H)) {
      uint32_t dur = (uint32_t)setH * 3600u + (uint32_t)setM * 60u + setS;
      if (dur == 0) { hapticBuzz(60, 40); return; }     // nothing to count
      timerArm(nowEpoch(), dur);
      hapticBuzz(120, 80);
      enterRunning();
    }
    return;
  }

  if (mode == Mode::Running) {
    // CANCEL.
    if (inRect(e.x, e.y, ACT_X, ACT_Y, ACT_W, ACT_H)) {
      timerCancel();
      hapticBuzz(100, 70);
      enterSetting();
    }
    return;
  }

  if (mode == Mode::Fired) {
    // DISMISS — anywhere in the action strip.
    if (inRect(e.x, e.y, ACT_X, ACT_Y, ACT_W, ACT_H)) {
      timerClearFired();
      hapticBuzz(80, 70);
      enterSetting();
    }
    return;
  }
}

// ---------- field edit ----------
void TimerView::bumpField(int col, int8_t dir) {
  if (col == 0) setH = (uint8_t)((setH + 24 + dir) % 24);
  if (col == 1) setM = (uint8_t)((setM + 60 + dir) % 60);
  if (col == 2) {                                  // seconds step in 5s
    int s = setS + dir * 5;
    if (s < 0)  s = 55;
    if (s > 59) s = 0;
    setS = (uint8_t)s;
  }
}

// ---------- drawing ----------
void TimerView::drawSetting() {
  drawBackButton();
  drawTitle("Timer");

  static const char *kLbl[3] = { "HRS", "MIN", "SEC" };
  gfx->setTextSize(1);
  for (int c = 0; c < 3; c++) {
    int16_t x = COL_X(c) + (COL_W - (int16_t)strlen(kLbl[c]) * 6) / 2;
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(x, 52);
    gfx->print(kLbl[c]);
  }
  for (int c = 0; c < 3; c++) {
    int16_t x = COL_X(c);
    gfx->drawRoundRect(x, PLUS_Y,  COL_W, BTN_H, 6, DARKGREY);
    gfx->drawRoundRect(x, MINUS_Y, COL_W, BTN_H, 6, DARKGREY);
    gfx->setTextSize(3);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(x + COL_W / 2 - 9, PLUS_Y  + 10);
    gfx->print('+');
    gfx->setCursor(x + COL_W / 2 - 9, MINUS_Y + 10);
    gfx->print('-');
  }
  gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, DARKGREEN);
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE, DARKGREEN);
  gfx->setCursor(ACT_X + (ACT_W - 5 * 18) / 2, ACT_Y + 10);
  gfx->print("START");

  drawSettingDigits();
}
void TimerView::drawSettingDigits() {
  gfx->fillRect(0, DIGITS_Y, W, 44, BLACK);
  gfx->setTextSize(5);
  gfx->setTextColor(YELLOW, BLACK);
  uint8_t vals[3] = { setH, setM, setS };
  char buf[4];
  for (int c = 0; c < 3; c++) {
    snprintf(buf, sizeof(buf), "%02u", vals[c]);
    int16_t x = COL_X(c) + (COL_W - 6 * 5 * 2) / 2;
    gfx->setCursor(x, DIGITS_Y);
    gfx->print(buf);
  }
}

void TimerView::drawRunning(uint32_t remainSec) {
  // Chrome is cheap to repaint and never flickers (identical pixels), so we
  // just redraw the whole page each second rather than tracking dirty rects.
  drawBackButton();
  drawTitle("Timer");

  uint32_t hh = remainSec / 3600;
  uint32_t mm = (remainSec % 3600) / 60;
  uint32_t ss = remainSec % 60;
  char buf[16];
  uint8_t size;
  if (hh > 0) { snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu",
                         (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
                size = 4; }
  else        { snprintf(buf, sizeof(buf), "%02lu:%02lu",
                         (unsigned long)mm, (unsigned long)ss);
                size = 6; }
  gfx->fillRect(0, 90, W, 90, BLACK);
  gfx->setTextSize(size);
  // Go red/urgent in the last 10 seconds.
  gfx->setTextColor(remainSec <= 10 ? RED : CYAN, BLACK);
  int16_t tw = (int16_t)strlen(buf) * 6 * size;
  gfx->setCursor((W - tw) / 2, 110);
  gfx->print(buf);

  gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, MAROON);
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE, MAROON);
  gfx->setCursor(ACT_X + (ACT_W - 6 * 18) / 2, ACT_Y + 10);
  gfx->print("CANCEL");

  lastShownRemain = remainSec;
}

void TimerView::drawFired() {
  // Flash the banner colour at ~3 Hz.
  bool flip = ((millis() / 300) & 1) != 0;
  uint16_t fg = flip ? RED : YELLOW;

  if (firstDraw) {
    drawBackButton();
    drawTitle("Timer");
    gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, DARKGREEN);
    gfx->setTextSize(3);
    gfx->setTextColor(WHITE, DARKGREEN);
    gfx->setCursor(ACT_X + (ACT_W - 7 * 18) / 2, ACT_Y + 10);
    gfx->print("DISMISS");
    firstDraw = false;
  }
  gfx->fillRect(0, 100, W, 70, BLACK);
  gfx->setTextSize(5);
  gfx->setTextColor(fg, BLACK);
  const char *msg = "TIME UP";
  int16_t tw = (int16_t)strlen(msg) * 6 * 5;
  gfx->setCursor((W - tw) / 2, 116);
  gfx->print(msg);
}
