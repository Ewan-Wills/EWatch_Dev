#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>
#include "pins.h"
#include "power.h"
#include "display.h"
#include "haptic.h"
#include "view.h"
#include "event.h"
#include "controller.h"
#include "storage.h"
#include "apptimer.h"
#include "viewer3d.h"
#include "media.h"
#include "qr.h"
#include "anim_demo.h"
#include "stopwatch.h"
#include "timer_app.h"
#include "wifi_svc.h"

// ---------- shared draw helpers ----------
static const int16_t W = 240;
static const int16_t H = 280;

static void clearAll() { if (gfx) gfx->fillScreen(BLACK); }

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

static int16_t centerX(const char *s, uint8_t size) {
  // GFX glyph box = 6 px wide * size, with one px gap. Approximate.
  int16_t w = (int16_t)strlen(s) * 6 * size;
  return (W - w) / 2;
}

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}

// Top-left back chevron. Wider/taller for easier tapping.
static const int16_t BACK_W = 60, BACK_H = 42;
static void drawBackButton() {
  if (!gfx) return;
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(WHITE, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}
// Generous hit zone so a thumb tap registers reliably.
static bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}

// =====================================================================
// Watch face — default view.
// Centered HH:MM, secondary :SS row. Any touch -> AppList.
// =====================================================================
class WatchFaceView : public View {
public:
  void onEnter() override {
    { ModelLock lk; cachedBg = model.bgColor; cachedFg = model.fgColor; }
    if (gfx) {
      gfx->fillScreen(cachedBg);
      drawPowerIcon();
      drawWifiIcon(/*force=*/true);
    }
    cached.h = 99; cached.m = 99; cached.s = 99; cached.rtcOk = false;
    cached.day = 0; cached.month = 0; cached.year = 0; cached.weekday = 9;
    cached.swRun = false; cached.swSec = 0xFFFFFFFF;
    cached.tmrOn = false; cached.tmrSec = 0xFFFFFFFF;
  }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    // Repaint the background and force time/seconds/date redraw if the user
    // has changed colors since we last entered.
    if (snap.bgColor != cachedBg || snap.fgColor != cachedFg) {
      gfx->fillScreen(snap.bgColor);
      cachedBg = snap.bgColor;
      cachedFg = snap.fgColor;
      drawPowerIcon();
      drawWifiIcon(/*force=*/true);
      cached.h = 99; cached.m = 99; cached.s = 99; cached.rtcOk = false;
      cached.day = 0; cached.month = 0; cached.year = 0; cached.weekday = 9;
      cached.swRun = false; cached.swSec = 0xFFFFFFFF;
      cached.tmrOn = false; cached.tmrSec = 0xFFFFFFFF;
    }

    drawWifiIcon(/*force=*/false);

    // Big time HH:MM — vertically nudged down a touch now that the header is gone.
    if (snap.hour != cached.h || snap.minute != cached.m ||
        !snap.rtcOk != !cached.rtcOk) {
      gfx->fillRect(0, 100, W, 60, cachedBg);
      gfx->setTextSize(6);
      gfx->setTextColor(snap.rtcOk ? cachedFg : RED, cachedBg);
      char buf[8];
      if (snap.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", snap.hour, snap.minute);
      else            strcpy(buf, "--:--");
      gfx->setCursor(centerX(buf, 6), 108);
      gfx->print(buf);
      cached.h = snap.hour; cached.m = snap.minute; cached.rtcOk = snap.rtcOk;
    }

    // Seconds.
    if (snap.second != cached.s) {
      gfx->fillRect(0, 172, W, 22, cachedBg);
      gfx->setTextSize(3);
      gfx->setTextColor(cachedFg, cachedBg);
      char buf[8];
      snprintf(buf, sizeof(buf), ":%02u", snap.second);
      gfx->setCursor(centerX(buf, 3), 174);
      gfx->print(buf);
      cached.s = snap.second;
    }

    // Date below the seconds row. Redraw only when it changes.
    if (snap.day != cached.day || snap.month != cached.month ||
        snap.year != cached.year || snap.weekday != cached.weekday) {
      gfx->fillRect(0, 210, W, 20, cachedBg);
      if (snap.rtcOk) {
        static const char *kWday[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
        static const char *kMon[]  = { "???","Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec" };
        const char *wd = (snap.weekday < 7) ? kWday[snap.weekday] : "---";
        const char *mn = (snap.month >= 1 && snap.month <= 12) ? kMon[snap.month] : "???";
        char buf[20];
        snprintf(buf, sizeof(buf), "%s %u %s %u", wd, snap.day, mn, snap.year);
        gfx->setTextSize(2);
        gfx->setTextColor(cachedFg, cachedBg);
        gfx->setCursor(centerX(buf, 2), 212);
        gfx->print(buf);
      }
      cached.day = snap.day; cached.month = snap.month;
      cached.year = snap.year; cached.weekday = snap.weekday;
    }

    // Stopwatch / timer status lines — shown only while each is active.
    uint32_t epoch = snap.rtcOk
        ? rtcEpochSec(snap.year, snap.month, snap.day,
                      snap.hour, snap.minute, snap.second)
        : 0;

    bool     swRun = stopwatchRunning();
    uint32_t swSec = swRun ? stopwatchElapsedMs(epoch, millis()) / 1000u : 0;
    if (swRun != cached.swRun || swSec != cached.swSec) {
      gfx->fillRect(0, 236, W, 18, cachedBg);
      if (swRun) {
        char buf[20];
        fmtClock(buf, sizeof(buf), "SW ", swSec);
        gfx->setTextSize(2);
        gfx->setTextColor(GREEN, cachedBg);
        gfx->setCursor(centerX(buf, 2), 238);
        gfx->print(buf);
      }
      cached.swRun = swRun; cached.swSec = swSec;
    }

    bool     tmrOn  = timerArmed();
    uint32_t tmrSec = tmrOn ? timerRemainingSec(epoch) : 0;
    if (tmrOn != cached.tmrOn || tmrSec != cached.tmrSec) {
      gfx->fillRect(0, 258, W, 20, cachedBg);
      if (tmrOn) {
        char buf[20];
        fmtClock(buf, sizeof(buf), "T-", tmrSec);
        gfx->setTextSize(2);
        gfx->setTextColor(ORANGE, cachedBg);
        gfx->setCursor(centerX(buf, 2), 260);
        gfx->print(buf);
      }
      cached.tmrOn = tmrOn; cached.tmrSec = tmrSec;
    }
  }
  void onEvent(const Event &e) override {
    // Swipe-left is the only way into the app list now — a plain tap stays
    // on the watch face so the user has to commit a gesture intentionally.
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
      pressInPower = inRect(e.x, e.y, PWR_HIT_X, PWR_HIT_Y, PWR_HIT_W, PWR_HIT_H);
      pressInWifi  = inRect(e.x, e.y, WIFI_HIT_X, WIFI_HIT_Y, WIFI_HIT_W, WIFI_HIT_H);
      pressMoved = false;
      pressActive = true;
      return;
    }
    if (e.type == EventType::TouchHold && pressActive) {
      int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
      if (dx * dx + dy * dy > 16 * 16) pressMoved = true;
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
  // can land 10-15 px off, so a tight bbox makes the button feel broken. We
  // kept it well clear of the date row (drawn at y=210+).
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

  // Cached WiFi-icon state so we only redraw on visual change.
  struct {
    bool     en;
    WifiMode mode;
    bool     conn;
    int8_t   bars;     // 0..3
  } wifiCache{false, WifiMode::AP, false, -1};

  uint16_t cachedBg = BLACK;
  uint16_t cachedFg = WHITE;
  struct {
    uint8_t  h, m, s;
    bool     rtcOk;
    uint8_t  day, month, weekday;
    uint16_t year;
    bool     swRun, tmrOn;
    uint32_t swSec, tmrSec;
  } cached{99,99,99,false,0,0,9,0,false,false,0xFFFFFFFF,0xFFFFFFFF};

  // "prefix" + H:MM:SS / MM:SS depending on magnitude.
  static void fmtClock(char *buf, size_t n, const char *prefix, uint32_t sec) {
    uint32_t hh = sec / 3600, mm = (sec % 3600) / 60, ss = sec % 60;
    if (hh > 0) snprintf(buf, n, "%s%lu:%02lu:%02lu", prefix,
                         (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
    else        snprintf(buf, n, "%s%02lu:%02lu", prefix,
                         (unsigned long)mm, (unsigned long)ss);
  }

  void drawPowerIcon() {
    // Two-pixel-thick ring + erased top notch + double-thick stem.
    gfx->drawCircle(PWR_CX, PWR_CY, PWR_R,     cachedFg);
    gfx->drawCircle(PWR_CX, PWR_CY, PWR_R - 1, cachedFg);
    gfx->fillRect(PWR_CX - 2, PWR_CY - PWR_R - 2, 5, 5, cachedBg);
    gfx->drawFastVLine(PWR_CX,     PWR_CY - PWR_R + 1, PWR_R - 2, cachedFg);
    gfx->drawFastVLine(PWR_CX + 1, PWR_CY - PWR_R + 1, PWR_R - 2, cachedFg);
  }

  // WiFi fan icon — three concentric top-half arcs above a center dot. Off
  // when disabled, yellow (full) when hosting an AP, green with bars from
  // RSSI when connected as a client, orange (dot only) when scanning.
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

    if (!force &&
        wifiCache.en == en && wifiCache.mode == mode &&
        wifiCache.conn == conn && wifiCache.bars == bars) return;
    wifiCache = { en, mode, conn, bars };

    // GFX 1.4.7 doesn't expose drawCircleHelper, so we draw full circles and
    // erase the bottom half. The dot is then drawn on top.
    gfx->fillRect(2, 4, 56, 46, cachedBg);
    uint16_t off = DARKGREY;
    uint16_t cDot = en ? col : off;
    uint16_t cA1  = (en && bars >= 1) ? col : off;
    uint16_t cA2  = (en && bars >= 2) ? col : off;
    uint16_t cA3  = (en && bars >= 3) ? col : off;
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R1,     cA1);
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R1 + 1, cA1);
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R2,     cA2);
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R2 + 1, cA2);
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R3,     cA3);
    gfx->drawCircle(WIFI_CX, WIFI_CY, WIFI_R3 + 1, cA3);
    gfx->fillRect(2, WIFI_CY + 1, 56, 22, cachedBg);
    gfx->fillCircle(WIFI_CX, WIFI_CY - 2, 2, cDot);
