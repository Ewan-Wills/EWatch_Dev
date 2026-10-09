// Watch face — default view. Centered HH:MM, secondary :SS row, date row,
// corner power/WiFi icons, and stopwatch/timer status lines. Swipe left for
// the app list. Also owns the watch-face style table (fonts, 7-seg digital,
// bitmap effects) that Settings → Font and the web form pick from.
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "FreeSans24pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "FreeSerifBold24pt7b.h"
#include "FreeMono24pt7b.h"
#include "FreeSans12pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSerifBold12pt7b.h"
#include "FreeMono12pt7b.h"
#include "pins.h"
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "controller.h"
#include "storage.h"
#include "apptimer.h"
#include "system_views.h"
#include "smicon_bg.h"     // SMICON_BG  240x280 logo watermark (SM Logo face)

static const int16_t W = 240;

// =====================================================================
// Watch face style profiles. Each profile drives the size + Y position of
// the time/seconds/date rows on the watch face, plus an optional effect
// pass. The user picks an index via the web page or Display settings;
// persisted as model.watchFaceStyle.
// =====================================================================
enum class WatchFaceEffect : uint8_t {
  Plain   = 0,
  Bold    = 1,    // overstrike 1 px right
  Outline = 2,    // 8-direction outline in bg around fg-coloured glyph
  Shadow  = 3,    // glyph in bg color shifted +2,+2 then fg on top
};

struct WatchFaceStyle {
  const char *name;
  // For bitmap-font styles `timeFont == nullptr`, `timeSize` controls scaling
  // and `timeY` is the top-left Y of the row. For Adafruit FreeFont styles
  // `timeFont != nullptr` and `timeY` is the BASELINE — that's where Adafruit
  // GFXfonts anchor each glyph.
  const GFXfont *timeFont;
  // `uiFont` is the 12 pt counterpart used for the rest of the UI text —
  // page titles, carousel tile labels, etc. nullptr means "stay on bitmap".
  const GFXfont *uiFont;
  uint8_t  timeSize;
  uint8_t  secSize;      // :SS row text size (always bitmap)
  uint8_t  dateSize;     // date row text size (always bitmap)
  int16_t  timeY;
  int16_t  secY;
  int16_t  dateY;
  WatchFaceEffect effect;  // bitmap-only effect (ignored for GFXfont / digital)
  bool     digital;        // 7-segment HH:MM (overrides everything else for time)
  // 0 = text face, 1 = analog dial (sweeping hands), 2 = orbit rings,
  // 3 = SM logo watermark (static, event-driven). Non-zero styles render
  // full frames into the shared canvas.
  uint8_t  custom;
};

static const WatchFaceStyle kWatchFaceStyles[] = {
  // name     timeFont               uiFont                  tsz ssz dsz   timeY secY dateY  effect                    digital custom
  { "Default",nullptr,               nullptr,                  6,  3,  2,  108,  174,  212,  WatchFaceEffect::Plain,   false,  0 },
  { "Sans",   &FreeSans24pt7b,       &FreeSans12pt7b,          1,  3,  2,  146,  186,  220,  WatchFaceEffect::Plain,   false,  0 },
  { "Bold",   &FreeSansBold24pt7b,   &FreeSansBold12pt7b,      1,  3,  2,  146,  186,  220,  WatchFaceEffect::Plain,   false,  0 },
  { "Serif",  &FreeSerifBold24pt7b,  &FreeSerifBold12pt7b,     1,  3,  2,  146,  186,  220,  WatchFaceEffect::Plain,   false,  0 },
  { "Mono",   &FreeMono24pt7b,       &FreeMono12pt7b,          1,  3,  2,  146,  186,  220,  WatchFaceEffect::Plain,   false,  0 },
  { "Digital",nullptr,               nullptr,                  0,  3,  2,  102,  186,  220,  WatchFaceEffect::Plain,   true,   0 },
  { "Outline",nullptr,               nullptr,                  6,  3,  2,  108,  174,  212,  WatchFaceEffect::Outline, false,  0 },
  { "Shadow", nullptr,               nullptr,                  6,  3,  2,  108,  174,  212,  WatchFaceEffect::Shadow,  false,  0 },
  { "Analog", nullptr,               nullptr,                  0,  0,  2,    0,    0,  250,  WatchFaceEffect::Plain,   false,  1 },
  { "Orbit",  nullptr,               nullptr,                  0,  0,  2,    0,    0,  250,  WatchFaceEffect::Plain,   false,  2 },
  { "SM Logo",nullptr,               nullptr,                  0,  0,  2,    0,    0,  250,  WatchFaceEffect::Plain,   false,  3 },
};

static const int kWatchFaceStyleCount =
    (int)(sizeof(kWatchFaceStyles) / sizeof(kWatchFaceStyles[0]));

// The UI font for the currently-saved style. Returns nullptr to mean "use
// the built-in bitmap font". Cheap enough to call from the title bar each
// render because it just takes a single short ModelLock.
const GFXfont *currentUiFont() {
  uint8_t s; { ModelLock lk; s = model.watchFaceStyle; }
  if (s >= (uint8_t)kWatchFaceStyleCount) return nullptr;
  return kWatchFaceStyles[s].uiFont;
}

static const WatchFaceStyle &watchFaceStyleFor(uint8_t idx) {
  if (idx >= kWatchFaceStyleCount) idx = 0;
  return kWatchFaceStyles[idx];
}

int watchFaceStyleCount() { return kWatchFaceStyleCount; }
const char *watchFaceStyleName(int idx) {
  if (idx < 0 || idx >= kWatchFaceStyleCount) return "";
  return kWatchFaceStyles[idx].name;
}

