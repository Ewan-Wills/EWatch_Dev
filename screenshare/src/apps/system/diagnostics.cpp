// Diagnostic pages: Sensor Test, Touch Gestures, IMU Gestures.
#include <Arduino_GFX_Library.h>
#include "pins.h"
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "system_views.h"

static const int16_t W = 240;

// =====================================================================
// Sensor Test — diagnostic readout. RTC, IMU bars, touch, button, INTs,
// battery, uptime/heap. Swipe right or long-press SW2 -> back.
// =====================================================================
class SensorTestView : public View {
public:
  void onEnter() override { uiClearAll(); firstDraw = true; }
  // Live sensor readout — refresh at ~10 fps even without input events now
  // that the I/O task no longer bumps the model revision on every cycle.
  uint16_t desiredFrameMs() const override { return 100; }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    bool intRtc  = digitalRead(PIN_RTC_INT);
    bool intMma1 = digitalRead(PIN_MMA_INT1);
    bool intMma2 = digitalRead(PIN_MMA_INT2);

    if (firstDraw) {
      drawTitleBar("Sensors", 8, 32, 8, W - 16);
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

// =====================================================================
// Touch Gestures — visualize CST816S gesture stream. Big label for the most
// recent gesture, small history list under it, live touch dot on a pad.
// =====================================================================
class TouchGesturesView : public View {
public:
  void onEnter() override {
    uiClearAll();
    firstDraw = true;
    histCount = 0;
    lastGesture = Gesture::None;
    lastGestureMs = 0;
    haveDot = false;
    dotX = dotY = 0;
  }
  // The "fresh" highlight on the current gesture decays after 800 ms with no
  // further events — tick at 5 fps so it fades even when the user stops.
  uint16_t desiredFrameMs() const override { return 200; }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawTitleBar("Gestures", 8, 32, 8, W - 16);
      { ThemeColors t = theme();
        gfx->drawRoundRect(PAD_X, PAD_Y, PAD_W, PAD_H, 8, t.line); }
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
      if (uiInRect(e.x, e.y, PAD_X, PAD_Y, PAD_W, PAD_H)) {
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
    gfx->setCursor(uiCenterX(s, 3), LABEL_Y + 4);
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
// IMU Gestures — visualizes orientation + recent shake events.
// =====================================================================
class ImuGesturesView : public View {
public:
  void onEnter() override {
    uiClearAll();
    firstDraw = true;
    motionCount = 0;
    lastMotionMs = 0;
    lastDrawnOrient = -1;
    lastDotX = lastDotY = -1;
  }
  // Tilt ball follows the live accelerometer — 20 fps keeps it fluid.
  uint16_t desiredFrameMs() const override { return 50; }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }

    if (firstDraw) {
      drawTitleBar("IMU", 8, 32, 8, W - 16);
      { ThemeColors t = theme();
        gfx->drawCircle(BALL_CX, BALL_CY, BALL_R, t.line);
        gfx->drawCircle(BALL_CX, BALL_CY, 2,      t.line); }
      firstDraw = false;
    }

    // Orientation classification from accel direction.
    int o = classifyOrientation(snap);
    if (o != lastDrawnOrient) {
      gfx->fillRect(0, 42, W, 22, BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(CYAN, BLACK);
      const char *s = orientName(o);
      gfx->setCursor(uiCenterX(s, 2), 42);
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

static SensorTestView    sSensorTest;
static TouchGesturesView sTouchGestures;
static ImuGesturesView   sImuGestures;
View *sensorTestViewPtr()    { return &sSensorTest; }
View *touchGesturesViewPtr() { return &sTouchGestures; }
View *imuGesturesViewPtr()   { return &sImuGestures; }