#endif  // EWATCH_ENABLE_WIFI
  }

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

  void sleepNow() {
    hapticBuzz(120, 80);
    if (gfx) {
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(60, 130);
      gfx->print("sleeping");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    backlightOff();
    enterDeepSleep();   // does not return
  }
};

// =====================================================================
// Carousel — one big tile (~3/4 of the screen) shown at a time, with eased
// vertical scroll between selections, an animated entrance, a press pulse
// on tap, and live page-indicator dots. Uses the shared frame canvas so
// transitions stay flicker-free. The render() loop blocks while animation
// is in flight (same pattern the video player + anim demo use) and returns
// when settled so the controller's auto-sleep timer still works.
// =====================================================================
struct AppEntry {
  const char *name;
  uint16_t    color;
  Screen      target;
  const char *subtitle;            // short description (may be "")
};

// Set to 0 to silence carousel instrumentation once the bug is found.
#define CAROUSEL_DEBUG 1

#if CAROUSEL_DEBUG
  // Flush after every print: when the device panics we lose anything still in
  // the UART FIFO, so the very last log line — which is the one that points at
  // the crash — would otherwise be dropped.
  #define CAR_LOG(fmt, ...) do { \
      Serial.printf("[CAR %lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); \
      Serial.flush(); \
    } while (0)
#else
  #define CAR_LOG(fmt, ...) ((void)0)
#endif

#include <esp_heap_caps.h>
static void carHeapCheck(const char *tag) {
#if CAROUSEL_DEBUG
  bool ok = heap_caps_check_integrity_all(true);
  Serial.printf("[CAR %lu] HEAP[%s] integrity=%d free=%u min_free=%u\n",
                (unsigned long)millis(), tag, (int)ok,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  Serial.flush();
#else
  (void)tag;
#endif
}

class CarouselView : public View {
public:
  CarouselView(const AppEntry *entries, int n,
               const char *title, Screen back, bool wrap = false)
    : entries(entries), N(n), title(title), backScreen(back), wrapAround(wrap) {}

  void onEnter() override {
    CAR_LOG("onEnter this=%p vt=%p title=%s N=%d wrap=%d entries=%p back=%d",
            (void*)this, *(void**)this, title ? title : "(null)", N,
            (int)wrapAround, (const void*)entries, (int)backScreen);
    canvas = frameCanvas();
    if (!canvas) {
      CAR_LOG("onEnter: frameCanvas() returned null — bailing");
      return;
    }
    cur          = 0;
    // Tile slides in from below on entry.
    scrollPos    = -1.0f;
    scrollFrom   = -1.0f;
    scrollTo     = 0.0f;
    scrollStart  = millis();
    scrollDuration = 500;
    pulseStart   = 0;
    exitAtMs     = 0;
    exitTarget   = backScreen;
    pressActive  = false;
    pressMoved   = false;
    logStack("onEnter");
    logInvariants("onEnter");
    carHeapCheck("onEnter");
    // Dump every entry pointer so we'd see a corrupted name/subtitle ptr
    // before drawTiles dereferences it and crashes.
    for (int i = 0; i < N && i < 16; i++) {
      CAR_LOG("entries[%d] name=%p subtitle=%p target=%d color=0x%04x",
              i, (const void*)entries[i].name,
              (const void*)entries[i].subtitle,
              (int)entries[i].target, (unsigned)entries[i].color);
    }
  }
  void onExit() override {
    CAR_LOG("onExit this=%p vt=%p title=%s cur=%d scrollPos=%.3f scrollTo=%.3f",
            (void*)this, *(void**)this, title ? title : "(null)",
            cur, (double)scrollPos, (double)scrollTo);
  }
  void render() override {
    if (!canvas) {
      CAR_LOG("render: canvas==null — bailing");
      return;
    }
    CAR_LOG("render ENTER this=%p vt=%p cur=%d sp=%.3f sf=%.3f st=%.3f anim=%d",
            (void*)this, *(void**)this, cur, (double)scrollPos,
            (double)scrollFrom, (double)scrollTo, (int)isAnimating());
    logInvariants("render-enter");
    logStack("render-enter");

    uint32_t frameCount = 0;
    // Drive frames until either (a) the user leaves the view or (b) the
    // animation settles. When settled we return; the controller will call
    // render() again on the next event.
    do {
      uint32_t t0 = millis();
      drawFrame();
      CAR_LOG("render: drawFrame done, about to flush()");
      canvas->flush();
      CAR_LOG("render: flush done");
      frameCount++;
      if ((frameCount & 0x0F) == 0) {
        CAR_LOG("render frame=%lu sp=%.3f sf=%.3f st=%.3f cur=%d",
                (unsigned long)frameCount, (double)scrollPos,
                (double)scrollFrom, (double)scrollTo, cur);
        logStack("render-frame");
      }

      // Deferred exit (e.g. press pulse → switchTo after 200 ms).
      if (exitAtMs > 0 && (int32_t)(millis() - exitAtMs) >= 0) {
        Screen target = exitTarget;
        exitAtMs = 0;
        CAR_LOG("render: deferred exit firing → switchTo(%d) (this=%p vt=%p)",
                (int)target, (void*)this, *(void**)this);
        switchTo(target);
        return;
      }
      if (!isAnimating()) {
        CAR_LOG("render EXIT (settled) frames=%lu cur=%d sp=%.3f",
                (unsigned long)frameCount, cur, (double)scrollPos);
        return;
      }

      // Drain events with a ~30 fps frame budget.
      int32_t budget = 33 - (int32_t)(millis() - t0);
      if (budget < 1) budget = 1;
      while (budget > 0) {
        Event e;
        if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) != pdPASS) break;
        CAR_LOG("render inline event type=%d gesture=%d x=%u y=%u (mid-anim)",
                (int)e.type, (int)e.gesture, (unsigned)e.x, (unsigned)e.y);
        handleEvent(e);
        if (exitAtMs > 0 && (int32_t)(millis() - exitAtMs) >= 0) break;
        budget = 33 - (int32_t)(millis() - t0);
      }
    } while (true);
  }
  void onEvent(const Event &e) override {
    CAR_LOG("onEvent ENTER this=%p vt=%p type=%d gesture=%d x=%u y=%u cur=%d sp=%.3f",
            (void*)this, *(void**)this, (int)e.type, (int)e.gesture,
            (unsigned)e.x, (unsigned)e.y, cur, (double)scrollPos);
    logInvariants("onEvent");
    // The settled state delivers events through here; the animated state
    // drains them inline. Both go through handleEvent.
    handleEvent(e);
    // If a new animation kicked off, bump revision so the controller
    // re-enters render() right away.
    if (isAnimating()) { ModelLock lk; model.revision++; }
  }

private:
  const AppEntry *entries;
  const int       N;
  const char     *title;
  const Screen    backScreen;
  const bool      wrapAround;

  Arduino_Canvas *canvas = nullptr;
  int      cur = 0;
  float    scrollPos = 0.f;
  float    scrollFrom = 0.f, scrollTo = 0.f;
  uint32_t scrollStart = 0;
  uint32_t scrollDuration = 350;

  uint32_t pulseStart = 0;
  static const uint32_t kPulseMs = 220;

  uint32_t exitAtMs = 0;
  Screen   exitTarget;

  bool     pressActive = false;
  bool     pressMoved  = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressTime = 0;
  static const int kMoveThreshSq = 16 * 16;

  // Tile sizing: ~3/4 of the screen height for the active card.
  static const int16_t TILE_W   = 200;
  static const int16_t TILE_H   = 210;       // 280 * 0.75 ≈ 210
  static const int16_t TILE_CY  = 152;       // center of stage
  static const int16_t STRIDE   = TILE_H + 24;

  bool isAnimating() const {
    if (scrollPos != scrollTo) return true;
    if (pulseStart > 0 && (millis() - pulseStart) < kPulseMs) return true;
    if (exitAtMs > 0) return true;
    return false;
  }
  static float easeOutCubic(float t) {
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float u = 1.f - t;
    return 1.f - u * u * u;
  }
  void advanceScrollTween() {
    if (scrollPos == scrollTo) return;
    uint32_t now = millis();
    uint32_t elapsed = now - scrollStart;
    if (elapsed >= scrollDuration) {
      float prev = scrollPos;
      scrollPos = scrollTo;
      if (wrapAround) {
        // Re-anchor into [0, N) so cur and scrollPos agree, and so the next
        // wrap snap doesn't drift further outside the canonical range.
        if (scrollPos < 0)         scrollPos += (float)N;
        if (scrollPos >= (float)N) scrollPos -= (float)N;
        scrollTo   = scrollPos;
        scrollFrom = scrollPos;
      }
      CAR_LOG("tween SETTLE prev=%.3f → sp=%.3f cur=%d (this=%p)",
              (double)prev, (double)scrollPos, cur, (void*)this);
      logInvariants("tween-settle");
    } else {
      float p = (float)elapsed / (float)scrollDuration;
      scrollPos = scrollFrom + (scrollTo - scrollFrom) * easeOutCubic(p);
      if (isnan(scrollPos) || isinf(scrollPos)) {
        CAR_LOG("!!! tween produced bad sp=%.3f from sf=%.3f st=%.3f p=%.3f elapsed=%lu dur=%lu",
                (double)scrollPos, (double)scrollFrom, (double)scrollTo,
                (double)p, (unsigned long)elapsed, (unsigned long)scrollDuration);
      }
    }
  }
  float currentPulseScale() {
    if (pulseStart == 0) return 1.0f;
    uint32_t elapsed = millis() - pulseStart;
    if (elapsed >= kPulseMs) { return 1.0f; }
    float p = (float)elapsed / (float)kPulseMs;
    // Sine bump: 1.0 -> 1.18 -> 1.0
    return 1.0f + 0.18f * sinf(p * 3.14159265f);
  }
  void startScroll(int newIndex) {
    int requested = newIndex;
    float spBefore = scrollPos;
    CAR_LOG("startScroll ENTER req=%d cur=%d sp=%.3f sf=%.3f st=%.3f wrap=%d N=%d (this=%p)",
            requested, cur, (double)scrollPos, (double)scrollFrom,
            (double)scrollTo, (int)wrapAround, N, (void*)this);
    logInvariants("startScroll-enter");
    if (wrapAround) {
      // Snap scrollPos so the wrapped tile sits exactly one slot from center
      // in the natural direction, then tween that one slot. drawTiles + the
      // indicator both use modular arithmetic on idx so out-of-range scrollPos
      // is fine — it represents "we're visually past the end / before the
      // start" and the tween settles back into [0, N) when it completes.
      if (newIndex < 0) {
        scrollPos += (float)N;        // 0 -> N, so cur=N-1 sits at scrollPos-1
        newIndex += N;
        CAR_LOG("startScroll WRAP- (down past 0) sp %.3f→%.3f newIdx %d→%d",
                (double)spBefore, (double)scrollPos, requested, newIndex);
      } else if (newIndex >= N) {
        scrollPos -= (float)N;        // N-1 -> -1, so cur=0 sits at scrollPos+1
        newIndex -= N;
        CAR_LOG("startScroll WRAP+ (up past N-1) sp %.3f→%.3f newIdx %d→%d",
                (double)spBefore, (double)scrollPos, requested, newIndex);
      }
      if (newIndex < 0 || newIndex >= N) {
        CAR_LOG("!!! startScroll: post-wrap newIndex=%d still OOB (N=%d)",
                newIndex, N);
        return;
      }
    } else {
      if (newIndex < 0 || newIndex >= N) {
        CAR_LOG("startScroll: non-wrap OOB newIndex=%d ignored", newIndex);
        return;
      }
    }
    cur = newIndex;
    scrollFrom = scrollPos;
    scrollTo = (float)newIndex;
    scrollStart = millis();
    scrollDuration = 320;
    CAR_LOG("startScroll EXIT cur=%d sf=%.3f st=%.3f delta=%.3f",
            cur, (double)scrollFrom, (double)scrollTo,
            (double)(scrollTo - scrollFrom));
    logInvariants("startScroll-exit");
    hapticBuzz(40, 45);
  }

  void drawFrame() {
    CAR_LOG("drawFrame ENTER canvas=%p (vt=%p) sp=%.3f cur=%d N=%d entries=%p",
            (void*)canvas, canvas ? *(void**)canvas : nullptr,
            (double)scrollPos, cur, N, (const void*)entries);
    advanceScrollTween();
    CAR_LOG("drawFrame after tween sp=%.3f", (double)scrollPos);
    canvas->fillScreen(BLACK);
    CAR_LOG("drawFrame after fillScreen");
    drawChrome();
    CAR_LOG("drawFrame after chrome");
    drawTiles();
    CAR_LOG("drawFrame after tiles");
    drawIndicator();
    CAR_LOG("drawFrame after indicator");
  }
  void drawChrome() {
    // Back chevron.
    canvas->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
    canvas->setTextColor(WHITE, DARKGREY);
    canvas->setTextSize(3);
    canvas->setCursor(18, 12);
    canvas->print('<');
    // Title.
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE, BLACK);
    int16_t tw = (int16_t)strlen(title) * 12;
    canvas->setCursor((W - tw) / 2, 14);
    canvas->print(title);
    canvas->drawFastHLine(20, 44, 200, DARKGREY);
  }
  void drawTiles() {
    float pulse = currentPulseScale();
    // Guard against a corrupt scrollPos turning the visible-window loop into
    // a giant or undefined range (floor/ceil of NaN/Inf, cast to int → UB).
    if (isnan(scrollPos) || isinf(scrollPos)) {
      CAR_LOG("!!! drawTiles: scrollPos invalid=%.6f — snapping to 0", (double)scrollPos);
      scrollPos = 0.f; scrollFrom = 0.f; scrollTo = 0.f;
    }
    // Visible window: index +/-1 around the currently displayed position.
    int low  = (int)floorf(scrollPos) - 1;
    int high = (int)ceilf (scrollPos) + 1;
    if (high - low > 8 || N <= 0) {
      CAR_LOG("!!! drawTiles: bad window low=%d high=%d N=%d sp=%.3f — clamping",
              low, high, N, (double)scrollPos);
      low = -1; high = 1;
    }
    for (int idx = low; idx <= high; idx++) {
      int dataIdx;
      if (wrapAround) {
        dataIdx = ((idx % N) + N) % N;
      } else {
        if (idx < 0 || idx >= N) continue;
        dataIdx = idx;
      }
      if (dataIdx < 0 || dataIdx >= N) {
        CAR_LOG("!!! drawTiles: dataIdx=%d OOB (idx=%d N=%d sp=%.3f) — skip",
                dataIdx, idx, N, (double)scrollPos);
        continue;
      }
      float offset = (float)idx - scrollPos;          // tile units
      int16_t cy = (int16_t)(TILE_CY + offset * STRIDE);
      if (cy - TILE_H / 2 > H || cy + TILE_H / 2 < 50) continue;

      bool centred = (dataIdx == cur) && (scrollPos == scrollTo);
      float s = centred ? pulse : 1.0f;

      int16_t tw = (int16_t)(TILE_W * s);
      int16_t th = (int16_t)(TILE_H * s);
      int16_t tx = (W - tw) / 2;
      int16_t ty = cy - th / 2;

      uint16_t fg = entries[dataIdx].color;
      // Tile body: Arduino_Canvas's fillRect does NOT clip negative coords
      // — a partially off-canvas fillRoundRect computes `framebuffer + y*W + x`
      // with negative y and trampoles into the heap block in front of the
      // canvas buffer, which manifested as random-looking StoreProhibited
      // panics inside the WiFi stack (NAVY pixels (0x000f) clobbering an
      // RX buffer / TLSF header). When the tile is partially off-canvas we
      // skip the rounded fill and draw a clamped fillRect of the visible
      // portion instead; the rounded corners that would be off-canvas aren't
      // visible anyway.
      bool fullyOnCanvas = (tx >= 0 && ty >= 0 && tx + tw <= W && ty + th <= H);
      if (fullyOnCanvas) {
        canvas->fillRoundRect(tx, ty, tw, th, 16, fg);
        canvas->drawRoundRect(tx, ty, tw, th, 16, WHITE);
      } else {
        int16_t cx0 = tx < 0 ? 0 : tx;
        int16_t cy0 = ty < 0 ? 0 : ty;
        int16_t cx1 = (tx + tw) > W ? W : (tx + tw);
        int16_t cy1 = (ty + th) > H ? H : (ty + th);
        if (cx1 > cx0 && cy1 > cy0) {
          canvas->fillRect(cx0, cy0, cx1 - cx0, cy1 - cy0, fg);
        }
      }

      // Text: only draw when the baseline is on-canvas. setTextColor with a
      // background uses an opaque-fill path inside drawChar that, for v1.4.7,
      // is just as unclipped as fillRect — so an off-canvas cursor is the
      // same hazard as an off-canvas tile body.
      int16_t titleY = cy - 16;
      if (titleY >= 0 && titleY + 24 <= H) {
        canvas->setTextColor(WHITE, fg);
        canvas->setTextSize(3);
        int16_t labelW = (int16_t)strlen(entries[dataIdx].name) * 18;
        canvas->setCursor(tx + (tw - labelW) / 2, titleY);
        canvas->print(entries[dataIdx].name);
      }
      int16_t subY = cy + 16;
      if (entries[dataIdx].subtitle && *entries[dataIdx].subtitle &&
          subY >= 0 && subY + 16 <= H) {
        canvas->setTextSize(2);
        int16_t subW = (int16_t)strlen(entries[dataIdx].subtitle) * 12;
        canvas->setCursor(tx + (tw - subW) / 2, subY);
        canvas->print(entries[dataIdx].subtitle);
      }
    }
  }
  void drawIndicator() {
    if (N <= 1) return;
    int16_t y = H - 14;
    int16_t spacing = 14;
    int16_t totalW = (N - 1) * spacing;
    int16_t startX = (W - totalW) / 2;
    // Normalize scrollPos into [0, N) for dot calculations, and (when
    // wrapping) consider the shorter wrap-around distance so the active dot
    // stays on the actual target during the cross-boundary tween.
    float pos = scrollPos;
    if (wrapAround) {
      pos = fmodf(pos, (float)N);
      if (pos < 0) pos += (float)N;
    }
    for (int i = 0; i < N; i++) {
      float dist = fabsf((float)i - pos);
      if (wrapAround) {
        float dist2 = (float)N - dist;
        if (dist2 < dist) dist = dist2;
      }
      if (dist > 1.f) dist = 1.f;
      int16_t radius = (int16_t)(3.f + (1.f - dist) * 3.f);
      uint16_t col = (dist < 0.5f) ? WHITE : DARKGREY;
      canvas->fillCircle(startX + i * spacing, y, radius, col);
    }
  }

  void handleEvent(const Event &e) {
    CAR_LOG("handleEvent type=%d gesture=%d x=%u y=%u (cur=%d sp=%.3f st=%.3f this=%p vt=%p)",
            (int)e.type, (int)e.gesture, (unsigned)e.x, (unsigned)e.y,
            cur, (double)scrollPos, (double)scrollTo,
            (void*)this, *(void**)this);
    if (e.type == EventType::ButtonShort) {
      exitTarget = backScreen;
      exitAtMs   = millis();
      return;
    }
    if (e.type == EventType::TimerExpired) {
      exitTarget = Screen::Timer;
      exitAtMs   = millis();
      return;
    }
    if (e.type == EventType::Gesture) {
      if (e.gesture == Gesture::SwipeUp)   {
        CAR_LOG("gesture SwipeUp → startScroll(%d)", cur + 1);
        startScroll(cur + 1); return;
      }
      if (e.gesture == Gesture::SwipeDown) {
        CAR_LOG("gesture SwipeDown → startScroll(%d)", cur - 1);
        startScroll(cur - 1); return;
      }
      return;
    }
    if (e.type == EventType::Touch) {
      if (tappedBack(e.x, e.y)) {
        exitTarget = backScreen;
        exitAtMs   = millis();
        return;
      }
      pressActive = true;
      pressMoved  = false;
      pressX = e.x; pressY = e.y;
      pressTime = millis();
      return;
    }
    if (e.type == EventType::TouchHold && pressActive) {
      int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
      if (dx * dx + dy * dy > kMoveThreshSq) pressMoved = true;
      return;
    }
    if (e.type == EventType::TouchUp) {
      bool quick = (millis() - pressTime) < 600;
      if (pressActive && !pressMoved && quick && hitCurrentTile(pressX, pressY)) {
        pulseStart = millis();
        if (cur < 0 || cur >= N) {
          CAR_LOG("!!! tap-launch BAD cur=%d N=%d entries=%p — skip",
                  cur, N, (const void*)entries);
          pressActive = false; pressMoved = false; return;
        }
        CAR_LOG("tap-launch cur=%d → target=%d", cur, (int)entries[cur].target);
        exitTarget = entries[cur].target;
        exitAtMs   = millis() + 180;
        hapticBuzz(80, 70);
      }
      pressActive = false;
      pressMoved  = false;
      return;
    }
  }
  bool hitCurrentTile(int x, int y) {
    if (scrollPos != scrollTo) return false;       // ignore taps mid-scroll
    int16_t tx = (W - TILE_W) / 2;
    int16_t ty = TILE_CY - TILE_H / 2;
    return inRect((uint16_t)x, (uint16_t)y, tx, ty, TILE_W, TILE_H);
  }

  // Per-call sanity probe: catches state corruption before it crashes.
  // Logs only when something is off so steady-state traffic stays readable.
  void logInvariants(const char *tag) {
#if CAROUSEL_DEBUG
    bool bad = false;
    if (N <= 0 || N > 64) {
      CAR_LOG("INV[%s] BAD N=%d (this=%p vt=%p)", tag, N, (void*)this, *(void**)this);
      bad = true;
    }
    if (cur < 0 || (N > 0 && cur >= N)) {
      CAR_LOG("INV[%s] BAD cur=%d (N=%d this=%p)", tag, cur, N, (void*)this);
      bad = true;
    }
    if (isnan(scrollPos) || isinf(scrollPos)) {
      CAR_LOG("INV[%s] BAD scrollPos=%.6f (this=%p)", tag, (double)scrollPos, (void*)this);
      bad = true;
    }
    if (isnan(scrollFrom) || isinf(scrollFrom) || isnan(scrollTo) || isinf(scrollTo)) {
      CAR_LOG("INV[%s] BAD sf=%.6f st=%.6f", tag, (double)scrollFrom, (double)scrollTo);
      bad = true;
    }
    if (!entries) {
      CAR_LOG("INV[%s] BAD entries=null", tag);
      bad = true;
    }
    if (!title) {
      CAR_LOG("INV[%s] BAD title=null", tag);
      bad = true;
    }
    if (canvas == nullptr) {
      CAR_LOG("INV[%s] note canvas=null", tag);
    }
    // Extremely large scrollPos magnitude indicates drift outside the
    // wrap re-anchor range — flag for inspection.
    if (fabsf(scrollPos) > (float)(N * 4 + 16)) {
      CAR_LOG("INV[%s] DRIFT scrollPos=%.3f (N=%d cur=%d)",
              tag, (double)scrollPos, N, cur);
      bad = true;
    }
    (void)bad;
#else
    (void)tag;
#endif
  }

  void logStack(const char *tag) {
#if CAROUSEL_DEBUG
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    CAR_LOG("STACK[%s] hwm=%u free_heap=%u (this=%p task=%s)",
            tag, (unsigned)hwm, (unsigned)ESP.getFreeHeap(),
            (void*)this, pcTaskGetName(nullptr));
#else
    (void)tag;
#endif
  }
};

