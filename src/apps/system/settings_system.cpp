// System settings pages: Sleep, Display, Font, Haptics, Memory.
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "storage.h"
#include "controller.h"      // requestMmaMotionReconfig (Wake page)
#include "system_views.h"

static const int16_t W = 240;

// Named color palette used by the Display settings page. Indexed; the model
// stores the RGB565 value, but the page steps through this list.
struct ColorChoice { const char *name; uint16_t color; };
static const ColorChoice kColors[] = {
  { "Black",   BLACK    },
  { "White",   WHITE    },
  { "Red",     RED      },
  { "Orange",  ORANGE   },
  { "Yellow",  YELLOW   },
  { "Green",   GREEN    },
  { "Cyan",    CYAN     },
  { "Blue",    BLUE     },
  { "Magenta", MAGENTA  },
  { "Navy",    NAVY     },
  { "Maroon",  MAROON   },
  { "Purple",  PURPLE   },
  { "DkGreen", DARKGREEN},
  { "DkGrey",  DARKGREY },
  { "LtGrey",  LIGHTGREY},
};
static const int kColorCount = sizeof(kColors) / sizeof(kColors[0]);
static uint8_t colorIndexOf(uint16_t c) {
  for (int i = 0; i < kColorCount; i++) if (kColors[i].color == c) return i;
  return 0;
}

