// BasicDigital UI:
//   * WatchFaceView   — big HH:MM, :SS, and a date line. Swipe → Settings;
//     short SW2 press → sleep.
//   * SettingsView    — menu: "Set Time" / "Set Date" / "Sleep" / "Power Off".
//   * SettingsTimeView / SettingsDateView — the FirstOS +/- hold-to-ramp
//     editors, carried over so the on-watch clock-set UX is identical.
//   * SleepSettingsView — auto-sleep + auto-power-off timeout steppers.
//
// Everything is drawn with the built-in 5x7 bitmap font (scaled) — no FreeFont
// assets — to keep the build tiny. The dirty-region caching on the watch face
// and the dirty flags on the settings pages mirror FirstOS so the 1 Hz render
// heartbeat never causes flicker.
#include <Arduino_GFX_Library.h>
#include "pins.h"
#include "power.h"
#include "display.h"
#include "haptic.h"
#include "view.h"
#include "event.h"
#include "controller.h"
#include "storage.h"

// ---------- shared draw helpers ----------
static const int16_t W = 240;
static const int16_t H = 280;

// Settings-UI palette: flat grey with thin outlines, replacing the old navy
// (accent) fills. UI_LINE is the box outline; UI_FILL is the single filled
// "primary" control (the SAVE bar). Greys read cleaner than the saturated
// blue against the black background, and outlines look crisper than fills.
static const uint16_t UI_LINE = 0x7BEF;   // mid grey  — box outlines
static const uint16_t UI_FILL = 0x4208;   // dark grey — SAVE fill

// Every screen reads its colours from the model so the palette stays in one
// place. BasicDigital has no Display settings page; these are the defaults.
ThemeColors theme() {
  ThemeColors t;
  ModelLock lk;
  t.bg     = model.bgColor;
  t.fg     = model.fgColor;
  t.accent = model.accentColor;
  t.line   = model.lineColor;
  return t;
}
// Picks WHITE or BLACK depending on which is more readable against `bg` —
// used so button labels stay legible against the accent fill.
uint16_t contrastFor(uint16_t bg) {
  uint8_t r = (bg >> 11) & 0x1F;
  uint8_t g = (bg >>  5) & 0x3F;
  uint8_t b = (bg      ) & 0x1F;
  uint16_t lum = (uint16_t)((r << 3) * 3 + (g << 2) * 6 + (b << 3) * 1) / 10;
  return (lum < 128) ? WHITE : BLACK;
}

static void clearAll() {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
}

static int16_t centerX(const char *s, uint8_t size) {
  // GFX glyph box = 6 px wide * size. Approximate; good enough for centring.
  int16_t w = (int16_t)strlen(s) * 6 * size;
  return (W - w) / 2;
}

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}

// Top-left back chevron. Wider/taller for easier tapping. A thin grey outline
// (matching the menu boxes) instead of a filled navy block.
static const int16_t BACK_W = 60, BACK_H = 42;
void drawBackButton() {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->drawRoundRect(2, 2, BACK_W, BACK_H, 10, UI_LINE);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}
// Generous hit zone so a thumb tap registers reliably.
bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}

// title: text centred below the back button. titleY / lineY default to the
// standard settings layout. Bitmap font only (no FreeFont in BasicDigital).
void drawTitleBar(const char *title,
                  int16_t titleY, int16_t lineY,
                  int16_t lineX, int16_t lineW) {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawBackButton();
  gfx->setTextColor(t.fg, t.bg);
  gfx->setTextSize(2);
  int16_t tw = (int16_t)strlen(title) * 12;
  gfx->setCursor((W - tw) / 2, titleY);
  gfx->print(title);
  gfx->drawFastHLine(lineX, lineY, lineW, t.line);
}

// Mixin-ish helper bag for views that have +/- columns with hold-to-ramp.
// Carried over verbatim from FirstOS; subclassed by both settings editors.
struct RampPlusMinus {
  int      holdCol     = -1;
  int8_t   holdSign    = 0;
  uint32_t holdStartMs = 0;
  uint32_t nextFireMs  = 0;

  void startHold(int col, int8_t sign) {
    holdCol = col; holdSign = sign;
    holdStartMs = millis();
    nextFireMs = holdStartMs + 400;
  }
  void stopHold() { holdCol = -1; holdSign = 0; }

