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
// drawBackButton / tappedBack / drawTitleBar / theme() / contrastFor() are all
// shared helpers declared in view.h — they read model.bgColor / fgColor /
// accentColor / lineColor so this view re-themes automatically.
static void drawTitle(const char *t) {
  drawTitleBar(t);
}
static uint32_t nowEpoch() {
  ModelLock lk;
  if (!model.rtcOk) return 0;
  return rtcEpochSec(model.year, model.month, model.day,
                     model.hour, model.minute, model.second);
}

// ---------- mode transitions ----------
void TimerView::onEnter() {
  if (gfx) { ThemeColors _t = theme(); gfx->fillScreen(_t.bg); };
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
void TimerView::enterSetting() { mode = Mode::Setting; { ThemeColors _t = theme(); gfx->fillScreen(_t.bg); }; firstDraw = true; }
void TimerView::enterRunning() { mode = Mode::Running; { ThemeColors _t = theme(); gfx->fillScreen(_t.bg); }; firstDraw = true;
                                 lastShownRemain = 0xFFFFFFFF; }
void TimerView::enterFired()   { mode = Mode::Fired;   { ThemeColors _t = theme(); gfx->fillScreen(_t.bg); }; firstDraw = true;
                                 lastBuzzMs = 0;
                                 alertCycleStartMs = millis();
                                 alertPulseStage   = 0;
                                 lastAlertFlip     = -1; }

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
      // Repeating triplet-then-pause buzz pattern. Each ~1.4 s cycle fires
      // three short pulses (at +0, +220, +440 ms) followed by a longer pause
      // so the alarm reads as a distinct rhythm, not a single buzz on a loop.
      {
        const uint32_t cycleMs  = 1400;
        const uint32_t pulseAt[3] = { 0, 220, 440 };
        uint32_t now = millis();
        uint32_t elapsed = now - alertCycleStartMs;
        if (elapsed >= cycleMs) {
          alertCycleStartMs = now;
          alertPulseStage   = 0;
          elapsed = 0;
        }
        while (alertPulseStage < 3 && elapsed >= pulseAt[alertPulseStage]) {
          hapticBuzz(220, 110);
          alertPulseStage++;
        }
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

  ThemeColors t = theme();
  static const char *kLbl[3] = { "HRS", "MIN", "SEC" };
  gfx->setTextSize(1);
  for (int c = 0; c < 3; c++) {
    int16_t x = COL_X(c) + (COL_W - (int16_t)strlen(kLbl[c]) * 6) / 2;
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(x, 52);
    gfx->print(kLbl[c]);
  }
  for (int c = 0; c < 3; c++) {
    int16_t x = COL_X(c);
    gfx->drawRoundRect(x, PLUS_Y,  COL_W, BTN_H, 6, t.line);
    gfx->drawRoundRect(x, MINUS_Y, COL_W, BTN_H, 6, t.line);
    gfx->setTextSize(3);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(x + COL_W / 2 - 9, PLUS_Y  + 10);
    gfx->print('+');
    gfx->setCursor(x + COL_W / 2 - 9, MINUS_Y + 10);
    gfx->print('-');
  }
  uint16_t startTxt = contrastFor(t.accent);
  gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, t.accent);
  gfx->setTextSize(3);
  gfx->setTextColor(startTxt, t.accent);
  gfx->setCursor(ACT_X + (ACT_W - 5 * 18) / 2, ACT_Y + 10);
  gfx->print("START");

  drawSettingDigits();
}
void TimerView::drawSettingDigits() {
  ThemeColors t = theme();
  gfx->fillRect(0, DIGITS_Y, W, 44, t.bg);
  gfx->setTextSize(5);
  gfx->setTextColor(YELLOW, t.bg);   // YELLOW kept as a "this is the value" cue
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
  ThemeColors th = theme();
  gfx->fillRect(0, 90, W, 90, th.bg);
  gfx->setTextSize(size);
  // Go red/urgent in the last 10 seconds; otherwise use the user's fg colour
  // so the countdown matches the watch face palette.
  gfx->setTextColor(remainSec <= 10 ? RED : th.fg, th.bg);
  int16_t tw = (int16_t)strlen(buf) * 6 * size;
  gfx->setCursor((W - tw) / 2, 110);
  gfx->print(buf);

  // CANCEL stays MAROON — destructive action, want it visually distinct from
  // the (accent-coloured) primary SAVE/START pattern.
  gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, MAROON);
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE, MAROON);
  gfx->setCursor(ACT_X + (ACT_W - 6 * 18) / 2, ACT_Y + 10);
  gfx->print("CANCEL");

  lastShownRemain = remainSec;
}

void TimerView::drawFired() {
  // Full-screen alert: the entire background flashes between RED and BLACK at
  // ~3 Hz to grab attention from across the room; "TIME UP" stays white,
  // centred and oversized. Only repaint when the flip phase actually changes
  // so we don't burn cycles redrawing identical frames.
  int8_t flip = (int8_t)(((millis() / 320) & 1) != 0);
  if (flip == lastAlertFlip && !firstDraw) return;
  lastAlertFlip = flip;

  uint16_t bg = flip ? RED : BLACK;
  gfx->fillScreen(bg);

  // Title — size 5 = 25 px wide / 40 tall per char. "TIME UP" is 7 chars,
  // so the centred block is 7 * 30 = 210 px, which fits the 240-wide screen
  // with a 15 px gutter each side. Size 6 wraps the "P" onto a new line.
  gfx->setTextSize(5);
  gfx->setTextColor(WHITE, bg);
  const char *msg = "TIME UP";
  int16_t tw = (int16_t)strlen(msg) * 6 * 5;
  gfx->setCursor((W - tw) / 2, 110);
  gfx->print(msg);

  // DISMISS strip: white pill on whichever background is current so it stays
  // tappable in both flash phases. Same hit rectangle as in setting/running
  // modes so the existing onEvent code still works.
  gfx->fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, WHITE);
  gfx->drawRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 8, BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(BLACK, WHITE);
  gfx->setCursor(ACT_X + (ACT_W - 7 * 18) / 2, ACT_Y + 10);
  gfx->print("DISMISS");

  // Back chevron in the corner, drawn each flip so it survives the fillScreen.
  drawBackButton();
  firstDraw = false;
}