// =====================================================================
// SettingsSleep — auto-sleep timeout, IMU wake threshold, wake-source toggles.
// =====================================================================
class SettingsSleepView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      saverSec     = model.screensaverTimeoutSec;
      timeoutSec   = model.sleepTimeoutSec;
      toOffSec     = model.sleepToOffSec;
      wkTouch = model.wakeOnTouch;
      wkBtn   = model.wakeOnButton;
      wkImu   = model.wakeOnImu; }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Sleep");
      drawRowChrome(ROW0_Y, "Watch -> saver (s)");
      drawRowChrome(ROW1_Y, "Screen off (s)");
      drawRowChrome(ROW2_Y, "Sleep -> power off (s)");

      gfx->setTextSize(1);
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(20, TOG_Y - 10);
      gfx->print("Wake on");

      { ThemeColors _t = theme(); gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, _t.accent); }
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(contrastFor(_t.accent), _t.accent); }
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & VALUE)   { drawValues();  dirty &= ~VALUE; }
    if (dirty & TOGGLES) { drawToggles(); dirty &= ~TOGGLES; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    // Hit-test +/- columns. row 0 = saver, 1 = screen off, 2 = sleep->off.
    int col = -1; int8_t sign = 0;
    int16_t rowY[] = { ROW0_Y, ROW1_Y, ROW2_Y };
    for (int r = 0; r < 3; r++) {
      if      (uiInRect(e.x, e.y, MINUS_X, rowY[r], BTN_W, BTN_H)) { col = r; sign = -1; break; }
      else if (uiInRect(e.x, e.y, PLUS_X,  rowY[r], BTN_W, BTN_H)) { col = r; sign = +1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bumpRow(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      // Wake-source toggle row
      for (int i = 0; i < 3; i++) {
        if (uiInRect(e.x, e.y, TOG_X(i), TOG_Y, TOG_W, TOG_H)) {
          if (i == 0) wkTouch = !wkTouch;
          if (i == 1) wkBtn   = !wkBtn;
          if (i == 2) wkImu   = !wkImu;
          dirty |= TOGGLES; hapticBuzz(40, 50);
          return;
        }
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk;
          model.screensaverTimeoutSec = saverSec;
          model.sleepTimeoutSec  = timeoutSec;
          model.sleepToOffSec    = toOffSec;
          model.wakeOnTouch      = wkTouch;
          model.wakeOnButton     = wkBtn;
          model.wakeOnImu        = wkImu;
          model.revision++; }
        Storage::save();
        hapticBuzz(120, 70);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold && tickHold(col, sign)) {
      bumpRow(col, sign);
    }
  }
private:
  static const int16_t ROW0_Y   = 56;
  static const int16_t ROW1_Y   = 102;
  static const int16_t ROW2_Y   = 148;
  static const int16_t BTN_H    = 36;
  static const int16_t BTN_W    = 50;
  static const int16_t MINUS_X  = 20;
  static const int16_t PLUS_X   = 170;
  static const int16_t TOG_Y    = 196;
  static const int16_t TOG_H    = 22;
  static const int16_t TOG_W    = 70;
  static const int16_t ACT_Y    = 230;
  static const int16_t ACT_H    = 30;
  static const int16_t ACT_W    = 200;
  static const int16_t SAVE_X   = 20;
  static int16_t TOG_X(int i) { return 9 + i * (TOG_W + 6); }

  enum DirtyFlags { TITLE = 1, VALUE = 2, TOGGLES = 4, ALL = 7 };

  uint16_t saverSec     = 20;
  uint16_t timeoutSec   = 5;
  uint16_t toOffSec     = 30;
  bool     wkTouch = true, wkBtn = true, wkImu = false;
  uint8_t  dirty = ALL;

  void drawRowChrome(int16_t y, const char *label) {
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(20, y - 10);
    gfx->print(label);
    gfx->drawRoundRect(MINUS_X, y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->drawRoundRect(PLUS_X,  y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->setTextSize(3);
    { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
    gfx->setCursor(MINUS_X + BTN_W / 2 - 9, y + 4);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 9, y + 4);
    gfx->print('+');
  }

  // Step screensaver delay in 5-second increments, range 0 (off) ... 120, wraps.
  void bumpSaver(int8_t d) {
    if (d > 0) saverSec = (saverSec >= 120) ? 0 : saverSec + 5;
    else       saverSec = (saverSec == 0)  ? 120 : saverSec - 5;
    dirty |= VALUE;
  }
  // Step idle timeout in 5-second increments, range 0 (off) ... 300, wraps.
  void bumpTimeout(int8_t d) {
    if (d > 0) timeoutSec = (timeoutSec >= 300) ? 0 : timeoutSec + 5;
    else       timeoutSec = (timeoutSec == 0)  ? 300 : timeoutSec - 5;
    dirty |= VALUE;
  }
  // Step sleep->off in 10-second increments, range 0 (never) ... 600, wraps.
  void bumpToOff(int8_t d) {
    if (d > 0) toOffSec = (toOffSec >= 600) ? 0 : toOffSec + 10;
    else       toOffSec = (toOffSec == 0)  ? 600 : toOffSec - 10;
    dirty |= VALUE;
  }
  void bumpRow(int col, int8_t d) {
    if (col == 0) bumpSaver(d);
    if (col == 1) bumpTimeout(d);
    if (col == 2) bumpToOff(d);
  }
  void drawValues() {
    int16_t valX = MINUS_X + BTN_W;
    int16_t valW = PLUS_X - valX;
    gfx->fillRect(valX, ROW0_Y, valW, BTN_H, BLACK);
    gfx->fillRect(valX, ROW1_Y, valW, BTN_H, BLACK);
    gfx->fillRect(valX, ROW2_Y, valW, BTN_H, BLACK);

    gfx->setTextSize(3);
    gfx->setTextColor(YELLOW, BLACK);

    auto centerPrint = [&](int16_t y, const char *s) {
      int16_t w = (int16_t)strlen(s) * 18;
      gfx->setCursor(valX + (valW - w) / 2, y + 6);
      gfx->print(s);
    };

    char buf[8];
    if (saverSec == 0) snprintf(buf, sizeof(buf), "off");
    else               snprintf(buf, sizeof(buf), "%us", saverSec);
    centerPrint(ROW0_Y, buf);

    if (timeoutSec == 0) snprintf(buf, sizeof(buf), "off");
    else                 snprintf(buf, sizeof(buf), "%us", timeoutSec);
    centerPrint(ROW1_Y, buf);

    if (toOffSec == 0) snprintf(buf, sizeof(buf), "never");
    else               snprintf(buf, sizeof(buf), "%us", toOffSec);
    centerPrint(ROW2_Y, buf);
  }
  void drawToggle(int i, const char *label, bool on) {
    int16_t x = TOG_X(i);
    uint16_t fg = on ? DARKGREEN : DARKGREY;
    gfx->fillRoundRect(x, TOG_Y, TOG_W, TOG_H, 4, fg);
    gfx->setTextColor(WHITE, fg);
    gfx->setTextSize(1);
    int16_t lblw = (int16_t)strlen(label) * 6;
    gfx->setCursor(x + (TOG_W - lblw) / 2, TOG_Y + 8);
    gfx->print(label);
  }
  void drawToggles() {
    drawToggle(0, "Touch",  wkTouch);
    drawToggle(1, "Button", wkBtn);
    drawToggle(2, "IMU",    wkImu);
  }
};

// =====================================================================
// SettingsWake — wake behaviour tuning:
//   * Touch protect: dwell a finger must hold to wake from the screensaver
//     (filters phantom CST816S frames). 0 = wake instantly.
//   * Touch wake: sensitivity for waking the dark screen by touch —
//     any (every INT pulse) / tap (real event required) / firm (finger must
//     still be down at validation).
//   * Raise wake: motion sensitivity for lifting the wrist out of light
//     sleep (MMA8451 TRANSIENT -> INT2). 0 = raise-to-wake off.
//   * Deep jolt: sharpness of the jolt needed to wake from deep sleep
//     (MMA8451 TRANSIENT -> INT1).
// =====================================================================
class SettingsWakeView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      touchHold = model.touchWakeHoldMs;
      sense     = model.touchWakeSense;
      raiseThr  = model.motionWakeThreshold;
      joltThr   = model.imuWakeThreshold; }
    if (sense > 2) sense = 2;
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Wake", 8, 32, 20, 200);
      drawRowChrome(ROW0_Y, "Touch protect (ms)");
      drawRowChrome(ROW1_Y, "Touch wake");
      drawRowChrome(ROW2_Y, "Raise wake (g)");
      drawRowChrome(ROW3_Y, "Deep jolt (g)");
      { ThemeColors _t = theme(); gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, _t.accent); }
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(contrastFor(_t.accent), _t.accent); }
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & VALUE) { drawValues(); dirty &= ~VALUE; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    int col = -1; int8_t sign = 0;
    int16_t rowY[] = { ROW0_Y, ROW1_Y, ROW2_Y, ROW3_Y };
    for (int r = 0; r < 4; r++) {
      if      (uiInRect(e.x, e.y, MINUS_X, rowY[r], BTN_W, BTN_H)) { col = r; sign = -1; break; }
      else if (uiInRect(e.x, e.y, PLUS_X,  rowY[r], BTN_W, BTN_H)) { col = r; sign = +1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bumpRow(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk;
          model.touchWakeHoldMs     = touchHold;
          model.touchWakeSense      = sense;
          model.motionWakeThreshold = raiseThr;
          model.imuWakeThreshold    = joltThr;
          model.revision++; }
        Storage::save();
        requestMmaMotionReconfig();     // apply the new raise threshold live
        hapticBuzz(120, 70);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold && tickHold(col, sign)) {
      bumpRow(col, sign);
    }
  }
private:
  // Compact 4-row layout (title pushed up to make room).
  static const int16_t ROW0_Y   = 48;
  static const int16_t ROW1_Y   = 98;
  static const int16_t ROW2_Y   = 148;
  static const int16_t ROW3_Y   = 198;
  static const int16_t BTN_H    = 34;
  static const int16_t BTN_W    = 50;
  static const int16_t MINUS_X  = 20;
  static const int16_t PLUS_X   = 170;
  static const int16_t ACT_Y    = 244;
  static const int16_t ACT_H    = 30;
  static const int16_t ACT_W    = 200;
  static const int16_t SAVE_X   = 20;

  enum DirtyFlags { TITLE = 1, VALUE = 2, ALL = 3 };

  uint16_t touchHold = 100;
  uint8_t  sense     = 0;
  uint8_t  raiseThr  = 0x08;
  uint8_t  joltThr   = 0x20;
  uint8_t  dirty = ALL;

  void drawRowChrome(int16_t y, const char *label) {
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(20, y - 10);
    gfx->print(label);
    gfx->drawRoundRect(MINUS_X, y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->drawRoundRect(PLUS_X,  y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->setTextSize(3);
    { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
    gfx->setCursor(MINUS_X + BTN_W / 2 - 9, y + 6);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 9, y + 6);
    gfx->print('+');
  }

  // Touch protect: 0 (off) .. 400 ms in 20 ms steps, wraps.
  void bumpTouch(int8_t d) {
    if (d > 0) touchHold = (touchHold >= 400) ? 0 : touchHold + 20;
    else       touchHold = (touchHold == 0)  ? 400 : touchHold - 20;
    dirty |= VALUE;
  }
  // Touch wake sensitivity: any (0) / tap (1) / firm (2), wraps.
  void bumpSense(int8_t d) {
    sense = (uint8_t)((sense + 3 + d) % 3);
    dirty |= VALUE;
  }
  // Raise wake: 0 (off), then 0x02..0x20 (~0.13..2.0 g) in 0x02 steps, wraps.
  void bumpRaise(int8_t d) {
    int v = (int)raiseThr + (d > 0 ? 2 : -2);
    if (v < 0)    v = 0x20;
    if (v > 0x20) v = 0;
    raiseThr = (uint8_t)v;
    dirty |= VALUE;
  }
  // Deep jolt: 0x08 (0.5 g) .. 0x40 (~4.0 g) in 0x04 (~0.25 g) steps, wraps.
  void bumpJolt(int8_t d) {
    int v = (int)joltThr + (d > 0 ? 4 : -4);
    if (v < 0x08) v = 0x40;
    if (v > 0x40) v = 0x08;
    joltThr = (uint8_t)v;
    dirty |= VALUE;
  }
  void bumpRow(int col, int8_t d) {
    if (col == 0) bumpTouch(d);
    if (col == 1) bumpSense(d);
    if (col == 2) bumpRaise(d);
    if (col == 3) bumpJolt(d);
  }
  void drawValues() {
    int16_t valX = MINUS_X + BTN_W;
    int16_t valW = PLUS_X - valX;
    gfx->fillRect(valX, ROW0_Y, valW, BTN_H, BLACK);
    gfx->fillRect(valX, ROW1_Y, valW, BTN_H, BLACK);
    gfx->fillRect(valX, ROW2_Y, valW, BTN_H, BLACK);
    gfx->fillRect(valX, ROW3_Y, valW, BTN_H, BLACK);

    gfx->setTextSize(3);
    gfx->setTextColor(YELLOW, BLACK);
    auto centerPrint = [&](int16_t y, const char *s) {
      int16_t w = (int16_t)strlen(s) * 18;
      gfx->setCursor(valX + (valW - w) / 2, y + 6);
      gfx->print(s);
    };

    char buf[8];
    if (touchHold == 0) snprintf(buf, sizeof(buf), "off");
    else                snprintf(buf, sizeof(buf), "%u", touchHold);
    centerPrint(ROW0_Y, buf);

    static const char *kSense[3] = { "any", "tap", "firm" };
    centerPrint(ROW1_Y, kSense[sense % 3]);

    if (raiseThr == 0) snprintf(buf, sizeof(buf), "off");
    else               snprintf(buf, sizeof(buf), "%.2f", raiseThr * 0.063f);
    centerPrint(ROW2_Y, buf);

    snprintf(buf, sizeof(buf), "%.2f", joltThr * 0.063f);
    centerPrint(ROW3_Y, buf);
  }
};

// =====================================================================
// SettingsDisplay — backlight brightness + watch face colors. Brightness is
// applied live so the user sees the change while adjusting; colors take
// effect on save.
// =====================================================================
class SettingsDisplayView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      brightness = model.brightness;
      bgIdx      = colorIndexOf(model.bgColor);
      fgIdx      = colorIndexOf(model.fgColor);
      accentIdx  = colorIndexOf(model.accentColor);
      lineIdx    = colorIndexOf(model.lineColor); }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Display");
      ThemeColors t = theme();
      uint16_t fg = t.fg, acc = t.accent;
      drawRowChrome(ROW0_Y, "Brightness");
      drawRowChrome(ROW1_Y, "Background");
      drawRowChrome(ROW2_Y, "Text");
      drawRowChrome(ROW3_Y, "Accent");
      drawRowChrome(ROW4_Y, "Lines");
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, acc);
      gfx->setTextSize(2);
      gfx->setTextColor(fg, acc);
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 8);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & VALUE) { drawValues(); dirty &= ~VALUE; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) {
      restoreBrightness();
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      restoreBrightness();
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    int col = -1; int8_t sign = 0;
    int16_t rowY[] = { ROW0_Y, ROW1_Y, ROW2_Y, ROW3_Y, ROW4_Y };
    for (int r = 0; r < 5; r++) {
      if (uiInRect(e.x, e.y, MINUS_X, rowY[r], BTN_W, BTN_H)) { col = r; sign = -1; break; }
      if (uiInRect(e.x, e.y, PLUS_X,  rowY[r], BTN_W, BTN_H)) { col = r; sign = +1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bumpRow(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk;
          model.brightness   = brightness;
          model.bgColor      = kColors[bgIdx].color;
          model.fgColor      = kColors[fgIdx].color;
          model.accentColor  = kColors[accentIdx].color;
          model.lineColor    = kColors[lineIdx].color;
          model.revision++; }
        Storage::save();
        hapticBuzz(120, 70);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold && tickHold(col, sign)) {
      bumpRow(col, sign);
    }
  }
private:
  // Compact 5-row layout — fits Brightness + 4 colour pickers + SAVE on 280 px.
  static const int16_t ROW0_Y  = 58;
  static const int16_t ROW1_Y  = 96;
  static const int16_t ROW2_Y  = 134;
  static const int16_t ROW3_Y  = 172;
  static const int16_t ROW4_Y  = 210;
  static const int16_t BTN_H   = 28;
  static const int16_t BTN_W   = 44;
  static const int16_t MINUS_X = 14;
  static const int16_t PLUS_X  = 182;
  static const int16_t ACT_Y   = 246;
  static const int16_t ACT_H   = 30;
  static const int16_t ACT_W   = 200;
  static const int16_t SAVE_X  = 20;

  enum DirtyFlags { TITLE = 1, VALUE = 2, ALL = 3 };

  uint8_t brightness = 200;
  uint8_t bgIdx     = 0;
  uint8_t fgIdx     = 1;
  uint8_t accentIdx = 0;
  uint8_t lineIdx   = 0;
  uint8_t dirty     = ALL;

  void drawRowChrome(int16_t y, const char *label) {
    uint16_t bg, fg, line;
    { ModelLock lk; bg = model.bgColor; fg = model.fgColor; line = model.lineColor; }
    gfx->setTextSize(1);
    gfx->setTextColor(line, bg);
    gfx->setCursor(20, y - 9);
    gfx->print(label);
    gfx->drawRoundRect(MINUS_X, y, BTN_W, BTN_H, 6, line);
    gfx->drawRoundRect(PLUS_X,  y, BTN_W, BTN_H, 6, line);
    gfx->setTextSize(2);
    gfx->setTextColor(fg, bg);
    gfx->setCursor(MINUS_X + BTN_W / 2 - 6, y + 6);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 6, y + 6);
    gfx->print('+');
  }

  void bumpBrightness(int8_t d) {
    int v = brightness + (d > 0 ? 16 : -16);
    if (v < 16)  v = 16;       // keep panel visible
    if (v > 255) v = 255;
    brightness = (uint8_t)v;
    backlightSet(brightness);  // live preview
    dirty |= VALUE;
  }
  void bumpColor(uint8_t &idx, int8_t d) {
    idx = (uint8_t)((idx + kColorCount + (d > 0 ? 1 : -1)) % kColorCount);
    dirty |= VALUE;
  }
  void bumpRow(int row, int8_t d) {
    if (row == 0) bumpBrightness(d);
    if (row == 1) bumpColor(bgIdx,     d);
    if (row == 2) bumpColor(fgIdx,     d);
    if (row == 3) bumpColor(accentIdx, d);
    if (row == 4) bumpColor(lineIdx,   d);
  }
  void restoreBrightness() {
    uint8_t saved;
    { ModelLock lk; saved = model.brightness; }
    backlightSet(saved);
  }
  void drawValues() {
    int16_t valX = MINUS_X + BTN_W;
    int16_t valW = PLUS_X - valX;
    uint16_t bg;
    { ModelLock lk; bg = model.bgColor; }

    gfx->fillRect(valX, ROW0_Y, valW, BTN_H, bg);
    gfx->setTextSize(2);
    gfx->setTextColor(YELLOW, bg);
    char buf[8];
    int pct = (brightness * 100 + 127) / 255;
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int16_t bw = (int16_t)strlen(buf) * 12;
    gfx->setCursor(valX + (valW - bw) / 2, ROW0_Y + 6);
    gfx->print(buf);

    drawColorVal(ROW1_Y, valX, valW, kColors[bgIdx]);
    drawColorVal(ROW2_Y, valX, valW, kColors[fgIdx]);
    drawColorVal(ROW3_Y, valX, valW, kColors[accentIdx]);
    drawColorVal(ROW4_Y, valX, valW, kColors[lineIdx]);
  }
  void drawColorVal(int16_t y, int16_t valX, int16_t valW, const ColorChoice &c) {
    uint16_t bg, fg, line;
    { ModelLock lk; bg = model.bgColor; fg = model.fgColor; line = model.lineColor; }
    gfx->fillRect(valX, y, valW, BTN_H, bg);
    int16_t sw = 20;
    int16_t sx = valX + 4;
    int16_t sy = y + (BTN_H - sw) / 2;
    gfx->fillRect(sx, sy, sw, sw, c.color);
    gfx->drawRect(sx, sy, sw, sw, line);
    gfx->setTextSize(1);
    gfx->setTextColor(fg, bg);
    gfx->setCursor(sx + sw + 4, y + 10);
    gfx->print(c.name);
  }
};

