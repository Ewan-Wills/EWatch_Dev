// StopwatchView — count-up display + Start/Stop/Reset. The 1 Hz refresh is
// driven by the controller's Tick events; we just redraw the changed region.
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>

#include "stopwatch.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "apptimer.h"

static const int16_t W = 240, H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

// Button pad.
static const int16_t BTN_Y  = 198, BTN_H = 60, BTN_W = 100;
static const int16_t BTN1_X = 16, BTN2_X = W - 16 - BTN_W;

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

// Current RTC epoch (0 if the clock isn't readable).
static uint32_t nowEpoch() {
  ModelLock lk;
  if (!model.rtcOk) return 0;
  return rtcEpochSec(model.year, model.month, model.day,
                     model.hour, model.minute, model.second);
}

void StopwatchView::onEnter() {
  if (gfx) gfx->fillScreen(BLACK);
  firstDraw = true;
  lastShownElapsed = 0xFFFFFFFF;
  lastShownRunning = !stopwatchRunning();   // force first button draw
}

void StopwatchView::render() {
  if (!gfx) return;
  if (firstDraw) {
    drawBackButton();
    gfx->setTextSize(2);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(60, 14);
    gfx->print("Stopwatch");
    gfx->drawFastHLine(20, 44, 200, DARKGREY);
    firstDraw = false;
  }
  uint32_t elapsed = stopwatchElapsedMs(nowEpoch(), millis());
  bool running = stopwatchRunning();

  if (elapsed != lastShownElapsed || running != lastShownRunning) {
    drawTime(elapsed, running);
    lastShownElapsed = elapsed;
  }
  if (running != lastShownRunning) {
    drawButtons(running);
    lastShownRunning = running;
  }
}

void StopwatchView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }

  uint32_t ep = nowEpoch();
  uint32_t ms = millis();
  if (inRect(e.x, e.y, BTN1_X, BTN_Y, BTN_W, BTN_H)) {       // Start / Stop
    if (stopwatchRunning()) stopwatchStop(ep, ms);
    else                    stopwatchStart(ep, ms);
    hapticBuzz(60, 60);
    lastShownRunning = !stopwatchRunning();                  // force redraw
    lastShownElapsed = 0xFFFFFFFF;
    return;
  }
  if (inRect(e.x, e.y, BTN2_X, BTN_Y, BTN_W, BTN_H)) {       // Reset
    stopwatchReset();
    hapticBuzz(90, 70);
    lastShownRunning = !stopwatchRunning();
    lastShownElapsed = 0xFFFFFFFF;
    return;
  }
}

void StopwatchView::drawTime(uint32_t elapsedMs, bool running) {
  uint32_t totalSec = elapsedMs / 1000;
  uint32_t ms = elapsedMs % 1000;
  uint32_t hh = totalSec / 3600;
  uint32_t mm = (totalSec % 3600) / 60;
  uint32_t ss = totalSec % 60;
  char buf[20];
  uint8_t size;
  if (hh > 0) { snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu.%03lu",
                         (unsigned long)hh, (unsigned long)mm,
                         (unsigned long)ss, (unsigned long)ms);
                size = 3; }
  else        { snprintf(buf, sizeof(buf), "%02lu:%02lu.%03lu",
                         (unsigned long)mm, (unsigned long)ss,
                         (unsigned long)ms);
                size = 4; }
  gfx->fillRect(0, 90, W, 80, BLACK);
  gfx->setTextSize(size);
  gfx->setTextColor(running ? GREEN : WHITE, BLACK);
  int16_t tw = (int16_t)strlen(buf) * 6 * size;
  gfx->setCursor((W - tw) / 2, 116);
  gfx->print(buf);
}

void StopwatchView::drawButtons(bool running) {
  uint16_t c1 = running ? MAROON : DARKGREEN;
  const char *l1 = running ? "Stop" : "Start";
  gfx->fillRoundRect(BTN1_X, BTN_Y, BTN_W, BTN_H, 8, c1);
  gfx->drawRoundRect(BTN1_X, BTN_Y, BTN_W, BTN_H, 8, WHITE);
  gfx->setTextColor(WHITE, c1);
  gfx->setTextSize(2);
  int16_t w1 = (int16_t)strlen(l1) * 12;
  gfx->setCursor(BTN1_X + (BTN_W - w1) / 2, BTN_Y + 22);
  gfx->print(l1);

  gfx->fillRoundRect(BTN2_X, BTN_Y, BTN_W, BTN_H, 8, NAVY);
  gfx->drawRoundRect(BTN2_X, BTN_Y, BTN_W, BTN_H, 8, WHITE);
  gfx->setTextColor(WHITE, NAVY);
  int16_t w2 = (int16_t)strlen("Reset") * 12;
  gfx->setCursor(BTN2_X + (BTN_W - w2) / 2, BTN_Y + 22);
  gfx->print("Reset");
}
