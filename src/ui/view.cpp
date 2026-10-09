// BasicAnalog UI:
//   * WatchFaceView   — a clean analog face: hour / minute / second hands over
//     twelve low-key grey hour ticks, no numerals or chrome. Swipe → Settings;
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
#include <math.h>
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

// =====================================================================
// Watch face — a clean analog dial. Twelve low-key grey hour ticks plus an
// hour, minute and second hand; no numerals, ring or other chrome.
//
// The whole face is drawn into an off-screen framebuffer (Arduino_Canvas, one
// 240x280x16bpp allocation kept for the app's lifetime) and blitted as a
// single complete frame. That matters here: an analog face has to clear and
// redraw the moving hands every second, and clearing the panel directly would
// flash black once a second. Compositing in RAM and pushing a finished frame
// keeps the sweep flicker-free. If the framebuffer can't be allocated we fall
// back to drawing straight to the panel (functional, with a faint per-second
// flicker). A swipe opens Settings; a short SW2 press sleeps the watch.
// =====================================================================

// Dial geometry. Centre sits in the middle of the 240x280 panel; the radius
// leaves a hair of margin on the narrow (width) axis.
static const int16_t FACE_CX = 120;
static const int16_t FACE_CY = 140;

// Hour ticks: short radial bars between these two radii, kept thin and dim so
// they read as quiet reference marks rather than graphics.
static const float TICK_R_OUT = 116.0f;
static const float TICK_R_IN  = 103.0f;
static const float TICK_HW    = 1.4f;     // half-width (≈3 px)
static const uint16_t TICK_COLOR = 0x39E7; // dim grey — low "opacity" on black

// Hands. Lengths are tip distance from centre; tails are the short counterweight
// behind the pivot. Each hand is a uniform-width rectangle (2*HW wide) whose
// corners are rounded at radius CR. CR is kept well under HW so the rounding
// only softens the corners — nothing bulges past the bar's footprint. Hour and
// minute take the foreground colour; the second hand gets a single restrained
// accent (one-line change below to go monochrome).
static const float HOUR_LEN = 58.0f, HOUR_TAIL = 16.0f, HOUR_HW = 4.0f, HOUR_CR = .5f;
static const float MIN_LEN  = 88.0f, MIN_TAIL  = 18.0f, MIN_HW  = 3.0f, MIN_CR  = .5f;
static const float SEC_LEN  = 98.0f, SEC_TAIL  = 24.0f, SEC_HW  = 1.0f, SEC_CR  = .5f;
static const uint16_t SECOND_COLOR = 0xE226;  // muted red accent
static const int16_t HUB_R  = 5;   // pivot cap (foreground)
static const int16_t HUB_R2 = 2;   // inner cap (accent)

// Fill a uniform-width rectangle from (x0,y0) to (x1,y1) of half-width hw, as
// two triangles. Square ends — used for the hour ticks.
static void fillQuad(Arduino_GFX *g, float x0, float y0, float x1, float y1,
                     float hw, uint16_t color) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  float px = -dy / len, py = dx / len;       // unit perpendicular
  int16_t ax = (int16_t)lroundf(x0 + px * hw), ay = (int16_t)lroundf(y0 + py * hw);
  int16_t bx = (int16_t)lroundf(x0 - px * hw), by = (int16_t)lroundf(y0 - py * hw);
  int16_t cx = (int16_t)lroundf(x1 + px * hw), cy = (int16_t)lroundf(y1 + py * hw);
  int16_t dx2 = (int16_t)lroundf(x1 - px * hw), dy2 = (int16_t)lroundf(y1 - py * hw);
  g->fillTriangle(ax, ay, cx, cy, dx2, dy2, color);
  g->fillTriangle(ax, ay, dx2, dy2, bx, by, color);
}

// Fill a uniform-width rounded rectangle from (x0,y0) to (x1,y1): half-width hw,
// corner radius cr (clamped to hw). Built as the standard rounded-rect union —
// a full-width body inset by cr at each end, a full-length core narrowed by cr
// on each side, and four corner circles of radius cr at the inset corners. The
// rounding stays inside the rectangle's outline, so no cap protrudes past the
// ends or sides; cr == hw degenerates to a stadium.
static void fillRoundedBar(Arduino_GFX *g, float x0, float y0, float x1, float y1,
                           float hw, float cr, uint16_t color) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  if (cr > hw) cr = hw;
  float ux = dx / len, uy = dy / len;        // unit along the axis
  float px = -uy, py = ux;                    // unit perpendicular
  // Ends pulled in by cr so the body never reaches past the rounded corners.
  float ex0 = x0 + ux * cr, ey0 = y0 + uy * cr;
  float ex1 = x1 - ux * cr, ey1 = y1 - uy * cr;
  fillQuad(g, ex0, ey0, ex1, ey1, hw, color);        // full-width, inset length
  fillQuad(g, x0, y0, x1, y1, hw - cr, color);        // full-length, narrow core
  int16_t r = (int16_t)lroundf(cr);
  if (r > 0) {
    float off = hw - cr;                              // corner centres, perp offset
    g->fillCircle((int16_t)lroundf(ex0 + px * off), (int16_t)lroundf(ey0 + py * off), r, color);
    g->fillCircle((int16_t)lroundf(ex0 - px * off), (int16_t)lroundf(ey0 - py * off), r, color);
    g->fillCircle((int16_t)lroundf(ex1 + px * off), (int16_t)lroundf(ey1 + py * off), r, color);
    g->fillCircle((int16_t)lroundf(ex1 - px * off), (int16_t)lroundf(ey1 - py * off), r, color);
  }
}