// =====================================================================
// SettingsFont — watch face style picker. Tapping selects + saves immediately
// so the user can switch back to the watch face and see the new look.
// =====================================================================
class SettingsFontView : public View {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      int n = watchFaceStyleCount();
      sel = (model.watchFaceStyle < (uint8_t)n) ? model.watchFaceStyle : 0; }
    firstDraw = true;
  }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawTitleBar("Font");
      drawAllCells();
      firstDraw = false;
    }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::Settings); return; }
    int n = watchFaceStyleCount();
    for (int i = 0; i < n; i++) {
      int16_t cx, cy;
      cellPos(i, cx, cy);
      if (uiInRect(e.x, e.y, cx, cy, CELL_W, CELL_H)) {
        sel = (uint8_t)i;
        { ModelLock lk; model.watchFaceStyle = sel; model.revision++; }
        Storage::save();
        hapticBuzz(60, 60);
        drawAllCells();
        return;
      }
    }
  }
private:
  uint8_t sel = 0;
  bool    firstDraw = true;

  // 2-column grid so all the style names fit on the 280-tall screen.
  // (Compact cells — 11 styles = 6 rows must fit below the title bar.)
  static const int16_t CELL_W   = 100;
  static const int16_t CELL_H   = 30;
  static const int16_t GRID_X   = 16;
  static const int16_t GRID_Y   = 56;
  static const int16_t GAP_X    = 8;
  static const int16_t GAP_Y    = 6;

  static void cellPos(int i, int16_t &x, int16_t &y) {
    int col = i & 1;            // 0 = left, 1 = right
    int row = i >> 1;
    x = GRID_X + col * (CELL_W + GAP_X);
    y = GRID_Y + row * (CELL_H + GAP_Y);
  }

  void drawAllCells() {
    int n = watchFaceStyleCount();
    for (int i = 0; i < n; i++) drawCell(i, (uint8_t)i == sel);
  }
  void drawCell(int i, bool active) {
    int16_t x, y; cellPos(i, x, y);
    uint16_t bg = active ? DARKGREEN : DARKGREY;
    gfx->fillRoundRect(x, y, CELL_W, CELL_H, 6, bg);
    gfx->drawRoundRect(x, y, CELL_W, CELL_H, 6, WHITE);
    const char *nm = watchFaceStyleName(i);
    gfx->setTextSize(2);
    gfx->setTextColor(WHITE, bg);
    int16_t lw = (int16_t)strlen(nm) * 12;
    gfx->setCursor(x + (CELL_W - lw) / 2, y + 8);
    gfx->print(nm);
  }
};