// Draws bitmap-font text with a per-style effect. Uses transparent text (so
// the multiple overlapping passes for Outline don't erase each other); the
// caller must have already cleared the band with the background colour.
static void drawStyledText(Arduino_GFX *g, int16_t x, int16_t y, uint8_t size,
                           uint16_t fg, uint16_t bg, WatchFaceEffect ef,
                           const char *text) {
  g->setTextSize(size);
  switch (ef) {
    case WatchFaceEffect::Plain:
      g->setTextColor(fg);
      g->setCursor(x, y);
      g->print(text);
      break;
    case WatchFaceEffect::Bold:
      // Two overlapping passes 1 px apart = chunkier strokes.
      g->setTextColor(fg);
      g->setCursor(x, y);     g->print(text);
      g->setCursor(x + 1, y); g->print(text);
      break;
    case WatchFaceEffect::Outline: {
      // Eight offset copies in fg form a 1 px halo; then a single bg-coloured
      // copy in the centre punches the interior out so the glyphs read as
      // hollow stroked letters.
      g->setTextColor(fg);
      for (int8_t dx = -1; dx <= 1; dx++)
        for (int8_t dy = -1; dy <= 1; dy++) {
          if (dx == 0 && dy == 0) continue;
          g->setCursor(x + dx, y + dy);
          g->print(text);
        }
      g->setTextColor(bg);
      g->setCursor(x, y);
      g->print(text);
      break;
    }
    case WatchFaceEffect::Shadow:
      g->setTextColor(DARKGREY);
      g->setCursor(x + 3, y + 3);
      g->print(text);
      g->setTextColor(fg);
      g->setCursor(x, y);
      g->print(text);
      break;
  }
}

// =====================================================================
// 7-segment "Digital" renderer for HH:MM. Drawn from filled trapezoids so the
// segments have angled ends. Each digit is `dw` wide × `dh` tall with a
// segment thickness of `t`. Layout: HH : MM, gap between digits, colon.
//
// Segment indexing (Adafruit-typical):
//   aaa
//  f   b
//  f   b
//   ggg
//  e   c
//  e   c
//   ddd
// abcdefg
// =====================================================================

// One horizontal segment — wider in the middle, tapered to a point at each end
// (an elongated hexagon). Looks like a real 7-seg LCD pip.
static void drawHSeg(Arduino_GFX *g, int16_t x, int16_t y,
                     int16_t w, int16_t t, uint16_t color) {
  g->fillRect(x + t / 2, y, w - t, t, color);
  for (int16_t i = 0; i < t / 2; i++) {
    g->fillRect(x + (t / 2) - i - 1,         y + i, 1, t - 2 * i, color);
    g->fillRect(x + w - (t / 2) + i,          y + i, 1, t - 2 * i, color);
  }
}
// One vertical segment — same idea rotated 90°.
static void drawVSeg(Arduino_GFX *g, int16_t x, int16_t y,
                     int16_t h, int16_t t, uint16_t color) {
  g->fillRect(x, y + t / 2, t, h - t, color);
  for (int16_t i = 0; i < t / 2; i++) {
    g->fillRect(x + i,         y + (t / 2) - i - 1, t - 2 * i, 1, color);
    g->fillRect(x + i,         y + h - (t / 2) + i, t - 2 * i, 1, color);
  }
}

static void draw7SegDigit(Arduino_GFX *g, int16_t x, int16_t y,
                          int16_t dw, int16_t dh, int16_t t,
                          uint8_t segs, uint16_t color) {
  int16_t halfH = dh / 2;
  if (segs & 0x40) drawHSeg(g, x, y,                  dw, t, color);
  if (segs & 0x20) drawVSeg(g, x + dw - t,     y,             halfH + t / 2, t, color);
  if (segs & 0x10) drawVSeg(g, x + dw - t,     y + halfH - t / 2, halfH + t / 2, t, color);
  if (segs & 0x08) drawHSeg(g, x, y + dh - t,         dw, t, color);
  if (segs & 0x04) drawVSeg(g, x,              y + halfH - t / 2, halfH + t / 2, t, color);
  if (segs & 0x02) drawVSeg(g, x,              y,             halfH + t / 2, t, color);
  if (segs & 0x01) drawHSeg(g, x, y + halfH - t / 2,  dw, t, color);
}

// Segment table for digits 0-9. Bit 0x40 = a (top), 0x20 = b (UR), 0x10 = c (LR),
// 0x08 = d (bottom), 0x04 = e (LL), 0x02 = f (UL), 0x01 = g (mid).
static const uint8_t k7SegDigit[10] = {
  /*0*/ 0x7E, /*1*/ 0x30, /*2*/ 0x6D, /*3*/ 0x79, /*4*/ 0x33,
  /*5*/ 0x5B, /*6*/ 0x5F, /*7*/ 0x70, /*8*/ 0x7F, /*9*/ 0x7B,
};