  // Returns whether a ramp step should fire now; caller must call again next
  // event to keep the ramp going. Updates nextFireMs internally.
  bool tickHold(int col, int8_t sign) {
    if (holdCol < 0) return false;
    if (col != holdCol || sign != holdSign) { stopHold(); return false; }
    uint32_t now = millis();
    if (now < nextFireMs) return false;
    uint32_t held = now - holdStartMs;
    uint32_t period = held < 800 ? 400 :
                      held < 1800 ? 250 :
                      held < 3000 ? 150 :
                      held < 4500 ?  80 : 50;
    nextFireMs = now + period;
    if (period >= 150) hapticBuzz(30, 25);
    return true;
  }
};

// ---------- date math (leap years, days-in-month, Sakamoto weekday) ----------
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

static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *kMon[]  = { "???","Jan","Feb","Mar","Apr","May","Jun",
                               "Jul","Aug","Sep","Oct","Nov","Dec" };

// =====================================================================
// Watch face — big HH:MM, a :SS row, and a date line. A swipe opens Settings;
// a short SW2 press sleeps the watch. Each row is cached and only repainted
// when its value (or the theme) changes so the 1 Hz render heartbeat doesn't
// flicker the whole screen.
// =====================================================================
class WatchFaceView : public View {
public:
  void onEnter() override {
    { ModelLock lk; cachedBg = model.bgColor; cachedFg = model.fgColor; }
    if (gfx) gfx->fillScreen(cachedBg);
    resetCache();
  }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    // Theme changed (shouldn't, BasicDigital has no Display page) → full repaint.
    if (snap.bgColor != cachedBg || snap.fgColor != cachedFg) {
      cachedBg = snap.bgColor; cachedFg = snap.fgColor;
      gfx->fillScreen(cachedBg);
      resetCache();
    }

    // Big time HH:MM.
    if (snap.hour != cached.h || snap.minute != cached.m ||
        !snap.rtcOk != !cached.rtcOk) {
      gfx->fillRect(0, TIME_Y - 4, W, 6 * 8 + 8, cachedBg);
      char buf[8];
      if (snap.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", snap.hour, snap.minute);
      else            strcpy(buf, "--:--");
      gfx->setTextSize(6);
      gfx->setTextColor(snap.rtcOk ? cachedFg : RED, cachedBg);
      gfx->setCursor(centerX(buf, 6), TIME_Y);
      gfx->print(buf);
      cached.h = snap.hour; cached.m = snap.minute; cached.rtcOk = snap.rtcOk;
    }

    // Seconds.
    if (snap.second != cached.s) {
      gfx->fillRect(0, SEC_Y - 2, W, 3 * 8 + 4, cachedBg);
      char buf[8];
      snprintf(buf, sizeof(buf), ":%02u", snap.second);
      gfx->setTextSize(3);
      gfx->setTextColor(cachedFg, cachedBg);
      gfx->setCursor(centerX(buf, 3), SEC_Y);
      gfx->print(buf);
      cached.s = snap.second;
    }

    // Date.
    if (snap.day != cached.day || snap.month != cached.month ||
        snap.year != cached.year || snap.weekday != cached.weekday) {
      gfx->fillRect(0, DATE_Y - 2, W, 2 * 8 + 4, cachedBg);
      if (snap.rtcOk) {
        const char *wd = (snap.weekday < 7) ? kWday[snap.weekday] : "---";
        const char *mn = (snap.month >= 1 && snap.month <= 12) ? kMon[snap.month] : "???";
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %u %s %u", wd, snap.day, mn, snap.year);
        gfx->setTextSize(2);
        gfx->setTextColor(cachedFg, cachedBg);
        gfx->setCursor(centerX(buf, 2), DATE_Y);
        gfx->print(buf);
      }
      cached.day = snap.day; cached.month = snap.month;
      cached.year = snap.year; cached.weekday = snap.weekday;
    }
  }
  void onEvent(const Event &e) override {
    // A short SW2 press sleeps the watch. (Swipe-right reaches us as the
    // controller's global "back" = ButtonShort; on the face that also just
    // sleeps, which reads naturally as "dismiss".)
    if (e.type == EventType::ButtonShort) {
      hapticBuzz(40, 50);
      enterDeepSleep();             // does not return
      return;
    }
    // Any swipe (other than the right-swipe back, handled above) opens
    // Settings. Accepting up / down / left keeps it working regardless of how
    // the panel's swipe axes map to the physical orientation.
    if (e.type == EventType::Gesture &&
        (e.gesture == Gesture::SwipeUp   ||
         e.gesture == Gesture::SwipeDown ||
         e.gesture == Gesture::SwipeLeft)) {
      hapticBuzz(60, 60);
      switchTo(Screen::Settings);
      return;
    }
  }
private:
  static const int16_t TIME_Y = 96;
  static const int16_t SEC_Y  = 160;
  static const int16_t DATE_Y = 204;