// =====================================================================
// SettingsHaptics — vibration strength slider (0..100%). A preview buzz
// fires on each step so the user can feel the new level live.
// =====================================================================
class SettingsHapticsView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk; strength = model.hapticStrength; }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Haptics");
      drawRowChrome();
      ThemeColors th = theme();
      uint16_t saveTxt = contrastFor(th.accent);
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, th.accent);
      gfx->setTextSize(2);
      gfx->setTextColor(saveTxt, th.accent);
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & VALUE) { drawValue(); dirty &= ~VALUE; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) {
      // Restore the saved strength so the live preview doesn't stick.
      uint8_t saved; { ModelLock lk; saved = model.hapticStrength; }
      hapticSetStrengthPct(saved);
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      uint8_t saved; { ModelLock lk; saved = model.hapticStrength; }
      hapticSetStrengthPct(saved);
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    int8_t sign = 0;
    if (uiInRect(e.x, e.y, MINUS_X, ROW_Y, BTN_W, BTN_H))      sign = -1;
    else if (uiInRect(e.x, e.y, PLUS_X, ROW_Y, BTN_W, BTN_H))  sign = +1;

    if (e.type == EventType::Touch) {
      if (sign != 0) {
        bump(sign);
        // Preview the new strength so the user can feel the difference live.
        hapticSetStrengthPct(strength);
        hapticBuzz(200, 60);
        startHold(0, sign);
        return;
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk; model.hapticStrength = strength; model.revision++; }
        Storage::save();
        hapticSetStrengthPct(strength);
        hapticBuzz(160, 80);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold) {
      int dummy = 0;
      if (tickHold(dummy, sign)) { bump(sign); hapticSetStrengthPct(strength); }
    }
  }