// Draw a 4-digit HH:MM time at the top of the watch face; ghost the 'off'
// segments at low intensity so the digit silhouette is always visible — same
// trick a real LCD watch uses.
static int16_t draw7SegTime(Arduino_GFX *g, int16_t topY,
                            uint8_t h, uint8_t m,
                            uint16_t color, uint16_t bg) {
  const int16_t dw = 40, dh = 70, t = 8;
  const int16_t gap = 8;
  const int16_t colW = 18;
  int16_t totalW = dw * 4 + gap * 3 + colW;
  int16_t x = (240 - totalW) / 2;
  g->fillRect(0, topY - 2, 240, dh + 6, bg);

  uint16_t ghost = DARKGREY;
  uint8_t d0 = h / 10, d1 = h % 10, d2 = m / 10, d3 = m % 10;
  uint8_t segsAll = 0x7F;
  draw7SegDigit(g, x,                       topY, dw, dh, t, segsAll, ghost);
  draw7SegDigit(g, x + dw + gap,            topY, dw, dh, t, segsAll, ghost);
  int16_t colX = x + 2 * (dw + gap);
  draw7SegDigit(g, colX + colW,             topY, dw, dh, t, segsAll, ghost);
  draw7SegDigit(g, colX + colW + dw + gap,  topY, dw, dh, t, segsAll, ghost);

  draw7SegDigit(g, x,                       topY, dw, dh, t, k7SegDigit[d0], color);
  draw7SegDigit(g, x + dw + gap,            topY, dw, dh, t, k7SegDigit[d1], color);
  int16_t dot = t;
  int16_t dotX = colX + (colW - dot) / 2;
  g->fillRect(dotX, topY + dh / 3 - dot / 2,       dot, dot, color);
  g->fillRect(dotX, topY + (2 * dh) / 3 - dot / 2, dot, dot, color);
  draw7SegDigit(g, colX + colW,             topY, dw, dh, t, k7SegDigit[d2], color);
  draw7SegDigit(g, colX + colW + dw + gap,  topY, dw, dh, t, k7SegDigit[d3], color);
  return topY + dh;
}

// =====================================================================
// Corner status icons — shared painters used by BOTH the direct-to-panel
// text faces and the canvas-rendered faces (analog / orbit / SM logo), so
// every face shows the identical icons with the identical state behaviour.
// =====================================================================
static const int16_t kPwrCX = 212, kPwrCY = 24, kPwrR = 13;
static const int16_t kWifiCX = 28, kWifiCY = 42;
static const int16_t kWifiR1 = 5, kWifiR2 = 11, kWifiR3 = 17;

static void drawPowerIconTo(Arduino_GFX *g, uint16_t fg, uint16_t bg) {
  // Two-pixel-thick ring + erased top notch + double-thick stem.
  g->drawCircle(kPwrCX, kPwrCY, kPwrR,     fg);
  g->drawCircle(kPwrCX, kPwrCY, kPwrR - 1, fg);
  g->fillRect(kPwrCX - 2, kPwrCY - kPwrR - 2, 5, 5, bg);
  g->drawFastVLine(kPwrCX,     kPwrCY - kPwrR + 1, kPwrR - 2, fg);
  g->drawFastVLine(kPwrCX + 1, kPwrCY - kPwrR + 1, kPwrR - 2, fg);
}

// WiFi fan icon — three concentric top-half arcs above a center dot. Off
// (grey) when disabled, yellow (full) when hosting an AP, green with bars
// from RSSI when connected as a client, orange (dot only) when scanning.
static void drawWifiFanTo(Arduino_GFX *g, uint16_t bg,
                          bool en, WifiMode mode, bool conn, int8_t rssi) {
#if !defined(EWATCH_ENABLE_WIFI) || !EWATCH_ENABLE_WIFI
  (void)g; (void)bg; (void)en; (void)mode; (void)conn; (void)rssi;
  return;
#else
  int8_t bars = 0;
  uint16_t col = DARKGREY;
  if (en && mode == WifiMode::AP)               { col = YELLOW; bars = 3; }
  else if (en && conn) {
    col = GREEN;
    if      (rssi >= -55) bars = 3;
    else if (rssi >= -65) bars = 2;
    else if (rssi >= -75) bars = 1;
    else                  bars = 0;
  }
  else if (en)                                  { col = ORANGE; bars = 0; }

  // GFX 1.4.7 doesn't expose drawCircleHelper, so we draw full circles and
  // erase the bottom half. The dot is then drawn on top.
  g->fillRect(2, 4, 56, 46, bg);
  uint16_t off = DARKGREY;
  uint16_t cDot = en ? col : off;
  uint16_t cA1  = (en && bars >= 1) ? col : off;
  uint16_t cA2  = (en && bars >= 2) ? col : off;
  uint16_t cA3  = (en && bars >= 3) ? col : off;
  g->drawCircle(kWifiCX, kWifiCY, kWifiR1,     cA1);
  g->drawCircle(kWifiCX, kWifiCY, kWifiR1 + 1, cA1);
  g->drawCircle(kWifiCX, kWifiCY, kWifiR2,     cA2);
  g->drawCircle(kWifiCX, kWifiCY, kWifiR2 + 1, cA2);
  g->drawCircle(kWifiCX, kWifiCY, kWifiR3,     cA3);
  g->drawCircle(kWifiCX, kWifiCY, kWifiR3 + 1, cA3);
  g->fillRect(2, kWifiCY + 1, 56, 22, bg);
  g->fillCircle(kWifiCX, kWifiCY - 2, 2, cDot);
#endif  // EWATCH_ENABLE_WIFI
}

// =====================================================================
// WatchFaceView
// =====================================================================
class WatchFaceView : public View {
public:
  void onEnter() override {
    { ModelLock lk;
      cachedBg    = model.bgColor;
      cachedFg    = model.fgColor;
      cachedStyle = model.watchFaceStyle; }
    if (gfx) {
      gfx->fillScreen(cachedBg);
      drawPowerIcon();
      drawWifiIcon(/*force=*/true);
    }
    resetCaches();
  }