  uint16_t cachedBg = BLACK, cachedFg = WHITE;
  struct {
    uint8_t  h, m, s;
    bool     rtcOk;
    uint8_t  day, month, weekday;
    uint16_t year;
  } cached;

  void resetCache() {
    cached.h = 99; cached.m = 99; cached.s = 99; cached.rtcOk = false;
    cached.day = 0; cached.month = 0; cached.year = 0; cached.weekday = 9;
  }
};

// =====================================================================
// Settings — a four-item menu: Set Time / Set Date / Sleep / Power Off.
// Painted once on entry (dirty flag) so the render heartbeat doesn't repaint it.
// =====================================================================
class SettingsView : public View {
public:
  void onEnter() override { dirty = true; }
  void render() override {
    if (!gfx || !dirty) return;
    ThemeColors t = theme();
    drawTitleBar("Settings");
    drawItem(0, "Set Time",  false);
    drawItem(1, "Set Date",  false);
    drawItem(2, "Sleep",     false);
    drawItem(3, "Power Off", true);    // destructive — outlined in red
    dirty = false;
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Watch); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::Watch); return; }
    if (inRect(e.x, e.y, ITEM_X, itemY(0), ITEM_W, ITEM_H)) {
      hapticBuzz(60, 60); switchTo(Screen::SettingsTime); return;
    }
    if (inRect(e.x, e.y, ITEM_X, itemY(1), ITEM_W, ITEM_H)) {
      hapticBuzz(60, 60); switchTo(Screen::SettingsDate); return;
    }
    if (inRect(e.x, e.y, ITEM_X, itemY(2), ITEM_W, ITEM_H)) {
      hapticBuzz(60, 60); switchTo(Screen::SettingsSleep); return;
    }
    if (inRect(e.x, e.y, ITEM_X, itemY(3), ITEM_W, ITEM_H)) {
      hapticBuzz(120, 70);
      powerOffNow();                   // drops the LDO latch — does not return
      return;
    }
  }
private:
  // Four items now, so the rows are shorter/tighter than the old three-item
  // menu to keep the last item clear of the 280 px-tall panel.
  static const int16_t ITEM_X = 20, ITEM_W = 200, ITEM_H = 46;
  static int16_t itemY(int i) { return 62 + i * (ITEM_H + 10); }
  bool dirty = true;

  void drawItem(int i, const char *label, bool destructive) {
    int16_t y = itemY(i);
    ThemeColors t = theme();
    uint16_t line = destructive ? RED : UI_LINE;
    uint16_t txt  = destructive ? RED : t.fg;
    gfx->drawRoundRect(ITEM_X, y, ITEM_W, ITEM_H, 12, line);
    gfx->setTextSize(3);
    gfx->setTextColor(txt, t.bg);
    int16_t tw = (int16_t)strlen(label) * 6 * 3;
    gfx->setCursor(ITEM_X + (ITEM_W - tw) / 2, y + (ITEM_H - 8 * 3) / 2);
    gfx->print(label);
  }
};

