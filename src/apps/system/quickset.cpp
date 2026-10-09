// Control Centre — swipe down from the watch face.
//
// A sheet of quick controls, Apple-style:
//   * brightness slider (live, persists on release)
//   * WiFi toggle   * silent-mode toggle (haptics off)
//   * torch tile    * settings shortcut
//   * battery readout across the top
// The sheet is taller than the screen: drag or fling to scroll (same
// physics as the menu lists). Swipe up dismisses when the swipe starts with
// the sheet already scrolled to the bottom; the button always dismisses.
//
// TorchView also lives here: full-white screen at max backlight, tap to
// cycle white -> red -> off-back-to-facce. Blocks sleep via power_mgr.
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "ui_icons.h"
#include "ui_motion.h"
#include "storage.h"
#include "wifi_svc.h"
#include "power_mgr.h"
#include "system_views.h"

using namespace aura;

// ---------------------------------------------------------------------------
// Control centre
// ---------------------------------------------------------------------------
class QuickSettingsView : public View {
public:
  void onEnter() override {
    canvas = frameCanvas();
    dragBright = false;
    // The opening swipe can leave its trailing TouchUp queued behind the
    // Gesture event; starting with swiped=true swallows it (a real tap always
    // begins with a fresh Touch, which re-arms).
    swiped = true;
    phys.pos = 0; phys.velocity = 0; phys.release();
    phys.maxScroll = CONTENT_H > H ? (float)(CONTENT_H - H) : 0.0f;
    entrance.start(18, 0, 220, easeOutCubic);
  }

  bool paintTo(Arduino_Canvas *cv) override {
    if (!cv) return false;
    canvas = cv;
    drawFrame();
    return true;
  }

  void render() override {
    if (!canvas) return;
    powerPerfDemand(true);            // blocking loop: governor can't help us
    for (;;) {
      esp_task_wdt_reset();
      uint32_t t0 = millis();
      if (dragBright && millis() - lastFingerMs > 400) {
        dragBright = false;         // lost TouchUp — end the drag, persist
        Storage::save();
      }
      if (phys.dragging && millis() - lastFingerMs > 400)
        phys.release();             // lost TouchUp — let the scroll settle
      phys.step();
      drawFrame();
      canvas->flush();
      if (!animating()) return;
      int32_t budget = 33 - (int32_t)(millis() - t0);
      if (budget < 1) budget = 1;
      while (budget > 0) {
        Event e;
        if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) != pdPASS) break;
        handleEvent(e);
        // A tap/gesture may have switched screens (torch, settings, dismiss);
        // drawing another control-centre frame now would overpaint them.
        if (currentView != this) return;
        budget = 33 - (int32_t)(millis() - t0);
      }
    }
  }

  void onEvent(const Event &e) override {
    handleEvent(e);
    if (animating()) { ModelLock lk; model.revision++; }
  }

private:
  Arduino_Canvas *canvas = nullptr;
  Tween  entrance;
  bool   dragBright = false;
  bool   brightCandidate = false;   // Touch landed on the slider; confirm on Hold
  uint32_t lastFingerMs = 0;        // drag watchdog (lost-TouchUp recovery)
  uint16_t downX = 0, downY = 0;
  bool     swiped = false;          // finger travelled: suppress the tile tap
  ScrollPhys phys;                  // vertical scroll of the whole sheet
  float    posAtDown = 0;           // scroll offset when the finger landed
  uint32_t downMs = 0;

  // Layout (content coordinates).
  static const int16_t SLIDER_X = 24, SLIDER_Y = 96, SLIDER_W = W - 48;
  static const int16_t TILE = 92, TGAP = 16;
  static const int16_t ROW1 = 136, ROW2 = ROW1 + TILE + TGAP;
  // The second tile row ends at 336 px, past the 280 px screen: scroll.
  static const int16_t CONTENT_H = ROW2 + TILE + 16;

  int16_t scrollY() const { return (int16_t)phys.pos; }

  bool animating() const {
    return !entrance.done() || dragBright || phys.moving();
  }

public:
  uint16_t desiredFrameMs() const override { return animating() ? 33 : 0; }