  uint16_t desiredFrameMs() const override {
    // Animated faces (analog sweep / orbit) run a ~15 fps frame loop; the
    // SM-logo face is static so it stays event-driven. The stopwatch / timer
    // status lines display milliseconds — animate at 10 fps while either is
    // active. Otherwise the face only changes on the RTC second tick, so
    // stay event-driven (0).
    uint8_t custom = watchFaceStyleFor(cachedStyle).custom;
    if (custom == 1 || custom == 2) return 66;
    if (stopwatchRunning() || timerArmed()) return 100;
    return 0;
  }

  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    // Repaint the background and force time/seconds/date redraw if the user
    // has changed colors OR the font style since we last entered.
    if (snap.bgColor != cachedBg || snap.fgColor != cachedFg ||
        snap.watchFaceStyle != cachedStyle) {
      gfx->fillScreen(snap.bgColor);
      cachedBg    = snap.bgColor;
      cachedFg    = snap.fgColor;
      cachedStyle = snap.watchFaceStyle;
      drawPowerIcon();
      drawWifiIcon(/*force=*/true);
      resetCaches();
    }

    const WatchFaceStyle &fs = watchFaceStyleFor(cachedStyle);

    // Animated faces repaint the whole frame into the shared canvas and
    // return — none of the dirty-tracked text plumbing below applies.
    if (fs.custom) { renderCustomFace(snap, fs.custom); return; }

    drawWifiIcon(/*force=*/false);
    drawBatteryWarning(snap);

    // Big time HH:MM.
    if (snap.hour != cached.h || snap.minute != cached.m ||
        !snap.rtcOk != !cached.rtcOk) {
      if (fs.digital) {
        // 7-segment style. Renderer erases its own band; on RTC failure fall
        // back to a centred "--:--" bitmap so the user sees something.
        if (snap.rtcOk) {
          draw7SegTime(gfx, fs.timeY, snap.hour, snap.minute, cachedFg, cachedBg);
        } else {
          gfx->fillRect(0, 100, W, 68, cachedBg);
          gfx->setTextSize(6);
          gfx->setTextColor(RED, cachedBg);
          const char *buf = "--:--";
          gfx->setCursor(uiCenterX(buf, 6), 108);
          gfx->print(buf);
        }
      } else {
        // The repaint band has to cover the tallest possible glyph. The
        // FreeFont 24pt fonts reach ~36 px above the baseline; with timeY=146
        // we need to clear y >= 110. Plus a small bleed for any leftover
        // pixels from prior styles.
        gfx->fillRect(0, 100, W, 68, cachedBg);
        char buf[8];
        if (snap.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", snap.hour, snap.minute);
        else            strcpy(buf, "--:--");
        uint16_t fg = snap.rtcOk ? cachedFg : RED;
        if (fs.timeFont) {
          // Adafruit FreeFont path. setFont changes glyph rendering for every
          // subsequent print; we must clear it before drawing sec/date so the
          // bitmap-font helper keeps working.
          gfx->setFont(fs.timeFont);
          gfx->setTextSize(fs.timeSize);
          gfx->setTextColor(fg);              // transparent over cleared band
          int16_t x1, y1; uint16_t tw, th;
          gfx->getTextBounds(buf, 0, fs.timeY, &x1, &y1, &tw, &th);
          int16_t cx = (W - (int16_t)tw) / 2 - (x1 - 0);
          gfx->setCursor(cx, fs.timeY);
          gfx->print(buf);
          gfx->setFont(nullptr);              // back to bitmap for sec/date
        } else {
          int16_t cx = uiCenterX(buf, fs.timeSize);
          drawStyledText(gfx, cx, fs.timeY, fs.timeSize, fg, cachedBg, fs.effect, buf);
        }
      }
      cached.h = snap.hour; cached.m = snap.minute; cached.rtcOk = snap.rtcOk;
    }

    // Seconds.
    if (snap.second != cached.s) {
      // Pad the cleared band on each side to swallow shadow / outline bleed.
      int16_t pad = (fs.effect == WatchFaceEffect::Shadow) ? 4 : 2;
      gfx->fillRect(0, fs.secY - pad, W, fs.secSize * 8 + 2 * pad, cachedBg);
      char buf[8];
      snprintf(buf, sizeof(buf), ":%02u", snap.second);
      int16_t cx = uiCenterX(buf, fs.secSize);
      drawStyledText(gfx, cx, fs.secY, fs.secSize, cachedFg, cachedBg, fs.effect, buf);
      cached.s = snap.second;
    }

    // Date below the seconds row. Redraw only when it changes.
    if (snap.day != cached.day || snap.month != cached.month ||
        snap.year != cached.year || snap.weekday != cached.weekday) {
      int16_t pad = (fs.effect == WatchFaceEffect::Shadow) ? 4 : 2;
      gfx->fillRect(0, fs.dateY - pad, W, fs.dateSize * 8 + 2 * pad, cachedBg);
      if (snap.rtcOk) {
        static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
        static const char *kMon[]  = { "???","Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec" };
        const char *wd = (snap.weekday < 7) ? kWday[snap.weekday] : "---";
        const char *mn = (snap.month >= 1 && snap.month <= 12) ? kMon[snap.month] : "???";
        char buf[20];
        snprintf(buf, sizeof(buf), "%s %u %s %u", wd, snap.day, mn, snap.year);
        int16_t cx = uiCenterX(buf, fs.dateSize);
        drawStyledText(gfx, cx, fs.dateY, fs.dateSize, cachedFg, cachedBg, fs.effect, buf);
      }
      cached.day = snap.day; cached.month = snap.month;
      cached.year = snap.year; cached.weekday = snap.weekday;
    }

    // Stopwatch / timer status lines — shown only while each is active.
    // Both lines display milliseconds. The stopwatch has native ms precision;
    // the timer is anchored at second precision in the RTC, so we interpolate
    // inside the current second using millis().
    if (snap.second != msTrack.lastSec) {
      msTrack.lastSec      = snap.second;
      msTrack.lastSecMs    = millis();
    }
    uint32_t fracMs = millis() - msTrack.lastSecMs;
    if (fracMs > 999) fracMs = 999;

    uint32_t epoch = snap.rtcOk
        ? rtcEpochSec(snap.year, snap.month, snap.day,
                      snap.hour, snap.minute, snap.second)
        : 0;

    bool     swRun = stopwatchRunning();
    uint32_t swMs  = swRun ? stopwatchElapsedMs(epoch, millis()) : 0;
    if (swRun != cached.swRun || swMs != cached.swMs) {
      gfx->fillRect(0, 236, W, 18, cachedBg);
      if (swRun) {
        char buf[24];
        fmtClockMs(buf, sizeof(buf), "SW ", swMs);
        gfx->setTextSize(2);
        gfx->setTextColor(GREEN, cachedBg);
        gfx->setCursor(uiCenterX(buf, 2), 238);
        gfx->print(buf);
      }
      cached.swRun = swRun; cached.swMs = swMs;
    }

    bool     tmrOn  = timerArmed();
    uint32_t tmrSec = tmrOn ? timerRemainingSec(epoch) : 0;
    // Subtract the fractional millis of the current RTC second so the timer
    // ticks down smoothly between RTC updates. Clamp at zero so we never go
    // negative around the deadline (controller will fire the alarm then).
    uint32_t tmrMs = 0;
    if (tmrOn) {
      uint32_t total = tmrSec * 1000u;
      tmrMs = (total > fracMs) ? (total - fracMs) : 0;
    }
    if (tmrOn != cached.tmrOn || tmrMs != cached.tmrMs) {
      gfx->fillRect(0, 258, W, 20, cachedBg);
      if (tmrOn) {
        char buf[24];
        fmtClockMs(buf, sizeof(buf), "T-", tmrMs);
        gfx->setTextSize(2);
        gfx->setTextColor(ORANGE, cachedBg);
        gfx->setCursor(uiCenterX(buf, 2), 260);
        gfx->print(buf);
      }
      cached.tmrOn = tmrOn; cached.tmrMs = tmrMs;
    }
  }