// =====================================================================
// Sensor Test — diagnostic readout. RTC, IMU bars, touch, button, INTs,
// battery, uptime/heap. Swipe right or long-press SW2 -> AppList.
// =====================================================================
class SensorTestView : public View {
public:
  void onEnter() override { clearAll(); firstDraw = true; }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    bool intRtc  = digitalRead(PIN_RTC_INT);
    bool intMma1 = digitalRead(PIN_MMA_INT1);
    bool intMma2 = digitalRead(PIN_MMA_INT2);

    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(60, 8);
      gfx->print("Sensors");
      gfx->drawFastHLine(8, 32, W - 16, DARKGREY);
      firstDraw = false;
    }

    // RTC time (size 2)
    gfx->fillRect(0, 42, W, 18, BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(snap.rtcOk ? YELLOW : RED, BLACK);
    gfx->setCursor(8, 42);
    if (snap.rtcOk) gfx->printf("RTC %02u:%02u:%02u",
                                snap.hour, snap.minute, snap.second);
    else            gfx->print("RTC ---");

    // Battery
    gfx->fillRect(0, 62, W, 18, BLACK);
    gfx->setTextColor(snap.batOk ? CYAN : RED, BLACK);
    gfx->setCursor(8, 62);
    if (snap.batOk) gfx->printf("Bat %.2fV %3u%%", snap.vbat, snap.batPct);
    else            gfx->print("Bat ---");

    // IMU
    gfx->fillRect(0, 88, W, 70, BLACK);
    gfx->setTextSize(2);
    if (snap.imuOk) {
      gfx->setTextColor(RED,   BLACK); gfx->setCursor(8, 90);
      gfx->printf("X %+5d", snap.ax);
      gfx->setTextColor(GREEN, BLACK); gfx->setCursor(8, 110);
      gfx->printf("Y %+5d", snap.ay);
      gfx->setTextColor(BLUE,  BLACK); gfx->setCursor(8, 130);
      gfx->printf("Z %+5d", snap.az);
      drawAxisBar(130, 91, 100, 12, snap.ax, RED);
      drawAxisBar(130, 111, 100, 12, snap.ay, GREEN);
      drawAxisBar(130, 131, 100, 12, snap.az, BLUE);
    } else {
      gfx->setTextColor(RED, BLACK);
      gfx->setCursor(8, 110);
      gfx->print("IMU ERR");
    }

    // Touch
    gfx->fillRect(0, 165, W, 18, BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(MAGENTA, BLACK);
    gfx->setCursor(8, 165);
    if (haveTouch) gfx->printf("Tch %3u,%3u", lastTx, lastTy);
    else           gfx->print("Tch ---");

    // Button
    gfx->fillRect(0, 187, W, 18, BLACK);
    gfx->setTextColor(snap.button ? GREEN : DARKGREY, BLACK);
    gfx->setCursor(8, 187);
    gfx->printf("Btn %s", snap.button ? "DOWN" : "up  ");

    // INT lines
    gfx->fillRect(0, 210, W, 14, BLACK);
    gfx->setTextSize(1);
    auto drawInt = [&](int16_t x, const char *lbl, bool h) {
      gfx->setTextColor(h ? GREEN : RED, BLACK);
      gfx->setCursor(x, 213);
      gfx->printf("%s:%c", lbl, h ? 'H' : 'L');
    };
    drawInt(8,   "RTC", intRtc);
    drawInt(80,  "M1",  intMma1);
    drawInt(140, "M2",  intMma2);

    // System diagnostics
    gfx->fillRect(0, 230, W, 14, BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(8, 233);
    gfx->printf("up %lus  heap %luk  psram %luk",
                (unsigned long)(millis() / 1000),
                (unsigned long)(ESP.getFreeHeap() / 1024),
                (unsigned long)(ESP.getFreePsram() / 1024));
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::SystemApps); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::SystemApps); return;
    }
    if (e.type == EventType::Touch || e.type == EventType::TouchHold) {
      haveTouch = true;
      lastTx = e.x; lastTy = e.y;
    }
  }