private:
  static const int16_t ROW_Y   = 96;
  static const int16_t BTN_H   = 48;
  static const int16_t BTN_W   = 56;
  static const int16_t MINUS_X = 16;
  static const int16_t PLUS_X  = 240 - 16 - BTN_W;
  static const int16_t VAL_X   = MINUS_X + BTN_W;
  static const int16_t VAL_W   = PLUS_X - VAL_X;
  static const int16_t ACT_Y   = 200;
  static const int16_t ACT_H   = 44;
  static const int16_t ACT_W   = 200;
  static const int16_t SAVE_X  = 20;

  enum DirtyFlags { TITLE = 1, VALUE = 2, ALL = 3 };
  uint8_t strength = 100;
  uint8_t dirty = ALL;

  void bump(int8_t d) {
    int v = (int)strength + d * 10;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    strength = (uint8_t)v;
    dirty |= VALUE;
  }
  void drawRowChrome() {
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(20, ROW_Y - 14);
    gfx->print("Strength");
    gfx->drawRoundRect(MINUS_X, ROW_Y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->drawRoundRect(PLUS_X,  ROW_Y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->setTextSize(4);
    { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
    gfx->setCursor(MINUS_X + BTN_W / 2 - 12, ROW_Y + 10);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 12, ROW_Y + 10);
    gfx->print('+');
  }
  void drawValue() {
    gfx->fillRect(VAL_X, ROW_Y, VAL_W, BTN_H, BLACK);
    char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)strength);
    gfx->setTextSize(4);
    gfx->setTextColor(YELLOW, BLACK);
    int16_t bw = (int16_t)strlen(buf) * 24;
    gfx->setCursor(VAL_X + (VAL_W - bw) / 2, ROW_Y + 10);
    gfx->print(buf);
  }
};