  void onEvent(const Event &e) override {
    // Swipe-left is the only way into the app list — a plain tap stays on
    // the watch face so the user has to commit a gesture intentionally.
    if (e.type == EventType::Gesture && e.gesture == Gesture::SwipeLeft) {
      hapticBuzz(60, 70);
      switchTo(Screen::AppList);
      pressActive = false;
      return;
    }
    // Tap-vs-swipe for the corner buttons: defer the action to TouchUp and
    // bail if the finger moved (i.e. the user was actually starting a swipe).
    if (e.type == EventType::Touch) {
      pressX = e.x; pressY = e.y;
      pressInPower = uiInRect(e.x, e.y, PWR_HIT_X, PWR_HIT_Y, PWR_HIT_W, PWR_HIT_H);
      pressInWifi  = uiInRect(e.x, e.y, WIFI_HIT_X, WIFI_HIT_Y, WIFI_HIT_W, WIFI_HIT_H);
      pressMoved = false;
      pressActive = true;
      pressStartMs = millis();
      return;
    }
    if (e.type == EventType::TouchHold && pressActive) {
      int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
      if (dx * dx + dy * dy > 16 * 16) pressMoved = true;
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
      // Press-and-hold on the WiFi icon opens the WiFi settings page
      // (a quick tap still toggles the radio, handled on TouchUp).
      if (pressInWifi && !pressMoved && millis() - pressStartMs > 600) {
        pressActive = false;
        pressInWifi = false;
        hapticBuzz(80, 70);
        switchTo(Screen::SettingsWifi);
        return;
      }
#endif
      return;
    }
    if (e.type == EventType::TouchUp) {
      if (pressActive && !pressMoved) {
        if      (pressInPower) sleepNow();
        else if (pressInWifi)  toggleWifi();
      }
      pressActive = false;
      pressInPower = false;
      pressInWifi  = false;
      return;
    }
  }

private:
  // Power-icon hit zone (top-right corner) and visual placement. The hit zone
  // is much larger than the visible icon — corner taps from the touch panel
  // can land 10-15 px off, so a tight bbox makes the button feel broken.
  static const int16_t PWR_HIT_X = 130, PWR_HIT_Y = 0;
  static const int16_t PWR_HIT_W = 110, PWR_HIT_H = 92;
  static const int16_t PWR_CX = 212, PWR_CY = 24, PWR_R = 13;

  // WiFi icon — mirror of the power icon in the top-left corner.
  static const int16_t WIFI_HIT_X = 0, WIFI_HIT_Y = 0;
  static const int16_t WIFI_HIT_W = 110, WIFI_HIT_H = 92;
  static const int16_t WIFI_CX = 28, WIFI_CY = 42;
  static const int16_t WIFI_R1 = 5, WIFI_R2 = 11, WIFI_R3 = 17;

  // Tap-vs-swipe disambiguation state for the corner buttons.
  bool     pressActive   = false;
  bool     pressInPower  = false;
  bool     pressInWifi   = false;
  bool     pressMoved    = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressStartMs = 0;     // long-press detection (WiFi icon -> settings)

  // Cached WiFi-icon state so we only redraw on visual change.
  struct {
    bool     en;
    WifiMode mode;
    bool     conn;
    int8_t   bars;     // 0..3
  } wifiCache{false, WifiMode::AP, false, -1};