private:
  bool firstDraw = true;
  bool haveTouch = false;
  uint16_t lastTx = 0, lastTy = 0;

  void drawAxisBar(int16_t x, int16_t y, int16_t w, int16_t h,
                   int16_t v, uint16_t color) {
    gfx->drawRect(x, y, w, h, DARKGREY);
    gfx->fillRect(x + 1, y + 1, w - 2, h - 2, BLACK);
    int16_t mid = x + w / 2;
    int16_t len = (int)v * (w / 2 - 1) / 4096;
    if (len > w / 2 - 1) len = w / 2 - 1;
    if (len < -(w / 2 - 1)) len = -(w / 2 - 1);
    if (len >= 0) gfx->fillRect(mid, y + 1, len, h - 2, color);
    else          gfx->fillRect(mid + len, y + 1, -len, h - 2, color);
    gfx->drawFastVLine(mid, y, h, WHITE);
  }
};

// Settings top-level menu is now driven by CarouselView via a kSettingsEntries
// table at the bottom of this file.

// Mixin-ish helper bag for views that have +/- columns with hold-to-ramp.
// Local to view.cpp; subclassed by both SettingsTimeView and SettingsSleepView.
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

// =====================================================================
// Shared helpers for SettingsTime/Date — leap years, days-in-month, and
// Sakamoto's weekday computation. Defined at file scope so both views
// can reuse them.
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
// SettingsTime — set HH:MM:SS on the RV-3028. Three +/- columns with the
// hold-to-ramp UX. Date fields are preserved by reading them from the
// model and writing them back unchanged.
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
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(110, 14);
      gfx->print("Time");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);
      for (int c = 0; c < 3; c++) {
        int16_t x = COL_X(c);
        gfx->drawRoundRect(x, BTN_PLUS_Y,  COL_W, BTN_H, 6, DARKGREY);
        gfx->drawRoundRect(x, BTN_MINUS_Y, COL_W, BTN_H, 6, DARKGREY);
        gfx->setTextSize(3);
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(x + COL_W / 2 - 9, BTN_PLUS_Y + 12);
        gfx->print('+');
        gfx->setCursor(x + COL_W / 2 - 9, BTN_MINUS_Y + 12);
        gfx->print('-');
      }
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, DARKGREEN);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, DARKGREEN);
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
// SettingsDate — set day / month / year on the RV-3028. Same chrome as
// the time page but the third column shows the last two digits of the
// year (stored full-width). Day clamps when month/year changes (28/29/
// 30/31 boundaries) and the weekday register is recomputed on save.
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
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(110, 14);
      gfx->print("Date");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);

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
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(x + COL_W / 2 - 9, BTN_PLUS_Y + 12);
        gfx->print('+');
        gfx->setCursor(x + COL_W / 2 - 9, BTN_MINUS_Y + 12);
        gfx->print('-');
      }
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, DARKGREEN);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, DARKGREEN);
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
// SettingsSleep — auto-sleep timeout, IMU wake threshold, wake-source toggles.
// Two +/- rows share the same hold-to-ramp; tap any toggle pill flips it.
// =====================================================================
class SettingsSleepView : public View, RampPlusMinus {
public:
  void onEnter() override {
    clearAll();
    { ModelLock lk;
      timeoutSec   = model.sleepTimeoutSec;
      toOffSec     = model.sleepToOffSec;
      imuThreshold = model.imuWakeThreshold;
      wkTouch = model.wakeOnTouch;
      wkBtn   = model.wakeOnButton;
      wkImu   = model.wakeOnImu; }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(100, 14);
      gfx->print("Sleep");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);

