// AuroraOS watch face — the home screen.
//
// Five faces, cycled with left/right swipes (persisted as model.watchFaceStyle):
//   0 Aurora  — big rounded-segment digits, date pill, battery ring, live
//               chips for stopwatch / timer / WiFi. The flagship.
//   1 Analog  — minimal dial: tick ring, three hands, tiny date.
//   2 Orbit   — hour/minute/second as planets orbiting a center star.
//   3 SM Logo — ScrubMarine watermark + bold HH:MM (the classic).
//   4 Mono    — terminal vibes, FreeMono, dim green.
//
// Navigation from here (the hub of the whole UI):
//   swipe up    -> app launcher       (sheet rises)
//   swipe down  -> control centre     (sheet drops)
//   swipe left  -> next face          swipe right -> previous face
//   button      -> app launcher
//
// Event-driven by default (the I/O task bumps model.revision each RTC tick);
// faces with sweep motion ask for frames via desiredFrameMs().
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "FreeSans12pt7b.h"
#include "FreeMono12pt7b.h"
#include "FreeMono24pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "pins.h"
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "ui_icons.h"
#include "ui_motion.h"
#include "controller.h"
#include "storage.h"
#include "apptimer.h"
#include "wifi_svc.h"
#include "system_views.h"
#include "smicon_bg.h"
#include "photo_store.h"

using namespace aura;

// ---------------------------------------------------------------------------
// Style table (public API kept for the web settings form + Settings page).
// ---------------------------------------------------------------------------
struct FaceStyle { const char *name; const GFXfont *uiFont; };
static const FaceStyle kFaces[] = {
  { "Aurora",  &FreeSans12pt7b },
  { "Analog",  &FreeSans12pt7b },
  { "Orbit",   &FreeSans12pt7b },
  { "Photo",   &FreeSans12pt7b },
  { "Mono",    &FreeMono12pt7b },
};
static const int kNumFaces = sizeof(kFaces) / sizeof(kFaces[0]);

int watchFaceStyleCount() { return kNumFaces; }
const char *watchFaceStyleName(int idx) {
  return (idx >= 0 && idx < kNumFaces) ? kFaces[idx].name : "?";
}
const GFXfont *currentUiFont() {
  uint8_t s;
  { ModelLock lk; s = model.watchFaceStyle; }
  if (s >= kNumFaces) s = 0;
  return kFaces[s].uiFont;
}

static uint8_t currentFace() {
  uint8_t s;
  { ModelLock lk; s = model.watchFaceStyle; }
  return (s >= kNumFaces) ? 0 : s;
}

// ---------------------------------------------------------------------------
// Rounded-segment digit renderer (Aurora face). Each digit is drawn from the
// classic 7 segments, but every segment is a rounded capsule — reads modern,
// not calculator. Scales cleanly: w ~= s, h ~= 2s.
// ---------------------------------------------------------------------------
static const uint8_t kSegForDigit[10] = {
  //  abcdefg  (bit6=a top ... bit0=g middle)
  0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011,
  0b1011011, 0b1011111, 0b1110000, 0b1111111, 0b1111011,
};

static void capsule(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t c) {
  int16_t r = (w < h ? w : h) / 2;
  d->fillRoundRect(x, y, w, h, r, c);
}

// Draw one digit with top-left (x,y), segment size s (digit is s x 2s), thickness t.
static void auroraDigit(Arduino_GFX *d, int digit, int16_t x, int16_t y,
                        int16_t s, int16_t t, uint16_t on, uint16_t off) {
  uint8_t m = (digit >= 0 && digit <= 9) ? kSegForDigit[digit] : 0;
  int16_t g = t / 2 + 1;                    // corner inset so segments don't overlap
  // a top, d bottom, g middle (horizontal)
  struct { int16_t x, y, w, h; uint8_t bit; } segs[7] = {
    { (int16_t)(x + g),      (int16_t)y,               (int16_t)(s - 2 * g), t, 6 }, // a
    { (int16_t)(x + s - t),  (int16_t)(y + g),          t, (int16_t)(s - 2 * g), 5 }, // b
    { (int16_t)(x + s - t),  (int16_t)(y + s + g),      t, (int16_t)(s - 2 * g), 4 }, // c
    { (int16_t)(x + g),      (int16_t)(y + 2 * s - t),  (int16_t)(s - 2 * g), t, 3 }, // d
    { (int16_t)x,            (int16_t)(y + s + g),      t, (int16_t)(s - 2 * g), 2 }, // e
    { (int16_t)x,            (int16_t)(y + g),          t, (int16_t)(s - 2 * g), 1 }, // f
    { (int16_t)(x + g),      (int16_t)(y + s - t / 2),  (int16_t)(s - 2 * g), t, 0 }, // g
  };
  for (auto &sg : segs) {
    uint16_t c = (m >> sg.bit) & 1 ? on : off;
    if (c != off || off != 0) capsule(d, sg.x, sg.y, sg.w, sg.h, c);
    else if ((m >> sg.bit) & 1) capsule(d, sg.x, sg.y, sg.w, sg.h, on);
  }
}