  uint16_t cachedBg    = BLACK;
  uint16_t cachedFg    = WHITE;
  uint8_t  cachedStyle = 0xFF;       // force first-draw diff vs. model
  struct {
    uint8_t  h, m, s;
    bool     rtcOk;
    uint8_t  day, month, weekday;
    uint16_t year;
    bool     swRun, tmrOn;
    uint32_t swMs, tmrMs;
  } cached{99,99,99,false,0,0,9,0,false,false,0xFFFFFFFF,0xFFFFFFFF};

  // Battery-warning pill state (drawn center-top when charge is low).
  int8_t lastBatWarn = -1;   // -1 unknown, 0 none, 1 low

  // Tracks the millis() value at which we last observed an RTC second change,
  // so we can interpolate sub-second progress for the timer display.
  struct {
    uint8_t  lastSec   = 99;
    uint32_t lastSecMs = 0;
  } msTrack;

  void resetCaches() {
    cached.h = 99; cached.m = 99; cached.s = 99; cached.rtcOk = false;
    cached.day = 0; cached.month = 0; cached.year = 0; cached.weekday = 9;
    cached.swRun = false; cached.swMs = 0xFFFFFFFF;
    cached.tmrOn = false; cached.tmrMs = 0xFFFFFFFF;
    msTrack.lastSec = 99; msTrack.lastSecMs = millis();
    lastBatWarn = -1;
  }

  // "prefix" + H:MM:SS.mmm / MM:SS.mmm depending on magnitude.
  static void fmtClockMs(char *buf, size_t n, const char *prefix, uint32_t ms) {
    uint32_t totalSec = ms / 1000u;
    uint32_t msPart   = ms % 1000u;
    uint32_t hh = totalSec / 3600u;
    uint32_t mm = (totalSec % 3600u) / 60u;
    uint32_t ss = totalSec % 60u;
    if (hh > 0) snprintf(buf, n, "%s%lu:%02lu:%02lu.%03lu", prefix,
                         (unsigned long)hh, (unsigned long)mm,
                         (unsigned long)ss, (unsigned long)msPart);
    else        snprintf(buf, n, "%s%02lu:%02lu.%03lu", prefix,
                         (unsigned long)mm, (unsigned long)ss,
                         (unsigned long)msPart);
  }

  void drawPowerIcon() {
    drawPowerIconTo(gfx, cachedFg, cachedBg);
  }