      drawRowChrome(ROW0_Y, "Idle -> sleep (s)");
      drawRowChrome(ROW1_Y, "Sleep -> off (s)");
      drawRowChrome(ROW2_Y, "IMU jolt (g)");

      gfx->setTextSize(1);
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(20, TOG_Y - 10);
      gfx->print("Wake on");

      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, DARKGREEN);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, DARKGREEN);
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

    // Hit-test +/- columns. row 0 = idle->sleep, 1 = sleep->off, 2 = IMU jolt.
    int col = -1; int8_t sign = 0;
    int16_t rowY[] = { ROW0_Y, ROW1_Y, ROW2_Y };
    for (int r = 0; r < 3; r++) {
      if      (inRect(e.x, e.y, MINUS_X, rowY[r], BTN_W, BTN_H)) { col = r; sign = -1; break; }
      else if (inRect(e.x, e.y, PLUS_X,  rowY[r], BTN_W, BTN_H)) { col = r; sign = +1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bumpRow(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      // Wake-source toggle row
      for (int i = 0; i < 3; i++) {
        if (inRect(e.x, e.y, TOG_X(i), TOG_Y, TOG_W, TOG_H)) {
          if (i == 0) wkTouch = !wkTouch;
          if (i == 1) wkBtn   = !wkBtn;
          if (i == 2) wkImu   = !wkImu;
          dirty |= TOGGLES; hapticBuzz(40, 50);
          return;
        }
      }
      if (inRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk;
          model.sleepTimeoutSec  = timeoutSec;
          model.sleepToOffSec    = toOffSec;
          model.imuWakeThreshold = imuThreshold;
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

  uint16_t timeoutSec   = 5;
  uint16_t toOffSec     = 30;
  uint8_t  imuThreshold = 0x20;
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
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(MINUS_X + BTN_W / 2 - 9, y + 6);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 9, y + 6);
    gfx->print('+');
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
  // Step IMU threshold in 0x04 (~0.25 g) increments, range 0x08 (0.5g)..0x40 (~4.0g).
  void bumpThreshold(int8_t d) {
    int v = imuThreshold + (d > 0 ? 4 : -4);
    if (v < 0x08) v = 0x40;
    if (v > 0x40) v = 0x08;
    imuThreshold = (uint8_t)v;
    dirty |= VALUE;
  }
  void bumpRow(int col, int8_t d) {
    if (col == 0) bumpTimeout(d);
    if (col == 1) bumpToOff(d);
    if (col == 2) bumpThreshold(d);
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
    if (timeoutSec == 0) snprintf(buf, sizeof(buf), "off");
    else                 snprintf(buf, sizeof(buf), "%us", timeoutSec);
    centerPrint(ROW0_Y, buf);

    if (toOffSec == 0) snprintf(buf, sizeof(buf), "never");
    else               snprintf(buf, sizeof(buf), "%us", toOffSec);
    centerPrint(ROW1_Y, buf);

    float g = imuThreshold * 0.063f;
    snprintf(buf, sizeof(buf), "%.2f", g);
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
// SettingsDisplay — backlight brightness and the watch face's background /
// text colors. Three +/- rows reuse the hold-to-ramp helper. Brightness is
// applied live so the user sees the change while adjusting; colors take
// effect on save (via model.revision bump propagating to the watch face).
// =====================================================================
class SettingsDisplayView : public View, RampPlusMinus {
public:
  void onEnter() override {
    clearAll();
    { ModelLock lk;
      brightness = model.brightness;
      bgIdx = colorIndexOf(model.bgColor);
      fgIdx = colorIndexOf(model.fgColor); }
    dirty = ALL;
    stopHold();
  }
  void render() override {
    if (!gfx) return;
    if (dirty & TITLE) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(94, 14);
      gfx->print("Display");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);
      drawRowChrome(ROW0_Y, "Brightness");
      drawRowChrome(ROW1_Y, "Background");
      drawRowChrome(ROW2_Y, "Text color");
      gfx->fillRoundRect(SAVE_X, ACT_Y, ACT_W, ACT_H, 6, DARKGREEN);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, DARKGREEN);
      gfx->setCursor(SAVE_X + (ACT_W - 48) / 2, ACT_Y + 10);
      gfx->print("SAVE");
      dirty &= ~TITLE;
    }
    if (dirty & VALUE) { drawValues(); dirty &= ~VALUE; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) {
      // If user backs out without saving, restore the saved brightness so the
      // live preview doesn't stick.
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
    int16_t rowY[] = { ROW0_Y, ROW1_Y, ROW2_Y };
    for (int r = 0; r < 3; r++) {
      if (inRect(e.x, e.y, MINUS_X, rowY[r], BTN_W, BTN_H)) { col = r; sign = -1; break; }
      if (inRect(e.x, e.y, PLUS_X,  rowY[r], BTN_W, BTN_H)) { col = r; sign = +1; break; }
    }

    if (e.type == EventType::Touch) {
      if (col >= 0) {
        bumpRow(col, sign); hapticBuzz(40, 50);
        startHold(col, sign);
        return;
      }
      if (inRect(e.x, e.y, SAVE_X, ACT_Y, ACT_W, ACT_H)) {
        { ModelLock lk;
          model.brightness = brightness;
          model.bgColor    = kColors[bgIdx].color;
          model.fgColor    = kColors[fgIdx].color;
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
  static const int16_t ROW0_Y  = 56;
  static const int16_t ROW1_Y  = 112;
  static const int16_t ROW2_Y  = 168;
  static const int16_t BTN_H   = 36;
  static const int16_t BTN_W   = 44;
  static const int16_t MINUS_X = 14;
  static const int16_t PLUS_X  = 182;
  static const int16_t ACT_Y   = 224;
  static const int16_t ACT_H   = 36;
  static const int16_t ACT_W   = 200;
  static const int16_t SAVE_X  = 20;

  enum DirtyFlags { TITLE = 1, VALUE = 2, ALL = 3 };

  uint8_t brightness = 200;
  uint8_t bgIdx = 0;
  uint8_t fgIdx = 1;
  uint8_t dirty = ALL;

  void drawRowChrome(int16_t y, const char *label) {
    gfx->setTextSize(1);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(20, y - 10);
    gfx->print(label);
    gfx->drawRoundRect(MINUS_X, y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->drawRoundRect(PLUS_X,  y, BTN_W, BTN_H, 6, DARKGREY);
    gfx->setTextSize(3);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(MINUS_X + BTN_W / 2 - 9, y + 6);
    gfx->print('-');
    gfx->setCursor(PLUS_X  + BTN_W / 2 - 9, y + 6);
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
    if (row == 1) bumpColor(bgIdx, d);
    if (row == 2) bumpColor(fgIdx, d);
  }
  void restoreBrightness() {
    uint8_t saved;
    { ModelLock lk; saved = model.brightness; }
    backlightSet(saved);
  }
  void drawValues() {
    int16_t valX = MINUS_X + BTN_W;
    int16_t valW = PLUS_X - valX;

    // Brightness as %.
    gfx->fillRect(valX, ROW0_Y, valW, BTN_H, BLACK);
    gfx->setTextSize(3);
    gfx->setTextColor(YELLOW, BLACK);
    char buf[8];
    int pct = (brightness * 100 + 127) / 255;
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int16_t bw = (int16_t)strlen(buf) * 18;
    gfx->setCursor(valX + (valW - bw) / 2, ROW0_Y + 8);
    gfx->print(buf);

    drawColorVal(ROW1_Y, valX, valW, kColors[bgIdx]);
    drawColorVal(ROW2_Y, valX, valW, kColors[fgIdx]);
  }
  void drawColorVal(int16_t y, int16_t valX, int16_t valW, const ColorChoice &c) {
    gfx->fillRect(valX, y, valW, BTN_H, BLACK);
    int16_t sw = 26;
    int16_t sx = valX + 6;
    int16_t sy = y + (BTN_H - sw) / 2;
    gfx->fillRect(sx, sy, sw, sw, c.color);
    gfx->drawRect(sx, sy, sw, sw, WHITE);
    gfx->setTextSize(2);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(sx + sw + 6, y + 10);
    gfx->print(c.name);
  }
};

// =====================================================================
// SettingsMemory — live readout of internal heap, PSRAM, and app-flash use.
// Each row shows USED/TOTAL in KiB, a percentage, and a horizontal fill bar.
// Refreshes a couple of times a second since these numbers move slowly.
// =====================================================================
class SettingsMemoryView : public View {
public:
  void onEnter() override {
    clearAll();
    firstDraw = true;
    lastUpdateMs = 0;
  }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(86, 14);
      gfx->print("Memory");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);
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
    gfx->setTextColor(WHITE, BLACK);
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

// =====================================================================
// Touch Gestures — visualize CST816S gesture stream. Big arrow / label for
// the most recent gesture, small history list under it, and the live touch
// dot on a target area. Swipe right or long-button -> back.
// =====================================================================
class TouchGesturesView : public View {
public:
  void onEnter() override {
    clearAll();
    firstDraw = true;
    histCount = 0;
    lastGesture = Gesture::None;
    lastGestureMs = 0;
    haveDot = false;
    dotX = dotY = 0;
  }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(50, 8);
      gfx->print("Gestures");
      gfx->drawFastHLine(8, 32, W - 16, DARKGREY);
      gfx->drawRoundRect(PAD_X, PAD_Y, PAD_W, PAD_H, 8, DARKGREY);
      firstDraw = false;
    }

    bool fresh = (millis() - lastGestureMs) < 800;
    drawCurrentGesture(fresh);
    drawHistory();
    drawDot();
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::SystemApps); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::SystemApps); return;
    }
    if (e.type == EventType::Gesture) {
      pushGesture(e.gesture);
      hapticBuzz(40, 40);
    }
    if (e.type == EventType::Touch || e.type == EventType::TouchHold) {
      // Track finger inside the pad area for the live dot.
      if (inRect(e.x, e.y, PAD_X, PAD_Y, PAD_W, PAD_H)) {
        dotX = e.x; dotY = e.y; haveDot = true;
      }
    }
    if (e.type == EventType::TouchUp) {
      haveDot = false;
    }
  }
private:
  static const int16_t LABEL_Y = 50;
  static const int16_t HIST_Y  = 100;
  static const int16_t PAD_X = 12, PAD_Y = 138, PAD_W = 216, PAD_H = 110;
  static const int HIST_N = 5;

  bool     firstDraw = true;
  Gesture  lastGesture = Gesture::None;
  uint32_t lastGestureMs = 0;
  Gesture  history[HIST_N];
  int      histCount = 0;
  int      lastDrawnHist = -1;
  Gesture  lastDrawnLabel = (Gesture)0xFE;
  bool     lastDrawnFresh = false;
  bool     haveDot = false;
  int16_t  dotX = 0, dotY = 0;
  int16_t  prevDotX = -1, prevDotY = -1;

  static const char *name(Gesture g) {
    switch (g) {
      case Gesture::None:       return "(none)";
      case Gesture::SwipeDown:  return "SWIPE DOWN";
      case Gesture::SwipeUp:    return "SWIPE UP";
      case Gesture::SwipeLeft:  return "SWIPE LEFT";
      case Gesture::SwipeRight: return "SWIPE RIGHT";
      case Gesture::SingleTap:  return "TAP";
      case Gesture::DoubleTap:  return "DOUBLE TAP";
      case Gesture::LongPress:  return "LONG PRESS";
    }
    return "?";
  }

  void pushGesture(Gesture g) {
    lastGesture = g;
    lastGestureMs = millis();
    // Push onto history (newest first), drop oldest.
    for (int i = HIST_N - 1; i > 0; i--) history[i] = history[i - 1];
    history[0] = g;
    if (histCount < HIST_N) histCount++;
  }

  void drawCurrentGesture(bool fresh) {
    if (lastGesture == lastDrawnLabel && fresh == lastDrawnFresh) return;
    gfx->fillRect(0, LABEL_Y, W, 36, BLACK);
    const char *s = name(lastGesture);
    gfx->setTextSize(3);
    gfx->setTextColor(fresh ? YELLOW : DARKGREY, BLACK);
    gfx->setCursor(centerX(s, 3), LABEL_Y + 4);
    gfx->print(s);
    lastDrawnLabel = lastGesture;
    lastDrawnFresh = fresh;
  }

  void drawHistory() {
    if (histCount == lastDrawnHist) return;
    lastDrawnHist = histCount;
    gfx->fillRect(0, HIST_Y, W, 30, BLACK);
    gfx->setTextSize(1);
    int16_t x = 8;
    for (int i = 0; i < histCount; i++) {
      const char *s = name(history[i]);
      gfx->setTextColor(i == 0 ? WHITE : DARKGREY, BLACK);
      if (x > W - 6) break;
      gfx->setCursor(x, HIST_Y + 4);
      gfx->print(s);
      x += (int16_t)strlen(s) * 6 + 8;
    }
  }

  void drawDot() {
    // Erase previous, draw new.
    if (prevDotX >= 0) gfx->fillCircle(prevDotX, prevDotY, 6, BLACK);
    if (haveDot) {
      // Re-draw pad outline if the previous dot clipped it.
      gfx->drawRoundRect(PAD_X, PAD_Y, PAD_W, PAD_H, 8, DARKGREY);
      gfx->fillCircle(dotX, dotY, 6, MAGENTA);
      prevDotX = dotX; prevDotY = dotY;
    } else {
      prevDotX = -1; prevDotY = -1;
      gfx->drawRoundRect(PAD_X, PAD_Y, PAD_W, PAD_H, 8, DARKGREY);
    }
  }
};

// =====================================================================
// SettingsWifi — top-level WiFi page: power on/off, mode (Host AP vs
// Client), live status, link to the saved-networks list.
// =====================================================================
class SettingsWifiView : public View {
public:
  void onEnter() override {
    clearAll(); firstDraw = true;
    last.en = false; last.mode = WifiMode::AP; last.conn = false;
    last.rssi = 0; last.cli = 0; last.rtcOk = false; last.ssid[0] = '\0';
  }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }
    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(108, 14);
      gfx->print("WiFi");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);
      drawKnownBtn();
      firstDraw = false;
    }
    if (last.en != snap.wifiEnabled)               drawEnableBtn(snap.wifiEnabled);
    if (last.mode != snap.wifiMode || last.en != snap.wifiEnabled)
      drawModeBtns(snap.wifiMode);
    Snap cur{ snap.wifiEnabled, snap.wifiMode, snap.wifiConnected,
              snap.wifiRssi, snap.wifiApClients, snap.rtcOk, "" };
    strncpy(cur.ssid, snap.wifiSsid, 32);
    bool statusChanged =
       last.en != cur.en || last.mode != cur.mode ||
       last.conn != cur.conn || last.rssi != cur.rssi ||
       last.cli  != cur.cli || strncmp(last.ssid, cur.ssid, 32) != 0;
    if (statusChanged) drawStatus(cur);
    last = cur;
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::Settings); return; }

    if (inRect(e.x, e.y, ENA_X, ENA_Y, ENA_W, ENA_H)) {
      { ModelLock lk; model.wifiEnabled = !model.wifiEnabled; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (inRect(e.x, e.y, MODE_AP_X, MODE_Y, MODE_W, MODE_H)) {
      { ModelLock lk; model.wifiMode = WifiMode::AP; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (inRect(e.x, e.y, MODE_CL_X, MODE_Y, MODE_W, MODE_H)) {
      { ModelLock lk; model.wifiMode = WifiMode::Client; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (inRect(e.x, e.y, KN_X, KN_Y, KN_W, KN_H)) {
      switchTo(Screen::SettingsKnownNets); return;
    }
  }
private:
  struct Snap {
    bool     en; WifiMode mode; bool conn;
    int8_t   rssi; uint8_t cli; bool rtcOk;
    char     ssid[33];
  } last{false, WifiMode::AP, false, 0, 0, false, ""};
  bool firstDraw = true;

  static const int16_t ENA_X = 20, ENA_Y = 60,  ENA_W = 200, ENA_H = 40;
  static const int16_t MODE_Y = 116, MODE_W = 96, MODE_H = 40;
  static const int16_t MODE_AP_X = 20;
  static const int16_t MODE_CL_X = 240 - 20 - MODE_W;     // 124
  static const int16_t STAT_Y = 168, STAT_H = 60;
  static const int16_t KN_X = 20, KN_Y = 236, KN_W = 200, KN_H = 36;

  void drawEnableBtn(bool on) {
    uint16_t bg = on ? DARKGREEN : DARKGREY;
    gfx->fillRoundRect(ENA_X, ENA_Y, ENA_W, ENA_H, 8, bg);
    gfx->drawRoundRect(ENA_X, ENA_Y, ENA_W, ENA_H, 8, WHITE);
    gfx->setTextColor(WHITE, bg);
    gfx->setTextSize(2);
    const char *l = on ? "WiFi: ON" : "WiFi: OFF";
    int16_t lw = (int16_t)strlen(l) * 12;
    gfx->setCursor(ENA_X + (ENA_W - lw) / 2, ENA_Y + 12);
    gfx->print(l);
  }
  void drawModeBtn(int16_t x, const char *label, bool active) {
    uint16_t bg = active ? PURPLE : DARKGREY;
    gfx->fillRoundRect(x, MODE_Y, MODE_W, MODE_H, 8, bg);
    gfx->drawRoundRect(x, MODE_Y, MODE_W, MODE_H, 8, WHITE);
    gfx->setTextColor(WHITE, bg);
    gfx->setTextSize(2);
    int16_t lw = (int16_t)strlen(label) * 12;
    gfx->setCursor(x + (MODE_W - lw) / 2, MODE_Y + 12);
    gfx->print(label);
  }
  void drawModeBtns(WifiMode m) {
    drawModeBtn(MODE_AP_X, "Host",   m == WifiMode::AP);
    drawModeBtn(MODE_CL_X, "Client", m == WifiMode::Client);
  }
  void drawStatus(const Snap &s) {
    gfx->fillRect(0, STAT_Y, W, STAT_H, BLACK);
    gfx->setTextSize(1);
    if (!s.en) {
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(20, STAT_Y + 12); gfx->print("Radio off");
      return;
    }
    char buf[80];
    if (s.mode == WifiMode::AP) {
      gfx->setTextColor(YELLOW, BLACK);
      gfx->setCursor(12, STAT_Y);
      snprintf(buf, sizeof(buf), "AP: %s", s.ssid[0] ? s.ssid : "EWATCH_SETUP");
      gfx->print(buf);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(12, STAT_Y + 14);
      gfx->print("http://192.168.4.1/");
      gfx->setCursor(12, STAT_Y + 28);
      snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)s.cli);
      gfx->print(buf);
    } else {
      if (s.conn) {
        gfx->setTextColor(GREEN, BLACK);
        gfx->setCursor(12, STAT_Y);
        snprintf(buf, sizeof(buf), "Connected: %s", s.ssid);
        gfx->print(buf);
        gfx->setTextColor(WHITE, BLACK);
        gfx->setCursor(12, STAT_Y + 14);
        snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)s.rssi);
        gfx->print(buf);
      } else {
        gfx->setTextColor(ORANGE, BLACK);
        gfx->setCursor(12, STAT_Y); gfx->print("Scanning for saved SSIDs...");
        if (Storage::knownCount() == 0) {
          gfx->setTextColor(0x8410, BLACK);
          gfx->setCursor(12, STAT_Y + 16);
          gfx->print("(no networks saved yet)");
        }
      }
    }
  }
  void drawKnownBtn() {
    gfx->fillRoundRect(KN_X, KN_Y, KN_W, KN_H, 6, NAVY);
    gfx->drawRoundRect(KN_X, KN_Y, KN_W, KN_H, 6, WHITE);
    gfx->setTextColor(WHITE, NAVY);
    gfx->setTextSize(2);
    const char *l = "Saved networks >";
    int16_t lw = (int16_t)strlen(l) * 12;
    gfx->setCursor(KN_X + (KN_W - lw) / 2, KN_Y + 10);
    gfx->print(l);
  }
};