// ---------------------------------------------------------------------------
// WatchFaceView
// ---------------------------------------------------------------------------
class WatchFaceView : public View {
public:
  void onEnter() override {
    canvas = frameCanvas();
    facePop.jump(0);
    forceFull = true;
  }

  uint16_t desiredFrameMs() const override {
    uint8_t f = currentFace();
    if (f == 1 || f == 2) return 33;                    // sweep hands / orbits
    if (stopwatchRunning() || timerArmed()) return 100; // live chips tick
    return 0;                                           // event-driven
  }

  bool paintTo(Arduino_Canvas *cv) override {
    if (!cv) return false;
    canvas = cv;
    drawFace();
    return true;
  }

  void render() override {
    if (!canvas) { legacyRender(); return; }
    drawFace();
    canvas->flush();
  }

  void onEvent(const Event &e) override {
    switch (e.type) {
      case EventType::Touch:
        downX = e.x; downY = e.y;
        return;
      case EventType::TouchUp: {
        // A tap (not a swipe) on the bottom-right corner toggles WiFi.
        int16_t dx = (int16_t)e.x - (int16_t)downX;
        int16_t dy = (int16_t)e.y - (int16_t)downY;
        if ((int32_t)dx * dx + (int32_t)dy * dy <= 14 * 14 &&
            inWifiCorner(e.x, e.y)) {
          bool on;
          { ModelLock lk; model.wifiEnabled = !model.wifiEnabled;
            on = model.wifiEnabled; model.revision++; }
          Storage::save();
          if (on) wifiSvcKickWindow();   // connect now, skip scheduler backoff
          hapticBuzz(on ? 40 : 24, on ? 45 : 28);
        }
        return;
      }
      case EventType::Gesture:
        switch (e.gesture) {
          case Gesture::SwipeUp:
            hapticBuzz(20, 22);
            switchToAnimated(Screen::AppList, Trans::SheetUp);
            return;
          case Gesture::SwipeDown:
            hapticBuzz(20, 22);
            switchToAnimated(Screen::QuickSettings, Trans::SheetDown);
            return;
          case Gesture::SwipeLeft:  cycleFace(+1); return;
          case Gesture::SwipeRight: cycleFace(-1); return;
          default: return;
        }
      case EventType::ButtonShort:
        hapticBuzz(20, 22);
        switchToAnimated(Screen::AppList, Trans::SheetUp);
        return;
      default:
        return;
    }
  }

private:
  Arduino_Canvas *canvas = nullptr;
  Tween facePop;             // scale-in flourish when the face changes
  bool  forceFull = true;
  uint8_t  secSeen = 255;    // last RTC second observed (sweep anchor)
  uint32_t secAnchorMs = 0;
  float    secFrac = 0;      // 0..1 within the current RTC second
  uint16_t downX = 0, downY = 0;   // tap-vs-swipe for the corner toggle

  void cycleFace(int dir) {
    uint8_t f = currentFace();
    f = (uint8_t)((f + kNumFaces + dir) % kNumFaces);
    { ModelLock lk; model.watchFaceStyle = f; model.revision++; }
    Storage::save();
    hapticBuzz(24, 30);
    facePop.start(1.0f, 0.0f, 320, easeOutCubic);
  }