// =====================================================================
// SettingsTime — set HH:MM:SS on the RV-3028. Three +/- columns with the
// hold-to-ramp UX. Date fields are preserved by reading them from the model
// and writing them back unchanged. (Verbatim from FirstOS, back → Settings.)
// =====================================================================
class SettingsTimeView : public View, RampPlusMinus {
public:
  void onEnter() override {
    clearAll();
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
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 8, UI_FILL);
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, UI_FILL); }
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
      if (inRect(e.x, e.y, x, BTN_PLUS_Y,  COL_W, BTN_H)) { col = c; sign = +1; break; }
      if (inRect(e.x, e.y, x, BTN_MINUS_Y, COL_W, BTN_H)) { col = c; sign = -1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bump(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (inRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
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
// SettingsDate — set day / month / year on the RV-3028. Same chrome as the
// time page but the third column shows the last two digits of the year
// (stored full-width). Day clamps when month/year changes (28/29/30/31
// boundaries) and the weekday register is recomputed on save.
// =====================================================================
class SettingsDateView : public View, RampPlusMinus {
public:
  void onEnter() override {
    clearAll();
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

      // Tiny DAY / MONTH / YEAR column labels just under the divider.
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
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 8, UI_FILL);
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, UI_FILL); }
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
      if (inRect(e.x, e.y, x, BTN_PLUS_Y,  COL_W, BTN_H)) { col = c; sign = +1; break; }
      if (inRect(e.x, e.y, x, BTN_MINUS_Y, COL_W, BTN_H)) { col = c; sign = -1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bump(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (inRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
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

// =====================================================================
// SettingsSleep — controls the two power/sleep timeouts the controller already
// honours: how long until the watch auto-sleeps, and how long it then waits in
// deep sleep before fully powering off. Each is a tap-to-step preset ladder.
// SAVE commits to the model and persists to NVS (Storage::save), so the choice
// survives a reboot. Same chrome (title bar, accent SAVE bar) as the other
// settings pages.
// =====================================================================

// Preset ladders, in seconds; index 0 == "Never". The controller reads
// model.sleepTimeoutSec (auto-sleep) and model.sleepToOffSec (auto-power-off
// while asleep) directly, so picking a value here changes behaviour as soon as
// SAVE writes it back to the model.
static const uint16_t kSleepPresets[] = { 0, 10, 15, 30, 60, 120, 300 };
static const uint16_t kOffPresets[]   = { 0, 60, 300, 1800, 3600 };
static const uint8_t  kSleepN = sizeof(kSleepPresets) / sizeof(kSleepPresets[0]);
static const uint8_t  kOffN   = sizeof(kOffPresets)   / sizeof(kOffPresets[0]);

// Map a persisted value (which may not sit exactly on a ladder rung, e.g. a
// legacy 45 s) to the nearest preset so the page opens on a sensible row.
static uint8_t nearestPresetIdx(const uint16_t *arr, uint8_t n, uint16_t v) {
  uint8_t best = 0; uint32_t bestd = 0xFFFFFFFFUL;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t d = (arr[i] > v) ? (uint32_t)(arr[i] - v) : (uint32_t)(v - arr[i]);
    if (d < bestd) { bestd = d; best = i; }
  }
  return best;
}

// "Never" / "10s" / "2m" / "1h" for a duration in seconds.
static void fmtDuration(uint16_t sec, char *out, size_t n) {
  if      (sec == 0)         snprintf(out, n, "Never");
  else if (sec < 60)         snprintf(out, n, "%us", (unsigned)sec);
  else if (sec % 3600 == 0)  snprintf(out, n, "%uh", (unsigned)(sec / 3600));
  else if (sec % 60 == 0)    snprintf(out, n, "%um", (unsigned)(sec / 60));
  else                       snprintf(out, n, "%us", (unsigned)sec);
}

class SleepSettingsView : public View {
public:
  void onEnter() override {
    clearAll();
    uint16_t sleepSec, offSec;
    { ModelLock lk; sleepSec = model.sleepTimeoutSec; offSec = model.sleepToOffSec; }
    sleepIdx = nearestPresetIdx(kSleepPresets, kSleepN, sleepSec);
    offIdx   = nearestPresetIdx(kOffPresets,   kOffN,   offSec);
    dirty = ALL;
  }
  void render() override {
    if (!gfx) return;
    if (dirty & CHROME) {
      ThemeColors t = theme();
      drawTitleBar("Sleep");
      gfx->setTextSize(2);
      gfx->setTextColor(t.fg, t.bg);
      gfx->setCursor(ROW_X, ROW1_LABEL_Y); gfx->print("Sleep after");
      gfx->setCursor(ROW_X, ROW2_LABEL_Y); gfx->print("Power off");
      drawStepperChrome(ROW1_CTRL_Y);
      drawStepperChrome(ROW2_CTRL_Y);
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 8, UI_FILL);
      gfx->setTextSize(2);
      gfx->setTextColor(t.fg, UI_FILL);
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~CHROME;
    }
    if (dirty & VALUES) {
      drawValue(ROW1_CTRL_Y, kSleepPresets[sleepIdx]);
      drawValue(ROW2_CTRL_Y, kOffPresets[offIdx]);
      dirty &= ~VALUES;
    }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::Settings); return; }

    if (inRect(e.x, e.y, MINUS_X, ROW1_CTRL_Y, STEP_W, STEP_H)) { step(sleepIdx, kSleepN, -1); return; }
    if (inRect(e.x, e.y, PLUS_X,  ROW1_CTRL_Y, STEP_W, STEP_H)) { step(sleepIdx, kSleepN, +1); return; }
    if (inRect(e.x, e.y, MINUS_X, ROW2_CTRL_Y, STEP_W, STEP_H)) { step(offIdx,   kOffN,   -1); return; }
    if (inRect(e.x, e.y, PLUS_X,  ROW2_CTRL_Y, STEP_W, STEP_H)) { step(offIdx,   kOffN,   +1); return; }

    if (inRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
      { ModelLock lk;
        model.sleepTimeoutSec = kSleepPresets[sleepIdx];
        model.sleepToOffSec   = kOffPresets[offIdx]; }
      Storage::save();                 // persist; takes ModelLock itself, so not nested
      hapticBuzz(120, 70);
      switchTo(Screen::Settings);
    }
  }
private:
  static const int16_t ROW_X        = 20;
  static const int16_t ROW1_LABEL_Y = 58;
  static const int16_t ROW1_CTRL_Y  = 80;
  static const int16_t ROW2_LABEL_Y = 148;
  static const int16_t ROW2_CTRL_Y  = 170;
  static const int16_t STEP_W = 52, STEP_H = 44;
  static const int16_t MINUS_X = 16;
  static const int16_t PLUS_X  = 172;          // 240 - 16 - 52
  static const int16_t ACT_Y = 232, ACT_H = 36, ACT_W = 200, SAVE_X = 20;

  enum DirtyFlags { CHROME = 1, VALUES = 2, ALL = 3 };

  uint8_t sleepIdx = 0, offIdx = 0;
  uint8_t dirty = ALL;

  void step(uint8_t &idx, uint8_t n, int8_t d) {
    int v = (int)idx + d;
    if (v < 0) v = 0;
    if (v >= n) v = n - 1;
    if ((uint8_t)v != idx) { idx = (uint8_t)v; dirty |= VALUES; hapticBuzz(40, 50); }
  }
  void drawStepperChrome(int16_t y) {
    ThemeColors t = theme();
    gfx->drawRoundRect(MINUS_X, y, STEP_W, STEP_H, 6, DARKGREY);
    gfx->drawRoundRect(PLUS_X,  y, STEP_W, STEP_H, 6, DARKGREY);
    gfx->setTextSize(3);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(MINUS_X + STEP_W / 2 - 9, y + (STEP_H - 24) / 2);
    gfx->print('-');
    gfx->setCursor(PLUS_X + STEP_W / 2 - 9, y + (STEP_H - 24) / 2);
    gfx->print('+');
  }
  void drawValue(int16_t y, uint16_t sec) {
    ThemeColors t = theme();
    char buf[8];
    fmtDuration(sec, buf, sizeof(buf));
    int16_t gapX = MINUS_X + STEP_W + 2;
    int16_t gapW = PLUS_X - gapX - 2;
    gfx->fillRect(gapX, y, gapW, STEP_H, t.bg);     // clear stale value
    gfx->setTextSize(3);
    gfx->setTextColor(YELLOW, t.bg);
    int16_t tw = (int16_t)strlen(buf) * 6 * 3;
    gfx->setCursor(gapX + (gapW - tw) / 2, y + (STEP_H - 24) / 2);
    gfx->print(buf);
  }
};

// =====================================================================
// View registry + switching.
// =====================================================================
static WatchFaceView     vWatch;
static SettingsView      vSettings;
static SettingsTimeView  vSettingsTime;
static SettingsDateView  vSettingsDate;
static SleepSettingsView vSettingsSleep;

View *currentView = nullptr;

View *viewFor(Screen s) {
  switch (s) {
    case Screen::Watch:         return &vWatch;
    case Screen::Settings:      return &vSettings;
    case Screen::SettingsTime:  return &vSettingsTime;
    case Screen::SettingsDate:  return &vSettingsDate;
    case Screen::SettingsSleep: return &vSettingsSleep;
  }
  return &vWatch;
}

void switchTo(Screen s) {
  View *next = viewFor(s);
  if (currentView == next) return;
  if (currentView) currentView->onExit();
  { ModelLock lk; model.screen = s; model.revision++; }
  currentView = next;
  currentView->onEnter();
}

void viewsInit() {
  currentView = &vWatch;
  if (gfx) clearAll();
  currentView->onEnter();
}