// =====================================================================
// SettingsKnownNets — list of saved SSIDs with per-row delete. Adding new
// networks happens via the AP web form (typing passwords on the touchscreen
// would be miserable).
// =====================================================================
class SettingsKnownNetsView : public View {
public:
  void onEnter() override { clearAll(); firstDraw = true; lastN = 0xFF; }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(74, 14);
      gfx->print("Networks");
      gfx->drawFastHLine(20, 46, 200, DARKGREY);
      firstDraw = false;
    }
    uint8_t n = Storage::knownCount();
    if (n != lastN) { drawList(); lastN = n; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::SettingsWifi); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::SettingsWifi); return; }

    uint8_t n = Storage::knownCount();
    for (uint8_t i = 0; i < n; i++) {
      int16_t y = ROW_Y0 + i * ROW_H;
      if (inRect(e.x, e.y, DEL_X, y, DEL_W, ROW_H - 4)) {
        Storage::knownRemove(i);
        Storage::knownSave();
        hapticBuzz(80, 80);
        lastN = 0xFF;        // force list redraw
        return;
      }
    }
  }
private:
  bool   firstDraw = true;
  uint8_t lastN = 0xFF;
  static const int16_t ROW_Y0 = 56;
  static const int16_t ROW_H  = 26;
  static const int16_t DEL_X  = 188;
  static const int16_t DEL_W  = 44;

  void drawList() {
    gfx->fillRect(0, ROW_Y0 - 2, W, H - ROW_Y0 - 2, BLACK);
    uint8_t n = Storage::knownCount();
    if (n == 0) {
      gfx->setTextSize(2);
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(28, 110); gfx->print("(none saved)");
      gfx->setTextSize(1);
      gfx->setTextColor(0x8410, BLACK);
      gfx->setCursor(12, 200); gfx->print("Add via Web Setup AP:");
      gfx->setCursor(12, 214); gfx->print("Settings -> WiFi -> Host,");
      gfx->setCursor(12, 228); gfx->print("join EWATCH_SETUP, open page.");
      return;
    }
    for (uint8_t i = 0; i < n && i < Storage::KNOWN_MAX; i++) {
      char s[Storage::KNOWN_SSID], p[Storage::KNOWN_PASS];
      if (!Storage::knownAt(i, s, p)) continue;
      int16_t y = ROW_Y0 + i * ROW_H;
      gfx->setTextSize(1);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(12, y + 7);
      char trunc[29];
      strncpy(trunc, s, 28); trunc[28] = '\0';
      gfx->print(trunc);
      gfx->fillRoundRect(DEL_X, y, DEL_W, ROW_H - 4, 4, MAROON);
      gfx->drawRoundRect(DEL_X, y, DEL_W, ROW_H - 4, 4, WHITE);
      gfx->setTextColor(WHITE, MAROON);
      gfx->setTextSize(2);
      gfx->setCursor(DEL_X + (DEL_W - 12) / 2, y + 5);
      gfx->print('X');
    }
  }
};