  struct Snap {
    uint8_t h, m, s, wd, day, mon;
    uint16_t year;
    bool rtcOk;
    bool wifiOn;      // user's WiFi enable (model)
    bool wifiUp;      // radio actually associated / AP up right now
  };
  Snap grab() {
    Snap sn;
    { ModelLock lk;
      sn.h = model.hour; sn.m = model.minute; sn.s = model.second;
      sn.wd = model.weekday; sn.day = model.day; sn.mon = model.month;
      sn.year = model.year; sn.rtcOk = model.rtcOk;
      sn.wifiOn = model.wifiEnabled; }
    sn.wifiUp = wifiSvcRadioActive();
    return sn;
  }

  void drawFace() {
    AuraTheme th = auraTheme();
    canvas->fillScreen(th.bg);
    Snap sn = grab();
    // Sub-second sweep fraction, anchored to the observed RTC second change.
    // A free-running millis()%1000 wraps mid-second — the hand visibly
    // jumped BACK one second, then the RTC tick snapped it forward again.
    if (sn.s != secSeen) { secSeen = sn.s; secAnchorMs = millis(); }
    secFrac = (millis() - secAnchorMs) / 1000.0f;
    if (secFrac > 0.999f) secFrac = 0.999f;
    switch (currentFace()) {
      case 0: faceAurora(th, sn); break;
      case 1: faceAnalog(th, sn); break;
      case 2: faceOrbit(th, sn);  break;
      case 3: faceLogo(th, sn);   break;
      default: faceMono(th, sn);  break;
    }
    // Face-switch flourish: a quick dim veil that lifts (cheap, classy).
    float pop = facePop.value();
    if (pop > 0.02f) {
      int16_t inset = (int16_t)(pop * 26.0f);
      canvas->drawRoundRect(inset, inset, W - 2 * inset, H - 2 * inset,
                            kRadius, th.line);
    }
  }

  static const char *kWd[7];
  static const char *kMon[12];

  // Bottom-right WiFi state + toggle target (all faces). accent = radio up,
  // dim = enabled but idle (duty-cycled), slashed line-color = off.
  void drawWifiToggle(const AuraTheme &th, const Snap &sn) {
    const int16_t cx = W - 36, cy = H - 34;
    uint16_t c = sn.wifiUp ? th.accent : (sn.wifiOn ? th.textDim : th.line);
    drawIcon(canvas, Icon::Wifi, cx, cy, 26, c, th.bg);
    if (!sn.wifiOn) {
      canvas->drawLine(cx - 12, cy + 12, cx + 12, cy - 12, th.line);
      canvas->drawLine(cx - 11, cy + 12, cx + 13, cy - 12, th.line);
    }
  }
  static bool inWifiCorner(uint16_t x, uint16_t y) {
    return x >= W - 72 && y >= H - 72;
  }

  void dateLine(char *buf, size_t n, const Snap &sn) {
    // month is 1-based; guard 0 (pre-sync) and >12 (corrupt RTC) — a bare
    // (mon-1)%12 goes negative at mon==0 and indexes out of bounds.
    if (!sn.rtcOk || sn.mon < 1 || sn.mon > 12) { snprintf(buf, n, "--- -- ---"); return; }
    snprintf(buf, n, "%s %u %s", kWd[sn.wd % 7], sn.day, kMon[sn.mon - 1]);
  }

