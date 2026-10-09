// Power Off — CANCEL goes back, SLEEP enters deep sleep with configured wake
// sources, OFF cuts the LDO latch entirely. The two "destructive" actions
// require an 800 ms hold to confirm.
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "power.h"
#include "ui_kit.h"
#include "controller.h"
#include "system_views.h"

class PowerOffView : public View {
public:
  void onEnter() override {
    uiClearAll();
    if (!gfx) return;
    drawBackButton();
    ThemeColors t = theme();
    gfx->setTextSize(3);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(60, 60);
    gfx->print("Power");

    drawButton(SLEEP_Y, t.accent, "Sleep");
    drawButton(OFF_Y,   MAROON,   "Off");

    armed = ARM_NONE;
    armedAt = 0;
    fired = false;
  }
  // While a button is armed we animate the hold-progress strip; TouchHold
  // events arrive at ~30 Hz anyway, but the tick keeps it smooth if the
  // finger stays perfectly still.
  uint16_t desiredFrameMs() const override { return armed != ARM_NONE ? 50 : 0; }
  void render() override {
    if (!gfx) return;
    // Repaint armed-button progress fill so user sees they're holding it.
    if (armed != ARM_NONE && !fired) {
      ThemeColors t = theme();
      uint32_t held = millis() - armedAt;
      if (held > 800) held = 800;
      int16_t y = (armed == ARM_SLEEP) ? SLEEP_Y : OFF_Y;
      uint16_t base = (armed == ARM_SLEEP) ? t.accent : MAROON;
      int16_t fill = (BTN_W - 4) * (int)held / 800;
      gfx->fillRect(BTN_X + 2, y + BTN_H - 6, fill, 4, t.fg);
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
      if (uiInRect(e.x, e.y, BTN_X, SLEEP_Y, BTN_W, BTN_H)) {
        armed = ARM_SLEEP; armedAt = millis(); fired = false;
        return;
      }
      if (uiInRect(e.x, e.y, BTN_X, OFF_Y, BTN_W, BTN_H)) {
        armed = ARM_OFF;   armedAt = millis(); fired = false;
        return;
      }
      return;
    }

    // Tolerate brief drift inside the button's hit zone — only TouchUp
    // cancels. (Cancelling on any TouchHold drift made the hold flaky.)
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
      uiClearAll();
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
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
      uiClearAll();
      gfx->setTextSize(2);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
      gfx->setCursor(60, 130);
      gfx->print("sleeping");
    }
    hapticBuzz(120, 80);
    vTaskDelay(pdMS_TO_TICKS(120));
    backlightOff();
    enterDeepSleep();         // does not return
  }
};

static PowerOffView sPowerOff;
View *powerOffViewPtr() { return &sPowerOff; }