private:

  void drawTile(int col, int row, Icon ic, const char *label, bool active,
                uint16_t activeColor, const AuraTheme &th) {
    int16_t x = (W - 2 * TILE - TGAP) / 2 + col * (TILE + TGAP);
    int16_t y = (row == 0 ? ROW1 : ROW2) - scrollY();
    auraCard(canvas, x, y, TILE, TILE, kRadius, active ? activeColor : th.card);
    drawIcon(canvas, ic, x + TILE / 2, y + TILE / 2 - 10, 34,
             active ? kText : th.textDim, active ? activeColor : th.card);
    auraTextCentered(canvas, x + TILE / 2, y + TILE - 22, label, 1,
                     active ? kText : th.textDim);
  }

  void drawFrame() {
    AuraTheme th = auraTheme();
    canvas->fillScreen(th.bg);
    float rise = entrance.value() - phys.pos;   // entrance lift + scroll
    const int16_t sy = scrollY();

    // Grab handle + battery header.
    canvas->fillRoundRect(W / 2 - 22, 8 - sy, 44, 5, 2, th.line);
    uint8_t pct; bool ok, low; uint8_t bright; bool wifiOn; uint8_t hstr;
    { ModelLock lk; pct = model.batPct; ok = model.batOk; low = model.batLow;
      bright = model.brightness; wifiOn = model.wifiEnabled;
      hstr = model.hapticStrength; }

    if (ok) {
      auraBatteryRing(canvas, 36, 44 + (int16_t)rise, 16, pct, low);
      char b[8];
      snprintf(b, sizeof b, "%u%%", pct);
      canvas->setTextSize(2);
      canvas->setTextColor(th.text);
      canvas->setCursor(62, 36 + (int16_t)rise);
      canvas->print(b);
    } else {
      auraTextCentered(canvas, 60, 40 + (int16_t)rise, "bat n/a", 1, th.textDim);
    }
    canvas->setTextSize(1);
    canvas->setTextColor(th.textDim);
    canvas->setCursor(W - 84, 40 + (int16_t)rise);
    canvas->print("Control");

    // Brightness slider with sun icons.
    drawIcon(canvas, Icon::Sun, SLIDER_X - 6, SLIDER_Y + 3 - sy, 14, th.textDim, th.bg);
    auraSlider(canvas, SLIDER_X + 12, SLIDER_Y - sy, SLIDER_W - 24,
               bright / 255.0f, th.accent);
    drawIcon(canvas, Icon::Sun, SLIDER_X + SLIDER_W + 4, SLIDER_Y + 3 - sy, 22, th.text, th.bg);

    // Tiles.
    drawTile(0, 0, Icon::Wifi,  wifiOn ? "WiFi on" : "WiFi",
             wifiOn, kBlue, th);
    drawTile(1, 0, Icon::Vibrate, hstr ? "Haptics" : "Silent",
             hstr == 0, kPurple, th);
    drawTile(0, 1, Icon::Torch, "Torch", false, th.card, th);
    drawTile(1, 1, Icon::Gear,  "Settings", false, th.card, th);
    auraScrollbar(canvas, phys.pos, phys.maxScroll, H);
  }

  int tileAt(int16_t x, int16_t y) {
    y += scrollY();                          // screen -> content
    for (int row = 0; row < 2; row++)
      for (int col = 0; col < 2; col++) {
        int16_t tx = (W - 2 * TILE - TGAP) / 2 + col * (TILE + TGAP);
        int16_t ty = (row == 0 ? ROW1 : ROW2);
        if (x >= tx && x < tx + TILE && y >= ty && y < ty + TILE)
          return row * 2 + col;
      }
    return -1;
  }

  void applyBrightness(int16_t x) {
    float v = (float)(x - (SLIDER_X + 12)) / (float)(SLIDER_W - 24);
    if (v < 0) v = 0; else if (v > 1) v = 1;
    uint8_t b = (uint8_t)(10 + v * 245.0f);       // floor so it never goes black
    { ModelLock lk; model.brightness = b; }
    backlightSet(b);
  }

  void handleEvent(const Event &e) {
    powerOnActivity();             // in-loop drains bypass taskRender's call
    switch (e.type) {
      case EventType::TimerExpired:
        switchTo(Screen::Timer);
        return;
      case EventType::ButtonVeryLong:
        switchTo(Screen::PowerOff);
        return;
      case EventType::Touch:
        lastFingerMs = millis();
        downX = e.x; downY = e.y; swiped = false;
        downMs = millis();
        posAtDown = phys.pos;
        phys.grab(e.y);
        // Only NOMINATE the slider here — a synthetic wake-tap Touch has no
        // TouchUp and must not rewrite brightness from its x coordinate.
        { int16_t cy = e.y + scrollY();
          brightCandidate = (cy > SLIDER_Y - 22 && cy < SLIDER_Y + 28); }
        break;
      case EventType::TouchHold: {
        lastFingerMs = millis();
        if (brightCandidate && !dragBright) {
          dragBright = true;        // confirmed by a live finger
        }
        if (dragBright) { applyBrightness(e.x); break; }
        int16_t dx = (int16_t)e.x - (int16_t)downX;
        int16_t dy = (int16_t)e.y - (int16_t)downY;
        if ((int32_t)dx * dx + (int32_t)dy * dy > 14 * 14) swiped = true;
        if (swiped) phys.drag(e.y);
        break;
      }
      case EventType::TouchUp:
        brightCandidate = false;
        if (dragBright) { dragBright = false; phys.release(); Storage::save(); break; }
        {
          // A flick faster than the TouchHold stream never armed a drag:
          // turn its travel into a fling (same as the menu lists).
          int16_t dy = (int16_t)downY - (int16_t)e.y;
          int16_t dx = (int16_t)e.x - (int16_t)downX;
          bool travelled = (int32_t)dx * dx + (int32_t)dy * dy > 14 * 14;
          if (!swiped && travelled) {
            uint32_t dt = millis() - downMs;
            if (dt < 1) dt = 1;
            float v = (float)dy * 1000.0f / (float)dt;
            if (v > 6000.0f) v = 6000.0f;
            if (v < -6000.0f) v = -6000.0f;
            phys.velocity = v;
            swiped = true;
          }
        }
        phys.release();
        if (!swiped) handleTap(e.x, e.y);
        swiped = false;
        break;
      case EventType::Gesture:
        // Swipe up scrolls the sheet; it only dismisses when the swipe
        // began with the sheet already at the bottom (or it fits).
        if (e.gesture == Gesture::SwipeUp && posAtDown >= phys.maxScroll - 1.0f)
          dismiss();
        break;
      case EventType::ButtonShort:
        dismiss();
        break;
      default: break;
    }
  }

  void handleTap(int16_t x, int16_t y) {
    int t = tileAt(x, y);
    if (t < 0) return;
    hapticBuzz(24, 30);
    switch (t) {
      case 0: {                                        // WiFi toggle
        bool on;
        { ModelLock lk; model.wifiEnabled = !model.wifiEnabled;
          on = model.wifiEnabled; model.revision++; }
        Storage::save();
        if (on) wifiSvcKickWindow();     // connect now, skip scheduler backoff
        break;
      }
      case 1: {                                        // silent toggle
        uint8_t hs;
        { ModelLock lk;
          model.hapticStrength = model.hapticStrength ? 0 : 100;
          hs = model.hapticStrength; model.revision++; }
        hapticSetStrengthPct(hs);
        Storage::save();
        if (hs) hapticBuzz(40, 45);                    // audible "back on" cue
        break;
      }
      case 2:
        switchTo(Screen::Torch);
        break;
      case 3:
        switchToAnimated(Screen::Settings, Trans::PushLeft);
        break;
    }
  }

  void dismiss() {
    hapticBuzz(18, 20);
    switchToAnimated(Screen::Watch, Trans::CurtainUp);
  }
};

// ---------------------------------------------------------------------------
// Torch
// ---------------------------------------------------------------------------
class TorchView : public View {
public:
  void onEnter() override {
    mode = 0;
    { ModelLock lk; savedBright = model.brightness; }
    backlightSet(255);
    paint();
  }
  void onExit() override { backlightSet(savedBright); }

  void render() override { paint(); }

  void onEvent(const Event &e) override {
    if (e.type == EventType::Touch) {
      mode++;
      if (mode > 1) { switchTo(Screen::Watch); return; }
      paint();
    } else if (e.type == EventType::ButtonShort ||
               (e.type == EventType::Gesture && e.gesture == Gesture::SwipeUp)) {
      switchTo(Screen::Watch);
    }
  }

private:
  int     mode = 0;              // 0 = white, 1 = red (night vision)
  uint8_t savedBright = 200;
  void paint() {
    if (!gfx) return;
    gfx->fillScreen(mode == 0 ? 0xFFFF : 0xF800);
  }
};

static QuickSettingsView sQuick;
static TorchView         sTorch;
View *quickSettingsViewPtr() { return &sQuick; }
View *torchViewPtr()         { return &sTorch; }