  // ---- face 0: Aurora ----
  void faceAurora(const AuraTheme &th, const Snap &sn) {
    // Time, HH:MM in rounded segments, hero-sized.
    const int16_t ds = 42, dt = 11;                 // digit box 42x84
    const int16_t gap = 10, colonW = 12;
    // Four gaps: d1|g|d2|g|colon|g|d3|g|d4 (was 3 — face sat ~5px right).
    const int16_t total = 4 * ds + 4 * gap + colonW;
    int16_t x = (W - total) / 2, y = 58;
    uint16_t off = 0;                                // no ghost segments
    int hh = sn.rtcOk ? sn.h : 0, mm = sn.rtcOk ? sn.m : 0;
    auroraDigit(canvas, hh / 10, x, y, ds, dt, th.text, off);   x += ds + gap;
    auroraDigit(canvas, hh % 10, x, y, ds, dt, th.text, off);   x += ds + gap;
    // Colon: two dots, blinking on even seconds (subtle life without a :SS row).
    if (!sn.rtcOk || (sn.s & 1) == 0) {
      canvas->fillCircle(x + colonW / 2, y + ds - 14, 5, th.accent);
      canvas->fillCircle(x + colonW / 2, y + ds + 14, 5, th.accent);
    }
    x += colonW + gap;
    auroraDigit(canvas, mm / 10, x, y, ds, dt, th.text, off);   x += ds + gap;
    auroraDigit(canvas, mm % 10, x, y, ds, dt, th.text, off);

    // Date pill under the time.
    char dbuf[20];
    dateLine(dbuf, sizeof dbuf, sn);
    int16_t pw = auraTextWidth(dbuf, 2) + 28;
    auraPill(canvas, (W - pw) / 2, 170, pw, 34, th.card, th.text, dbuf, 2);

    drawWifiToggle(th, sn);

    // Live chips: stopwatch / timer (only while active) centered at bottom.
    uint32_t epoch = sn.rtcOk
        ? rtcEpochSec(sn.year, sn.mon, sn.day, sn.h, sn.m, sn.s) : 0;
    int16_t cy = 218;
    if (stopwatchRunning()) {
      uint32_t ms = stopwatchElapsedMs(epoch, millis());
      char c[16];
      snprintf(c, sizeof c, "%lu:%02lu.%lu", (unsigned long)(ms / 60000),
               (unsigned long)((ms / 1000) % 60), (unsigned long)((ms / 100) % 10));
      auraChip(canvas, W / 2 - 60, cy, kGreen, c, th.textDim);
    }
    if (timerArmed()) {
      uint32_t rs = timerRemainingSec(epoch);
      char c[16];
      snprintf(c, sizeof c, "%lu:%02lu", (unsigned long)(rs / 60),
               (unsigned long)(rs % 60));
      auraChip(canvas, W / 2 + 14, cy, kOrange, c, th.textDim);
    }
  }

  // ---- face 1: Analog ----
  void faceAnalog(const AuraTheme &th, const Snap &sn) {
    const int16_t cx = W / 2, cy = H / 2, R = 108;
    for (int i = 0; i < 60; i++) {                       // tick ring
      float a = (float)i * (float)M_PI / 30.0f;
      bool major = (i % 5) == 0;
      int16_t r0 = R - (major ? 12 : 6);
      int16_t x0 = cx + (int16_t)(sinf(a) * r0), y0 = cy - (int16_t)(cosf(a) * r0);
      int16_t x1 = cx + (int16_t)(sinf(a) * R),  y1 = cy - (int16_t)(cosf(a) * R);
      canvas->drawLine(x0, y0, x1, y1, major ? th.text : th.line);
    }
    if (!sn.rtcOk) {
      auraTextCentered(canvas, cx, cy - 4, "syncing...", 1, th.textDim);
      return;
    }
    // Smooth hands (ms-interpolated second sweep).
    float sf = sn.s + secFrac;
    float mf = sn.m + sf / 60.0f;
    float hf = (sn.h % 12) + mf / 60.0f;
    auto hand = [&](float frac, int16_t len, int16_t t, uint16_t c) {
      float a = frac * 2.0f * (float)M_PI;
      int16_t x1 = cx + (int16_t)(sinf(a) * len);
      int16_t y1 = cy - (int16_t)(cosf(a) * len);
      for (int i = -(t / 2); i <= t / 2; i++)
        canvas->drawLine(cx + i, cy, x1 + i, y1, c);
    };
    hand(hf / 12.0f, 56, 5, th.text);
    hand(mf / 60.0f, 84, 3, th.text);
    hand(sf / 60.0f, 96, 1, th.accent);
    canvas->fillCircle(cx, cy, 5, th.accent);
    canvas->fillCircle(cx, cy, 2, th.bg);

    char dbuf[20];
    dateLine(dbuf, sizeof dbuf, sn);
    auraTextCentered(canvas, cx, cy + 46, dbuf, 1, th.textDim);
    drawWifiToggle(th, sn);
  }