// =====================================================================
// IMU Gestures — visualizes orientation + recent shake events.
// Live tilt dot driven by accel X/Y; classifies face-up/face-down/portrait/
// landscape from accel sign. Counts shakes from the controller's heuristic.
// =====================================================================
class ImuGesturesView : public View {
public:
  void onEnter() override {
    clearAll();
    firstDraw = true;
    motionCount = 0;
    lastMotionMs = 0;
    lastDrawnOrient = -1;
    lastDotX = lastDotY = -1;
  }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    if (firstDraw) {
      drawBackButton();
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(70, 8);
      gfx->print("IMU");
      gfx->drawFastHLine(8, 32, W - 16, DARKGREY);
      gfx->drawCircle(BALL_CX, BALL_CY, BALL_R, DARKGREY);
      gfx->drawCircle(BALL_CX, BALL_CY, 2, DARKGREY);
      firstDraw = false;
    }

    // Orientation classification from accel direction.
    int o = classifyOrientation(snap);
    if (o != lastDrawnOrient) {
      gfx->fillRect(0, 42, W, 22, BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(CYAN, BLACK);
      const char *s = orientName(o);
      gfx->setCursor(centerX(s, 2), 42);
      gfx->print(s);
      lastDrawnOrient = o;
    }

    // Tilt ball: map accel X/Y to a position inside the circle. The MMA8451
    // is mounted such that +ax means tilting right (board frame), so we
    // negate to get screen-frame motion that matches the user's expectation.
    int16_t bx = BALL_CX - clamp((int)snap.ax * BALL_R / 4096, -BALL_R, BALL_R);
    int16_t by = BALL_CY - clamp((int)snap.ay * BALL_R / 4096, -BALL_R, BALL_R);
    if (bx != lastDotX || by != lastDotY) {
      if (lastDotX >= 0) gfx->fillCircle(lastDotX, lastDotY, 7, BLACK);
      // Re-stroke the well in case the previous dot clipped it.
      gfx->drawCircle(BALL_CX, BALL_CY, BALL_R, DARKGREY);
      gfx->drawCircle(BALL_CX, BALL_CY, 2, DARKGREY);
      gfx->fillCircle(bx, by, 7, ORANGE);
      lastDotX = bx; lastDotY = by;
    }

    // Motion row.
    bool fresh = (millis() - lastMotionMs) < 600;
    gfx->fillRect(0, SHAKE_Y, W, 20, BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(fresh ? YELLOW : DARKGREY, BLACK);
    gfx->setCursor(8, SHAKE_Y);
    gfx->printf("motion: %lu", (unsigned long)motionCount);
    if (fresh) {
      gfx->setTextColor(YELLOW, BLACK);
      gfx->setCursor(150, SHAKE_Y);
      gfx->print("JOLT!");
    }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::SystemApps); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::SystemApps); return;
    }
    if (e.type == EventType::ImuMotion) {
      motionCount++;
      lastMotionMs = millis();
      hapticBuzz(120, 80);
    }
  }
private:
  static const int16_t BALL_CX = 120, BALL_CY = 150;
  static const int16_t BALL_R  = 70;
  static const int16_t SHAKE_Y = 232;

  bool     firstDraw = true;
  uint32_t motionCount = 0;
  uint32_t lastMotionMs = 0;
  int      lastDrawnOrient = -1;
  int16_t  lastDotX = -1, lastDotY = -1;

  static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

  // 0=face up, 1=face down, 2=portrait up, 3=portrait down,
  // 4=landscape left, 5=landscape right. Invert ax to match the screen frame.
  static int classifyOrientation(const Model &m) {
    int16_t ax = -m.ax, ay = m.ay, az = m.az;
    int absx = abs(ax), absy = abs(ay), absz = abs(az);
    if (absz >= absx && absz >= absy) return az > 0 ? 0 : 1;
    if (absy >= absx)                  return ay > 0 ? 2 : 3;
    return ax > 0 ? 4 : 5;
  }
  static const char *orientName(int o) {
    switch (o) {
      case 0: return "face up";
      case 1: return "face down";
      case 2: return "portrait";
      case 3: return "upside down";
      case 4: return "landscape L";
      case 5: return "landscape R";
    }
    return "?";
  }
};

