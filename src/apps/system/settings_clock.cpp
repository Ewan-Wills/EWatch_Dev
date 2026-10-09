// SettingsTime + SettingsDate — set HH:MM:SS and D/M/Y on the RV-3028.
// Three +/- columns each with the hold-to-ramp UX. The other half of the
// timestamp is preserved by reading it from the model and writing it back.
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "controller.h"
#include "system_views.h"

static const int16_t W = 240;

// =====================================================================
// Shared helpers — leap years, days-in-month, and Sakamoto's weekday
// computation.
// =====================================================================
static bool kvIsLeap(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static uint8_t kvDaysInMonth(uint16_t y, uint8_t mo) {
  static const uint8_t kDays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (mo < 1 || mo > 12) return 31;
  uint8_t d = kDays[mo - 1];
  if (mo == 2 && kvIsLeap(y)) d = 29;
  return d;
}
// 0=Sunday..6=Saturday. Sakamoto's algorithm.
static uint8_t kvSakamoto(uint16_t y, uint8_t mo, uint8_t d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  int yy = y;
  if (mo < 3) yy -= 1;
  return (uint8_t)((yy + yy/4 - yy/100 + yy/400 + t[mo-1] + d) % 7);
}

// =====================================================================
// SettingsTime
// =====================================================================
class SettingsTimeView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      h = model.rtcOk ? model.hour   : 12;
      m = model.rtcOk ? model.minute : 0;
      s = model.rtcOk ? model.second : 0; }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Time");
      for (int c = 0; c < 3; c++) {
        int16_t x = COL_X(c);
        gfx->drawRoundRect(x, BTN_PLUS_Y,  COL_W, BTN_H, 6, DARKGREY);
        gfx->drawRoundRect(x, BTN_MINUS_Y, COL_W, BTN_H, 6, DARKGREY);
        gfx->setTextSize(3);
        { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
        gfx->setCursor(x + COL_W / 2 - 9, BTN_PLUS_Y + 12);
        gfx->print('+');
        gfx->setCursor(x + COL_W / 2 - 9, BTN_MINUS_Y + 12);
        gfx->print('-');
      }
      { ThemeColors _t = theme(); gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, _t.accent); }
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(contrastFor(_t.accent), _t.accent); }
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & DIGITS) { drawDigits(); dirty &= ~DIGITS; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    int col = -1; int8_t sign = 0;
    for (int c = 0; c < 3; c++) {
      int16_t x = COL_X(c);
      if (uiInRect(e.x, e.y, x, BTN_PLUS_Y,  COL_W, BTN_H)) { col = c; sign = +1; break; }
      if (uiInRect(e.x, e.y, x, BTN_MINUS_Y, COL_W, BTN_H)) { col = c; sign = -1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bump(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        // Preserve the model's current date so writeRTC doesn't clobber it.
        uint8_t  wd, dd, mo;
        uint16_t yr;
        { ModelLock lk;
          wd = model.weekday; dd = model.day;
          mo = model.month;   yr = model.year; }
        requestSetRTC(h, m, s, wd, dd, mo, yr);
        hapticBuzz(120, 70);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold && tickHold(col, sign)) {
      bump(col, sign);
    }
  }
private:
  static const int16_t BTN_PLUS_Y  = 60;
  static const int16_t BTN_H       = 50;
  static const int16_t DIGITS_Y    = 118;
  static const int16_t BTN_MINUS_Y = 168;
  static const int16_t ACT_Y       = 232;
  static const int16_t ACT_H       = 36;
  static const int16_t ACT_W       = 200;
  static const int16_t SAVE_X      = 20;
  static const int16_t COL_W       = 64;
  static int16_t COL_X(int c) { return 16 + c * (COL_W + 12); }

  enum DirtyFlags { TITLE = 1, DIGITS = 2, ALL = 3 };

  uint8_t  h = 12, m = 0, s = 0;
  uint8_t  dirty = ALL;

  void bump(int col, int8_t d) {
    if (col == 0) h = (h + 24 + d) % 24;
    if (col == 1) m = (m + 60 + d) % 60;
    if (col == 2) s = (s + 60 + d) % 60;
    dirty |= DIGITS;
  }
  void drawDigits() {
    gfx->fillRect(0, DIGITS_Y, W, 44, BLACK);
    gfx->setTextSize(5);
    gfx->setTextColor(YELLOW, BLACK);
    char buf[3];
    for (int c = 0; c < 3; c++) {
      uint8_t v = (c == 0) ? h : (c == 1) ? m : s;
      snprintf(buf, sizeof(buf), "%02u", v);
      int16_t x = COL_X(c) + (COL_W - 6 * 5 * 2) / 2;
      gfx->setCursor(x, DIGITS_Y);
      gfx->print(buf);
    }
  }
};

// =====================================================================
// SettingsDate — same chrome as the time page but the third column shows
// the last two digits of the year (stored full-width). Day clamps when
// month/year changes; the weekday register is recomputed on save.
// =====================================================================
class SettingsDateView : public View, RampPlusMinus {
public:
  void onEnter() override {
    uiClearAll();
    { ModelLock lk;
      day  = model.rtcOk ? model.day   : 1;
      mon  = model.rtcOk ? model.month : 1;
      year = model.rtcOk ? model.year  : 2026; }
    if (mon < 1 || mon > 12) mon = 1;
    if (day < 1) day = 1;
    uint8_t dim = kvDaysInMonth(year, mon);
    if (day > dim) day = dim;
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawTitleBar("Date");

      // Tiny D / M / YY column labels just under the divider.
      gfx->setTextSize(1);
      gfx->setTextColor(DARKGREY, BLACK);
      const char *labels[] = { "DAY", "MONTH", "YEAR" };
      for (int c = 0; c < 3; c++) {
        int16_t x = COL_X(c) + (COL_W - (int16_t)strlen(labels[c]) * 6) / 2;
        gfx->setCursor(x, 52);
        gfx->print(labels[c]);
      }

      for (int c = 0; c < 3; c++) {
        int16_t x = COL_X(c);
        gfx->drawRoundRect(x, BTN_PLUS_Y,  COL_W, BTN_H, 6, DARKGREY);
        gfx->drawRoundRect(x, BTN_MINUS_Y, COL_W, BTN_H, 6, DARKGREY);
        gfx->setTextSize(3);
        { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
        gfx->setCursor(x + COL_W / 2 - 9, BTN_PLUS_Y + 12);
        gfx->print('+');
        gfx->setCursor(x + COL_W / 2 - 9, BTN_MINUS_Y + 12);
        gfx->print('-');
      }
      { ThemeColors _t = theme(); gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, _t.accent); }
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(contrastFor(_t.accent), _t.accent); }
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & DIGITS) { drawDigits(); dirty &= ~DIGITS; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::Settings); return;
    }
    if (e.type == EventType::TouchUp) { stopHold(); return; }
    if (e.type != EventType::Touch && e.type != EventType::TouchHold) return;

    int col = -1; int8_t sign = 0;
    for (int c = 0; c < 3; c++) {
      int16_t x = COL_X(c);
      if (uiInRect(e.x, e.y, x, BTN_PLUS_Y,  COL_W, BTN_H)) { col = c; sign = +1; break; }
      if (uiInRect(e.x, e.y, x, BTN_MINUS_Y, COL_W, BTN_H)) { col = c; sign = -1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bump(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (uiInRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        // Preserve current time, write new date + recomputed weekday.
        uint8_t hh, mm, ss;
        { ModelLock lk; hh = model.hour; mm = model.minute; ss = model.second; }
        uint8_t wd = kvSakamoto(year, mon, day);
        requestSetRTC(hh, mm, ss, wd, day, mon, year);
        hapticBuzz(120, 70);
        switchTo(Screen::Settings);
      }
      return;
    }
    if (e.type == EventType::TouchHold && tickHold(col, sign)) {
      bump(col, sign);
    }
  }
private:
  static const int16_t BTN_PLUS_Y  = 66;
  static const int16_t BTN_H       = 48;
  static const int16_t DIGITS_Y    = 120;
  static const int16_t BTN_MINUS_Y = 168;
  static const int16_t ACT_Y       = 232;
  static const int16_t ACT_H       = 36;
  static const int16_t ACT_W       = 200;
  static const int16_t SAVE_X      = 20;
  static const int16_t COL_W       = 64;
  static int16_t COL_X(int c) { return 16 + c * (COL_W + 12); }

  enum DirtyFlags { TITLE = 1, DIGITS = 2, ALL = 3 };

  uint8_t  day = 1, mon = 1;
  uint16_t year = 2026;
  uint8_t  dirty = ALL;

  void bump(int col, int8_t d) {
    if (col == 0) {
      uint8_t dim = kvDaysInMonth(year, mon);
      day = ((day - 1 + dim + d) % dim) + 1;
    }
    if (col == 1) {
      mon = ((mon - 1 + 12 + d) % 12) + 1;
      uint8_t dim = kvDaysInMonth(year, mon);
      if (day > dim) day = dim;
    }
    if (col == 2) {
      int y = (int)year + d;
      if (y < 2000) y = 2099;
      if (y > 2099) y = 2000;
      year = (uint16_t)y;
      uint8_t dim = kvDaysInMonth(year, mon);
      if (day > dim) day = dim;
    }
    dirty |= DIGITS;
  }
  void drawDigits() {
    gfx->fillRect(0, DIGITS_Y, W, 44, BLACK);
    gfx->setTextSize(5);
    gfx->setTextColor(YELLOW, BLACK);
    char buf[4];
    for (int c = 0; c < 3; c++) {
      uint8_t v;
      if      (c == 0) v = day;
      else if (c == 1) v = mon;
      else             v = (uint8_t)(year % 100);
      snprintf(buf, sizeof(buf), "%02u", v);
      int16_t x = COL_X(c) + (COL_W - 6 * 5 * 2) / 2;
      gfx->setCursor(x, DIGITS_Y);
      gfx->print(buf);
    }
  }
};

static SettingsTimeView sTime;
static SettingsDateView sDate;
View *settingsTimeViewPtr() { return &sTime; }
View *settingsDateViewPtr() { return &sDate; }