  // ---- face 2: Orbit ----
  void faceOrbit(const AuraTheme &th, const Snap &sn) {
    const int16_t cx = W / 2, cy = H / 2;
    const int16_t rH = 42, rM = 72, rS = 100;
    canvas->drawCircle(cx, cy, rH, th.line);
    canvas->drawCircle(cx, cy, rM, th.line);
    canvas->drawCircle(cx, cy, rS, th.line);
    canvas->fillCircle(cx, cy, 8, th.accent);            // the star
    if (sn.rtcOk) {
      float sf = sn.s + secFrac;
      float mf = sn.m + sf / 60.0f;
      float hf = (sn.h % 12) + mf / 60.0f;
      auto planet = [&](float frac, int16_t r, int16_t pr, uint16_t c) {
        float a = frac * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
        int16_t x = cx + (int16_t)(cosf(a) * r);
        int16_t y = cy + (int16_t)(sinf(a) * r);
        canvas->fillCircle(x, y, pr, c);
      };
      planet(hf / 12.0f, rH, 7, th.text);
      planet(mf / 60.0f, rM, 5, th.text);
      planet(sf / 60.0f, rS, 3, th.accent);
      char tbuf[8];
      snprintf(tbuf, sizeof tbuf, "%02u:%02u", sn.h, sn.m);
      auraTextCentered(canvas, cx - 24, H - 26, tbuf, 2, th.textDim);
    }
    drawWifiToggle(auraTheme(), sn);
  }

  // ---- face 3: Photo (user background; SM Logo watermark as fallback) ----
  void faceLogo(const AuraTheme &th, const Snap &sn) {
    const uint16_t *bg = photoBgGet();
    if (bg) {
      memcpy(canvas->getFramebuffer(), bg, (size_t)W * H * 2);
      // Soft scrim behind the time so it reads on any photo: darken a band by
      // halving each RGB565 channel (fast shift-and-mask per pixel).
      uint16_t *fb = canvas->getFramebuffer();
      for (int y = 52; y < 132; y++) {
        uint16_t *row = fb + (size_t)y * W;
        for (int x = 0; x < W; x++)
          row[x] = (row[x] >> 1) & 0x7BEF;
      }
    } else {
      canvas->draw16bitRGBBitmap(0, 0, (uint16_t *)SMICON_BG,
                                 SMICON_BG_W, SMICON_BG_H);
    }
    char tbuf[8];
    snprintf(tbuf, sizeof tbuf, "%02u:%02u", sn.rtcOk ? sn.h : 0,
             sn.rtcOk ? sn.m : 0);
    auraFontCentered(canvas, &FreeSansBold24pt7b, W / 2, 96, tbuf, th.text);
    char dbuf[20];
    dateLine(dbuf, sizeof dbuf, sn);
    auraTextCentered(canvas, W / 2, 120, dbuf, 1, th.textDim);
    drawWifiToggle(th, sn);
  }

  // ---- face 4: Mono terminal ----
  void faceMono(const AuraTheme &th, const Snap &sn) {
    const uint16_t green = 0x2F62, dim = 0x1B00;
    canvas->setFont(&FreeMono24pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(green);
    char tbuf[12];
    if (sn.rtcOk) snprintf(tbuf, sizeof tbuf, "%02u:%02u:%02u", sn.h, sn.m, sn.s);
    else          snprintf(tbuf, sizeof tbuf, "--:--:--");
    int16_t x1, y1; uint16_t tw, thh;
    canvas->getTextBounds(tbuf, 0, 0, &x1, &y1, &tw, &thh);
    canvas->setCursor((W - (int16_t)tw) / 2 - x1, 120);
    canvas->print(tbuf);
    canvas->setFont(nullptr);
    char dbuf[24];
    if (sn.rtcOk)
      snprintf(dbuf, sizeof dbuf, "> %04u-%02u-%02u", sn.year, sn.mon, sn.day);
    else
      snprintf(dbuf, sizeof dbuf, "> rtc sync...");
    canvas->setTextSize(2);
    canvas->setTextColor(dim);
    canvas->setCursor(28, 150);
    canvas->print(dbuf);
    canvas->setCursor(28, 176);
    canvas->printf("> wifi %s", sn.wifiOn ? (sn.wifiUp ? "up" : "on") : "off");
    drawWifiToggle(th, sn);
    // blinking block cursor
    if ((sn.s & 1) == 0) canvas->fillRect(28, 198, 12, 18, green);
  }

  // No-canvas fallback: bare time straight to gfx so the watch still works
  // if the PSRAM canvas ever fails to allocate.
  void legacyRender() {
    if (!gfx) return;
    Snap sn = grab();
    gfx->fillScreen(0);
    gfx->setTextSize(5);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(30, 110);
    gfx->printf("%02u:%02u", sn.h, sn.m);
  }
};

const char *WatchFaceView::kWd[7]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *WatchFaceView::kMon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static WatchFaceView sFace;
View *watchFaceViewPtr() { return &sFace; }