// =====================================================================
// Power Off — three actions. CANCEL goes back, SLEEP enters deep sleep
// with configured wake sources, OFF cuts the LDO latch entirely. The two
// "destructive" actions require a 800 ms hold to confirm.
// =====================================================================
class PowerOffView : public View {
public:
  void onEnter() override {
    clearAll();
    if (!gfx) return;
    drawBackButton();
    gfx->setTextSize(3);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(60, 60);
    gfx->print("Power");

    drawButton(SLEEP_Y,   NAVY,    "Sleep");
    drawButton(OFF_Y,     MAROON,  "Off");

    armed = ARM_NONE;
    armedAt = 0;
    fired = false;
  }
  void render() override {
    if (!gfx) return;
    // Repaint armed-button progress fill so user sees they're holding it.
    if (armed != ARM_NONE && !fired) {
      uint32_t held = millis() - armedAt;
      if (held > 800) held = 800;
      int16_t y = (armed == ARM_SLEEP) ? SLEEP_Y : OFF_Y;
      uint16_t base = (armed == ARM_SLEEP) ? NAVY : MAROON;
      int16_t fill = (BTN_W - 4) * (int)held / 800;
      gfx->fillRect(BTN_X + 2, y + BTN_H - 6, fill, 4, WHITE);
      gfx->fillRect(BTN_X + 2 + fill, y + BTN_H - 6, BTN_W - 4 - fill, 4, base);
      if (held >= 800) doAction();
    }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) {
      switchTo(Screen::SystemApps); return;
    }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::SystemApps); return;
    }

    if (e.type == EventType::Touch) {
      if (inRect(e.x, e.y, BTN_X, SLEEP_Y, BTN_W, BTN_H)) {
        armed = ARM_SLEEP; armedAt = millis(); fired = false;
        return;
      }
      if (inRect(e.x, e.y, BTN_X, OFF_Y, BTN_W, BTN_H)) {
        armed = ARM_OFF;   armedAt = millis(); fired = false;
        return;
      }
      return;
    }

    // Tolerate brief drift inside the button's hit zone — only TouchUp
    // cancels. (Previous version cancelled on any TouchHold drift, which
    // made the hold flaky.)
    if (e.type == EventType::TouchUp) {
      if (armed != ARM_NONE && !fired) {
        // erase progress strip on the (cancelled) button.
        int16_t y = (armed == ARM_SLEEP) ? SLEEP_Y : OFF_Y;
        uint16_t base = (armed == ARM_SLEEP) ? NAVY : MAROON;
        gfx->fillRect(BTN_X + 2, y + BTN_H - 6, BTN_W - 4, 4, base);
      }
      armed = ARM_NONE;
    }
  }
private:
  static const int16_t BTN_X    = 32;
  static const int16_t BTN_W    = 176;
  static const int16_t BTN_H    = 64;
  static const int16_t SLEEP_Y  = 110;
  static const int16_t OFF_Y    = 184;

  enum ArmedAction : uint8_t { ARM_NONE, ARM_SLEEP, ARM_OFF };
  ArmedAction armed = ARM_NONE;
  uint32_t    armedAt = 0;
  bool        fired = false;

  void drawButton(int16_t y, uint16_t color, const char *label) {
    gfx->fillRoundRect(BTN_X, y, BTN_W, BTN_H, 8, color);
    gfx->drawRoundRect(BTN_X, y, BTN_W, BTN_H, 8, WHITE);
    gfx->setTextColor(WHITE, color);
    gfx->setTextSize(3);
    gfx->setCursor(BTN_X + (BTN_W - (int16_t)strlen(label) * 18) / 2, y + 16);
    gfx->print(label);
  }

  void doAction() {
    fired = true;
    if (armed == ARM_SLEEP) goSleep();
    else                    goOff();
  }

  void goOff() {
    if (gfx) {
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(80, 130);
      gfx->print("bye");
    }
    backlightOff();
    hapticBuzz(180, 200);
    vTaskDelay(pdMS_TO_TICKS(80));
    unlatchPower();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  void goSleep() {
    if (gfx) {
      gfx->fillScreen(BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(60, 130);
      gfx->print("sleeping");
    }
    hapticBuzz(120, 80);
    vTaskDelay(pdMS_TO_TICKS(120));
    backlightOff();
    enterDeepSleep();         // does not return
  }
};

// ---------- view registry / dispatch ----------
// Top level: just the user-facing 3D viewer plus an entry into the System
// sub-page (settings, diagnostics, power off).
static const AppEntry kTopApps[] = {
  { "3D Viewer", DARKCYAN, Screen::Viewer3D,   "rotate a 3D model"    },
  { "Media",     OLIVE,    Screen::Media,      "images and videos"    },
  { "QR Share",  PURPLE,   Screen::QRCode,     "share contact info"   },
  { "Stopwatch", DARKCYAN, Screen::Stopwatch,  "count-up timer"       },
  { "Timer",     MAROON,   Screen::Timer,      "countdown + alarm"    },
  { "Animator",  OLIVE,    Screen::AnimDemo,   "framework demo"       },
  { "System",    NAVY,     Screen::SystemApps, "settings & sensors"   },
};
static const AppEntry kSystemApps[] = {
  { "Settings",       NAVY,      Screen::Settings,      "preferences"          },
  { "Sensor Test",    DARKGREEN, Screen::SensorTest,    "live readouts"        },
  { "Touch Gestures", PURPLE,    Screen::TouchGestures, "test the touch chip"  },
  { "IMU Gestures",   OLIVE,     Screen::ImuGestures,   "test accelerometer"   },
  { "Power Off",      MAROON,    Screen::PowerOff,      "sleep or shut down"   },
};
static const AppEntry kSettingsEntries[] = {
  { "Time",    NAVY,      Screen::SettingsTime,    "set the clock"        },
  { "Date",    DARKCYAN,  Screen::SettingsDate,    "set the calendar"     },
  { "Sleep",   DARKGREEN, Screen::SettingsSleep,   "auto-sleep + wake"    },
  { "Display", PURPLE,    Screen::SettingsDisplay, "brightness + colors"  },
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  { "WiFi",    DARKCYAN,  Screen::SettingsWifi,    "host AP / join WiFi"  },
#endif
  { "Memory",  MAROON,    Screen::SettingsMemory,  "heap / PSRAM usage"   },
};

static WatchFaceView      vWatch;
static CarouselView       vAppList(kTopApps,
                                   sizeof(kTopApps) / sizeof(kTopApps[0]),
                                   "Apps", Screen::Watch, /*wrap=*/true);
static CarouselView       vSystemApps(kSystemApps,
                                      sizeof(kSystemApps) / sizeof(kSystemApps[0]),
                                      "System", Screen::AppList);
static CarouselView       vSettings(kSettingsEntries,
                                    sizeof(kSettingsEntries) / sizeof(kSettingsEntries[0]),
                                    "Settings", Screen::SystemApps);
static SettingsTimeView     vSettingsTime;       // page
static SettingsDateView     vSettingsDate;       // page
static SettingsSleepView    vSettingsSleep;      // page
static SettingsDisplayView  vSettingsDisplay;    // page
static SettingsMemoryView   vSettingsMemory;     // page
static SettingsWifiView     vSettingsWifi;       // page
static SettingsKnownNetsView vSettingsKnownNets; // page
static SensorTestView     vSensorTest;
static TouchGesturesView  vTouchGestures;
static ImuGesturesView    vImuGestures;
static Viewer3DView       vViewer3D;       // user app — defined in apps/viewer3d.cpp
static MediaView          vMedia;          // user app — defined in apps/media.cpp
static QRCodeView         vQRCode;         // user app — defined in apps/qr.cpp
static AnimDemoView       vAnimDemo;       // user app — defined in apps/anim_demo.cpp
static StopwatchView      vStopwatch;      // user app — defined in apps/stopwatch.cpp
static TimerView          vTimer;          // user app — defined in apps/timer_app.cpp
static PowerOffView       vPowerOff;

View *currentView = nullptr;

View *viewFor(Screen s) {
  switch (s) {
    case Screen::Watch:         return &vWatch;
    case Screen::AppList:       return &vAppList;
    case Screen::SystemApps:    return &vSystemApps;
    case Screen::Settings:      return &vSettings;
    case Screen::SettingsTime:    return &vSettingsTime;
    case Screen::SettingsDate:    return &vSettingsDate;
    case Screen::SettingsSleep:   return &vSettingsSleep;
    case Screen::SettingsDisplay: return &vSettingsDisplay;
    case Screen::SettingsMemory:  return &vSettingsMemory;
    case Screen::SettingsWifi:    return &vSettingsWifi;
    case Screen::SettingsKnownNets: return &vSettingsKnownNets;
    case Screen::SensorTest:    return &vSensorTest;
    case Screen::TouchGestures: return &vTouchGestures;
    case Screen::ImuGestures:   return &vImuGestures;
    case Screen::Viewer3D:      return &vViewer3D;
    case Screen::Media:         return &vMedia;
    case Screen::QRCode:        return &vQRCode;
    case Screen::AnimDemo:      return &vAnimDemo;
    case Screen::Stopwatch:     return &vStopwatch;
    case Screen::Timer:         return &vTimer;
    case Screen::PowerOff:      return &vPowerOff;
  }
  return &vWatch;
}

void switchTo(Screen s) {
  View *next = viewFor(s);
  Serial.printf("[NAV %lu] switchTo(%d) cur=%p (vt=%p) next=%p (vt=%p)\n",
                (unsigned long)millis(), (int)s,
                (void*)currentView, currentView ? *(void**)currentView : nullptr,
                (void*)next,        next        ? *(void**)next        : nullptr);
  if (currentView == next) return;
  if (currentView) currentView->onExit();
  { ModelLock lk; model.screen = s; model.revision++; }
  currentView = next;
  currentView->onEnter();
}

void viewsInit() {
  currentView = &vWatch;
  if (gfx) gfx->fillScreen(BLACK);
  currentView->onEnter();
}