class WatchFaceView : public View {
public:
  void onEnter() override {
    { ModelLock lk; cachedBg = model.bgColor; cachedFg = model.fgColor; }
    ensureCanvas();
    resetCache();
    // Paint the current state immediately so the panel shows a finished dial
    // the moment we enter (boot, or returning from Settings).
    Model snap; { ModelLock lk; snap = model; }
    drawAndFlush(snap);
    rememberDrawn(snap);
  }
  void render() override {
    if (!gfx) return;
    Model snap; { ModelLock lk; snap = model; }

    // Theme change (BasicAnalog has no Display page, so this is belt-and-braces)
    // invalidates the cache and forces a repaint.
    if (snap.bgColor != cachedBg || snap.fgColor != cachedFg) {
      cachedBg = snap.bgColor; cachedFg = snap.fgColor;
      resetCache();
    }

    // Nothing the face shows has moved → skip the redraw (render() runs many
    // times a second; the hands only change when the second does).
    if (snap.second == cached.s && snap.minute == cached.m &&
        snap.hour == cached.h && snap.rtcOk == cached.rtcOk) {
      return;
    }
    drawAndFlush(snap);
    rememberDrawn(snap);
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
  uint16_t cachedBg = BLACK, cachedFg = WHITE;
  struct { uint8_t h, m, s; bool rtcOk; } cached;

  Arduino_Canvas *canvas = nullptr;   // off-screen framebuffer (null until tried)
  bool canvasTried = false;
  bool canvasReady = false;

  void resetCache() { cached.h = 99; cached.m = 99; cached.s = 99; cached.rtcOk = false; }
  void rememberDrawn(const Model &s) {
    cached.h = s.hour; cached.m = s.minute; cached.s = s.second; cached.rtcOk = s.rtcOk;
  }

  // Allocate the full-screen framebuffer once. begin(GFX_SKIP_OUTPUT_BEGIN)
  // reuses the already-initialised panel rather than re-running gfx->begin().
  void ensureCanvas() {
    if (canvasTried || !gfx) return;
    canvasTried = true;
    canvas = new Arduino_Canvas(W, H, gfx);
    if (canvas && canvas->begin(GFX_SKIP_OUTPUT_BEGIN) && canvas->getFramebuffer()) {
      canvasReady = true;
    } else {
      Serial.println("WatchFace: framebuffer alloc failed; direct-draw fallback");
    }
  }

  // Compose the face into the framebuffer (or straight onto the panel in the
  // fallback path) and present it.
  void drawAndFlush(const Model &snap) {
    Arduino_GFX *g = canvasReady ? (Arduino_GFX *)canvas : gfx;
    if (!g) return;
    drawFace(g, snap);
    if (canvasReady) canvas->flush();
  }

  // Place a hand at fraction `frac` of a full turn (0 = 12 o'clock, clockwise).
  // Drawn as a uniform-width rectangle with corners rounded at radius `cr`.
  void drawHand(Arduino_GFX *g, float frac, float len, float tail,
                float hw, float cr, uint16_t color) {
    float th = frac * TWO_PI;
    float s = sinf(th), c = cosf(th);
    float tipX  = FACE_CX + len  * s, tipY  = FACE_CY - len  * c;
    float tailX = FACE_CX - tail * s, tailY = FACE_CY + tail * c;
    fillRoundedBar(g, tailX, tailY, tipX, tipY, hw, cr, color);
  }

  void drawFace(Arduino_GFX *g, const Model &snap) {
    g->fillScreen(cachedBg);

    // Twelve low-key hour ticks.
    for (int i = 0; i < 12; i++) {
      float th = i * (TWO_PI / 12.0f);
      float s = sinf(th), c = cosf(th);
      fillQuad(g,
               FACE_CX + TICK_R_IN  * s, FACE_CY - TICK_R_IN  * c,
               FACE_CX + TICK_R_OUT * s, FACE_CY - TICK_R_OUT * c,
               TICK_HW, TICK_COLOR);
    }

    // Hands — only when the RTC has a valid time; otherwise a bare tick dial.
    if (snap.rtcOk) {
      float hf = ((snap.hour % 12) + snap.minute / 60.0f) / 12.0f;
      float mf = (snap.minute + snap.second / 60.0f) / 60.0f;
      float sf = snap.second / 60.0f;
      drawHand(g, hf, HOUR_LEN, HOUR_TAIL, HOUR_HW, HOUR_CR, cachedFg);
      drawHand(g, mf, MIN_LEN,  MIN_TAIL,  MIN_HW,  MIN_CR,  cachedFg);
      drawHand(g, sf, SEC_LEN,  SEC_TAIL,  SEC_HW,  SEC_CR,  SECOND_COLOR);
    }

    // Pivot cap, drawn last so the hands converge cleanly under it.
    g->fillCircle(FACE_CX, FACE_CY, HUB_R,  cachedFg);
    g->fillCircle(FACE_CX, FACE_CY, HUB_R2, SECOND_COLOR);
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