// =====================================================================
// SettingsMemory — live readout of internal heap, PSRAM, and app-flash use.
// =====================================================================
class SettingsMemoryView : public View {
public:
  void onEnter() override {
    uiClearAll();
    firstDraw = true;
    lastUpdateMs = 0;
  }
  uint16_t desiredFrameMs() const override { return 500; }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawTitleBar("Memory");
      firstDraw = false;
    }
    // Update every 500 ms so we don't burn cycles redrawing on every event.
    if (millis() - lastUpdateMs < 500) return;
    lastUpdateMs = millis();

    uint32_t heapTot  = ESP.getHeapSize();
    uint32_t heapUsed = heapTot - ESP.getFreeHeap();
    uint32_t psrTot   = ESP.getPsramSize();
    uint32_t psrUsed  = psrTot - ESP.getFreePsram();
    uint32_t skTot    = ESP.getSketchSize() + ESP.getFreeSketchSpace();
    uint32_t skUsed   = ESP.getSketchSize();

    drawRow( 56, "Heap",     heapUsed, heapTot, CYAN);
    drawRow(128, "PSRAM",    psrUsed,  psrTot,  MAGENTA);
    drawRow(200, "App flash",skUsed,   skTot,   YELLOW);
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::Settings); return;
    }
  }
private:
  bool     firstDraw = true;
  uint32_t lastUpdateMs = 0;

  void drawRow(int16_t y, const char *label,
               uint32_t used, uint32_t total, uint16_t color) {
    // Erase the row each refresh.
    gfx->fillRect(0, y, W, 60, BLACK);

    gfx->setTextSize(2);
    { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
    gfx->setCursor(14, y);
    gfx->print(label);

    uint32_t pct = total ? (uint32_t)((uint64_t)used * 100 / total) : 0;
    char buf[40];
    snprintf(buf, sizeof(buf), "%lu/%luk  %lu%%",
             (unsigned long)(used / 1024),
             (unsigned long)(total / 1024),
             (unsigned long)pct);
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(14, y + 22);
    gfx->print(buf);

    int16_t barX = 14, barY = y + 36, barW = W - 28, barH = 14;
    gfx->drawRect(barX, barY, barW, barH, DARKGREY);
    int16_t fill = total
        ? (int16_t)((uint64_t)used * (barW - 2) / total)
        : (int16_t)0;
    if (fill < 0) fill = 0;
    if (fill > barW - 2) fill = barW - 2;
    gfx->fillRect(barX + 1, barY + 1, fill, barH - 2, color);
  }
};

static SettingsSleepView    sSleep;
static SettingsWakeView     sWake;
static SettingsDisplayView  sDisplay;
static SettingsFontView     sFont;
static SettingsHapticsView  sHaptics;
static SettingsMemoryView   sMemory;
View *settingsSleepViewPtr()   { return &sSleep; }
View *settingsWakeViewPtr()    { return &sWake; }
View *settingsDisplayViewPtr() { return &sDisplay; }
View *settingsFontViewPtr()    { return &sFont; }
View *settingsHapticsViewPtr() { return &sHaptics; }
View *settingsMemoryViewPtr()  { return &sMemory; }