  // Small centre-top battery pill, shown only when the battery is low so the
  // face stays clean in normal use. Red outline + percentage.
  void drawBatteryWarning(const Model &snap) {
    int8_t warn = (snap.batOk && snap.batLow) ? 1 : 0;
    bool pctChanged = warn && (snap.batPct != lastBatPct);
    if (warn == lastBatWarn && !pctChanged) return;
    lastBatWarn = warn;
    lastBatPct  = snap.batPct;
    gfx->fillRect(90, 2, 60, 26, cachedBg);
    if (!warn) return;
    // Battery glyph: body + nub, red, with pct inside.
    gfx->drawRoundRect(92, 4, 50, 22, 4, RED);
    gfx->fillRect(142, 10, 4, 10, RED);
    char buf[6];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)snap.batPct);
    gfx->setTextSize(1);
    gfx->setTextColor(RED, cachedBg);
    gfx->setCursor(92 + (50 - (int16_t)strlen(buf) * 6) / 2, 4 + 8);
    gfx->print(buf);
  }
  uint8_t lastBatPct = 0xFF;

  void toggleWifi() {
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
    bool now;
    { ModelLock lk;
      model.wifiEnabled = !model.wifiEnabled;
      now = model.wifiEnabled;
      model.revision++; }
    Storage::save();
    hapticBuzz(50, 60);
    Serial.printf("UI: wifi toggled -> %s\n", now ? "ON" : "OFF");
#endif
  }

  // WiFi fan icon on the text faces: cache the visual state so we only
  // repaint on change; the actual pixels come from the shared painter.
  void drawWifiIcon(bool force) {
#if !defined(EWATCH_ENABLE_WIFI) || !EWATCH_ENABLE_WIFI
    (void)force;     // WiFi compiled out — leave the corner blank.
    return;
#else
    bool en, conn; WifiMode mode; int8_t rssi;
    { ModelLock lk;
      en = model.wifiEnabled; mode = model.wifiMode;
      conn = model.wifiConnected; rssi = model.wifiRssi; }
    int8_t bars = 0;
    if (en && mode == WifiMode::AP)  bars = 3;
    else if (en && conn) {
      if      (rssi >= -55) bars = 3;
      else if (rssi >= -65) bars = 2;
      else if (rssi >= -75) bars = 1;
    }
    if (!force &&
        wifiCache.en == en && wifiCache.mode == mode &&
        wifiCache.conn == conn && wifiCache.bars == bars) return;
    wifiCache = { en, mode, conn, bars };
    drawWifiFanTo(gfx, cachedBg, en, mode, conn, rssi);
#endif  // EWATCH_ENABLE_WIFI
  }

  // ================= animated faces (canvas frame loop) =================

  // Sub-second progress through the current RTC second, from the millis()
  // timestamp of the last observed second change.
  float secFraction(const Model &snap) {
    if (snap.second != msTrack.lastSec) {
      msTrack.lastSec   = snap.second;
      msTrack.lastSecMs = millis();
    }
    uint32_t fracMs = millis() - msTrack.lastSecMs;
    if (fracMs > 999) fracMs = 999;
    return (float)fracMs / 1000.0f;
  }

  // Corner icons for the canvas faces — the SAME painters (and therefore the
  // same look + state behaviour) as the text faces; redrawn every frame
  // because the canvas repaints fully.
  void drawCanvasIcons(Arduino_Canvas *c, const Model &snap, uint16_t fg, uint16_t bg) {
    drawPowerIconTo(c, fg, bg);
    drawWifiFanTo(c, bg, snap.wifiEnabled, snap.wifiMode,
                  snap.wifiConnected, snap.wifiRssi);
    if (snap.batOk && snap.batLow) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%u%%", (unsigned)snap.batPct);
      c->setTextSize(1);
      c->setTextColor(RED);
      c->setCursor(uiCenterX(buf, 1), 6);
      c->print(buf);
    }
  }

  // Stopwatch / timer status lines for animated faces (small, bottom).
  void drawCanvasStatus(Arduino_Canvas *c, const Model &snap) {
    uint32_t epoch = snap.rtcOk
        ? rtcEpochSec(snap.year, snap.month, snap.day,
                      snap.hour, snap.minute, snap.second)
        : 0;
    char buf[24];
    if (stopwatchRunning()) {
      fmtClockMs(buf, sizeof(buf), "SW ", stopwatchElapsedMs(epoch, millis()));
      c->setTextSize(1);
      c->setTextColor(GREEN);
      c->setCursor(uiCenterX(buf, 1), 262);
      c->print(buf);
    }
    if (timerArmed()) {
      uint32_t remMs = timerRemainingSec(epoch) * 1000u;
      fmtClockMs(buf, sizeof(buf), "T-", remMs);
      c->setTextSize(1);
      c->setTextColor(ORANGE);
      c->setCursor(uiCenterX(buf, 1), 271);
      c->print(buf);
    }
  }

  void renderCustomFace(const Model &snap, uint8_t kind) {
    Arduino_Canvas *c = frameCanvas();
    if (!c || !c->getFramebuffer()) return;

    if (kind == 3) {
      // SM-logo face: repaint on the second tick / any displayed change (or
      // continuously at the 10 fps status cadence while the stopwatch /
      // timer lines are live).
      bool live = stopwatchRunning() || timerArmed();
      bool changed = live ||
                     snap.hour != cached.h || snap.minute != cached.m ||
                     snap.second != cached.s ||
                     snap.day != cached.day || snap.month != cached.month ||
                     snap.year != cached.year || snap.weekday != cached.weekday ||
                     snap.rtcOk != cached.rtcOk;
      if (!changed) return;
      drawSMLogoFace(c, snap);
      drawCanvasIcons(c, snap, WHITE, BLACK);
      drawCanvasStatus(c, snap);
      c->flush();
      cached.h = snap.hour; cached.m = snap.minute; cached.s = snap.second;
      cached.day = snap.day; cached.month = snap.month;
      cached.year = snap.year; cached.weekday = snap.weekday;
      cached.rtcOk = snap.rtcOk;
      return;
    }

    float sweep = secFraction(snap);
    c->fillScreen(cachedBg);
    if (kind == 1) drawAnalog(c, snap, sweep);
    else           drawOrbit(c, snap, sweep);
    drawCanvasIcons(c, snap, cachedFg, cachedBg);
    drawCanvasStatus(c, snap);
    c->flush();
  }

  // SM-logo watermark face (ported from the ScrubMarine slate): big HH:MM
  // over the dimmed logo background, date line beneath. Fixed white/silver
  // text — the watermark supplies the colour, not the theme.
  void drawSMLogoFace(Arduino_Canvas *c, const Model &snap) {
    memcpy(c->getFramebuffer(), SMICON_BG, sizeof(SMICON_BG));

    char buf[8];
    if (snap.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", snap.hour, snap.minute);
    else            strcpy(buf, "--:--");
    c->setFont(&FreeSansBold24pt7b);
    c->setTextSize(1);
    c->setTextColor(snap.rtcOk ? WHITE : RED);
    int16_t bx, by; uint16_t bw, bh;
    c->getTextBounds(buf, 0, 150, &bx, &by, &bw, &bh);
    c->setCursor((W - (int16_t)bw) / 2 - bx, 150);
    c->print(buf);
    c->setFont(nullptr);

    // Seconds row between the big time and the date.
    if (snap.rtcOk) {
      char sb[6];
      snprintf(sb, sizeof(sb), ":%02u", snap.second);
      c->setTextSize(2);
      c->setTextColor(0xC618);           // silver, matches the date line
      c->setCursor(uiCenterX(sb, 2), 164);
      c->print(sb);
    }

    if (snap.rtcOk) {
      static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
      static const char *kMon[]  = { "???","Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };
      const char *wd = (snap.weekday < 7) ? kWday[snap.weekday] : "---";
      const char *mn = (snap.month >= 1 && snap.month <= 12) ? kMon[snap.month] : "???";
      char db[24];
      snprintf(db, sizeof(db), "%s %u %s %u", wd, snap.day, mn, snap.year);
      c->setTextSize(2);
      c->setTextColor(0xC618);           // silver
      c->setCursor(uiCenterX(db, 2), 196);
      c->print(db);
    }
  }

  // A hand as a thin filled triangle from the hub to the tip.
  static void drawHand(Arduino_Canvas *c, int16_t cx, int16_t cy,
                       float angle, float len, float halfWidth, uint16_t col) {
    float dx = sinf(angle), dy = -cosf(angle);     // 12 o'clock = up
    float px = -dy, py = dx;                       // perpendicular
    int16_t tipX = (int16_t)(cx + dx * len);
    int16_t tipY = (int16_t)(cy + dy * len);
    int16_t b1X  = (int16_t)(cx + px * halfWidth - dx * 8);
    int16_t b1Y  = (int16_t)(cy + py * halfWidth - dy * 8);
    int16_t b2X  = (int16_t)(cx - px * halfWidth - dx * 8);
    int16_t b2Y  = (int16_t)(cy - py * halfWidth - dy * 8);
    c->fillTriangle(tipX, tipY, b1X, b1Y, b2X, b2Y, col);
  }

  void drawAnalog(Arduino_Canvas *c, const Model &snap, float sweep) {
    ThemeColors t = theme();
    const int16_t cx = 120, cy = 138, R = 104;

    // Dial ring + ticks (majors at 12/3/6/9).
    c->drawCircle(cx, cy, R,     t.line);
    c->drawCircle(cx, cy, R - 1, t.line);
    for (int i = 0; i < 12; i++) {
      float a = (float)i / 12.f * 6.2831853f;
      float dx = sinf(a), dy = -cosf(a);
      bool major = (i % 3) == 0;
      int16_t r0 = major ? R - 16 : R - 9;
      c->drawLine((int16_t)(cx + dx * r0),      (int16_t)(cy + dy * r0),
                  (int16_t)(cx + dx * (R - 3)), (int16_t)(cy + dy * (R - 3)),
                  major ? t.fg : t.line);
    }

    if (snap.rtcOk) {
      float secF  = (float)snap.second + sweep;
      float minF  = (float)snap.minute + secF / 60.f;
      float hourF = (float)(snap.hour % 12) + minF / 60.f;
      drawHand(c, cx, cy, hourF / 12.f * 6.2831853f, R * 0.52f, 4.f, t.fg);
      drawHand(c, cx, cy, minF  / 60.f * 6.2831853f, R * 0.78f, 3.f, t.fg);
      drawHand(c, cx, cy, secF  / 60.f * 6.2831853f, R * 0.90f, 1.4f,
               (t.accent == cachedBg) ? RED : t.accent);
    } else {
      c->setTextSize(2);
      c->setTextColor(RED);
      c->setCursor(uiCenterX("--:--", 2), cy - 8);
      c->print("--:--");
    }
    c->fillCircle(cx, cy, 5, t.fg);
    c->fillCircle(cx, cy, 2, cachedBg);

    // Date under the dial.
    if (snap.rtcOk) {
      static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
      char buf[16];
      snprintf(buf, sizeof(buf), "%s %u",
               (snap.weekday < 7) ? kWday[snap.weekday] : "---", snap.day);
      c->setTextSize(2);
      c->setTextColor(t.fg);
      c->setCursor(uiCenterX(buf, 2), 252);
      c->print(buf);
    }
  }

  void drawOrbit(Arduino_Canvas *c, const Model &snap, float sweep) {
    ThemeColors t = theme();
    const int16_t cx = 120, cy = 130;
    const int16_t rH = 38, rM = 66, rS = 96;

    // Fixed starfield background (hash noise so it doesn't shimmer).
    for (int i = 0; i < 40; i++) {
      uint32_t h = (uint32_t)(i * 2654435761u);
      int16_t sx = (int16_t)(h % 240);
      int16_t sy = (int16_t)((h >> 12) % 280);
      c->writePixel(sx, sy, DARKGREY);
    }

    // Orbit rings.
    c->drawCircle(cx, cy, rH, t.line);
    c->drawCircle(cx, cy, rM, t.line);
    c->drawCircle(cx, cy, rS, t.line);

    // Sun.
    c->fillCircle(cx, cy, 13, (t.accent == cachedBg) ? YELLOW : t.accent);

    if (snap.rtcOk) {
      float secF  = (float)snap.second + sweep;
      float minF  = (float)snap.minute + secF / 60.f;
      float hourF = (float)(snap.hour % 12) + minF / 60.f;
      float aH = hourF / 12.f * 6.2831853f;
      float aM = minF  / 60.f * 6.2831853f;
      float aS = secF  / 60.f * 6.2831853f;

      // Second: small comet with a fading dot trail.
      for (int k = 5; k >= 1; k--) {
        float a = aS - (float)k * 0.09f;
        int16_t px = (int16_t)(cx + sinf(a) * rS);
        int16_t py = (int16_t)(cy - cosf(a) * rS);
        c->fillCircle(px, py, 1, t.line);
      }
      c->fillCircle((int16_t)(cx + sinf(aS) * rS),
                    (int16_t)(cy - cosf(aS) * rS), 3, t.fg);
      // Minute planet.
      c->fillCircle((int16_t)(cx + sinf(aM) * rM),
                    (int16_t)(cy - cosf(aM) * rM), 6, CYAN);
      // Hour planet.
      c->fillCircle((int16_t)(cx + sinf(aH) * rH),
                    (int16_t)(cy - cosf(aH) * rH), 8, ORANGE);

      // Digital readout under the system (with seconds — every face shows
      // seconds one way or another).
      char buf[12];
      snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
               snap.hour, snap.minute, snap.second);
      c->setTextSize(2);
      c->setTextColor(t.fg);
      c->setCursor(uiCenterX(buf, 2), 246);
      c->print(buf);
    } else {
      c->setTextSize(2);
      c->setTextColor(RED);
      c->setCursor(uiCenterX("--:--", 2), 246);
      c->print("--:--");
    }
  }

  void sleepNow() {
    hapticBuzz(120, 80);
    if (gfx) {
      uiClearAll();
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
      gfx->setCursor(60, 130);
      gfx->print("sleeping");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    backlightOff();
    enterDeepSleep();   // does not return
  }
};

static WatchFaceView sView;
View *watchFaceViewPtr() { return &sView; }
