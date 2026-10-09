// ScreenSaverView — the idle screen (Screen::ScreenSaver). Shows the large
// centred SM logo, dimmed. Entered by the render task after
// model.screensaverTimeoutSec of inactivity on the watch face; deep sleep
// still follows at model.sleepTimeoutSec.
//
// Dismiss needs a *deliberate* input — a sustained finger press or the
// button; stray CST816S touch frames and IMU jolts are ignored so it doesn't
// wake on its own. (Ported from the ScrubMarine slate.)
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "ui_kit.h"
#include "system_views.h"
#include "smicon_bg.h"     // SMICON_BG  240x280 logo watermark

// Backlight while the screensaver is up: a fraction of the user's setting so
// it's noticeably softer than the watch face but not pitch black. Floored so
// a low brightness setting doesn't make it invisible.
static uint8_t saverBrightness() {
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  int dim = (int)br * 6 / 10;          // ~60% of the user's brightness
  if (dim < 24) dim = 24;
  return (uint8_t)dim;
}

// Input protection. Stray CST816S frames tend to be single, momentary blips,
// so we only accept a finger that stays down for a real dwell before
// dismissing. The dwell is user-tunable (Settings -> Wake, "Touch protect");
// 0 = wake on the very first touch frame. The short entry grace only eats
// the trailing frames of whatever touch preceded the saver.
static const uint16_t SAVER_GRACE_MS = 300;   // ignore all input just after entry

static uint16_t saverHoldMs() {
  ModelLock lk;
  return model.touchWakeHoldMs;
}

class ScreenSaverView : public View {
public:
  void onEnter() override {
    drawn        = false;
    entryMs      = millis();
    pressActive  = false;
    pressStartMs = 0;
    if (gfx) gfx->fillScreen(BLACK);
    backlightSet(saverBrightness());
  }

  void onExit() override {
    uint8_t br;
    { ModelLock lk; br = model.brightness; }
    backlightSet(br);
  }

  void render() override {
    if (!gfx) return;
    if (drawn) return;                   // static — nothing changes per frame
    gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)SMICON_BG,
                            SMICON_BG_W, SMICON_BG_H);
    drawn = true;
  }

  void onEvent(const Event &e) override {
    // Swallow everything for a beat after the screensaver appears — this eats
    // the trailing touch frame from whatever the user last did.
    if (millis() - entryMs < SAVER_GRACE_MS) {
      if (e.type == EventType::TouchUp) pressActive = false;
      return;
    }

    switch (e.type) {
      // The physical button is reliable — a press is always intentional.
      case EventType::ButtonDown:
      case EventType::ButtonShort:
        switchTo(Screen::Watch);
        return;

      // Touch must be *sustained*. A stray frame lands as a lone Touch with
      // no follow-up holds; a real finger keeps generating TouchHold at ~30 Hz.
      // With protection set to 0, the first Touch frame wakes immediately.
      case EventType::Touch:
        if (saverHoldMs() == 0) { switchTo(Screen::Watch); return; }
        pressActive  = true;
        pressStartMs = millis();
        return;
      case EventType::TouchHold:
        if (pressActive && (millis() - pressStartMs) >= saverHoldMs()) {
          switchTo(Screen::Watch);
        }
        return;
      case EventType::TouchUp:
        pressActive = false;
        return;

      // Deliberately ignored: ImuMotion (movement/vibration is not intent)
      // and bare gestures (touch-derived, so they can ride in on a stray
      // frame).
      default:
        return;
    }
  }

private:
  bool     drawn        = false;   // static image — paint once per entry
  uint32_t entryMs      = 0;       // for the post-entry input grace window
  bool     pressActive  = false;   // a finger is currently down
  uint32_t pressStartMs = 0;       // when it went down (for the hold confirm)
};

static ScreenSaverView sSaver;
View *screenSaverViewPtr() { return &sSaver; }
