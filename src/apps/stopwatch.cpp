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
// tappedBack / drawBackButton / drawTitleBar / theme() / contrastFor() are
// declared in view.h — they read model colours so the page re-themes
// automatically when the user changes the palette.

// Current RTC epoch (0 if the clock isn't readable).
static uint32_t nowEpoch() {
  ModelLock lk;
  if (!model.rtcOk) return 0;
  return rtcEpochSec(model.year, model.month, model.day,
                     model.hour, model.minute, model.second);
}

void StopwatchView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true;
  lastShownElapsed = 0xFFFFFFFF;
  lastShownRunning = !stopwatchRunning();   // force first button draw
}

void StopwatchView::render() {
  if (!gfx) return;
  if (firstDraw) {
    drawTitleBar("Stopwatch");
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
  ThemeColors t = theme();
  gfx->fillRect(0, 90, W, 80, t.bg);
  gfx->setTextSize(size);
  // GREEN stays as a "running" affordance; otherwise use the user's fg.
  gfx->setTextColor(running ? GREEN : t.fg, t.bg);
  int16_t tw = (int16_t)strlen(buf) * 6 * size;
  gfx->setCursor((W - tw) / 2, 116);
  gfx->print(buf);
}

void StopwatchView::drawButtons(bool running) {
  ThemeColors t = theme();
  // Stop = MAROON (destructive), Start = accent (primary action).
  uint16_t c1     = running ? MAROON : t.accent;
  uint16_t c1Txt  = running ? WHITE  : contrastFor(t.accent);
  const char *l1  = running ? "Stop" : "Start";
  gfx->fillRoundRect(BTN1_X, BTN_Y, BTN_W, BTN_H, 8, c1);
  gfx->drawRoundRect(BTN1_X, BTN_Y, BTN_W, BTN_H, 8, t.line);
  gfx->setTextColor(c1Txt, c1);
  gfx->setTextSize(2);
  int16_t w1 = (int16_t)strlen(l1) * 12;
  gfx->setCursor(BTN1_X + (BTN_W - w1) / 2, BTN_Y + 22);
  gfx->print(l1);

  // Reset = secondary button — accent on a darker theme variant by reusing
  // the line colour as the fill (keeps it visually distinct from Start/Stop).
  gfx->fillRoundRect(BTN2_X, BTN_Y, BTN_W, BTN_H, 8, t.line);
  gfx->drawRoundRect(BTN2_X, BTN_Y, BTN_W, BTN_H, 8, t.fg);
  gfx->setTextColor(contrastFor(t.line), t.line);
  int16_t w2 = (int16_t)strlen("Reset") * 12;
  gfx->setCursor(BTN2_X + (BTN_W - w2) / 2, BTN_Y + 22);
  gfx->print("Reset");
}
