// ScrubMarine watch face + screensaver.
//
//   ScrubMarineFaceView — the default watch face (Screen::Watch). A big digital
//   HH:MM over the dimmed SM-logo watermark background, with a date line and a
//   small battery readout. Swipe left to open the app launcher.
//
//   ScreenSaverView — the idle screen (Screen::ScreenSaver). Shows the same
//   large centred SM logo as the watch face, minus the time and a little dimmer.
//   Entered by the render task after model.screensaverTimeoutSec of inactivity.
//   Dismiss needs a *deliberate* input — a sustained finger press or the button;
//   stray touch frames and IMU jolts are ignored so it doesn't wake on its own.
//   Deep sleep still follows at model.sleepTimeoutSec.
#pragma once
#include "view.h"

class ScrubMarineFaceView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
private:
  bool     forceDraw = true;
  uint8_t  lastH = 99, lastM = 99;
  uint8_t  lastDay = 0, lastMon = 0, lastWd = 9;
  uint16_t lastYear = 0;
  uint8_t  lastBat = 255;
  bool     lastRtcOk = false, lastBatOk = false;
};

class ScreenSaverView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
private:
  bool     drawn        = false;   // static image — paint once per entry
  uint32_t entryMs      = 0;       // for the post-entry input grace window
  bool     pressActive  = false;   // a finger is currently down
  uint32_t pressStartMs = 0;       // when it went down (for the hold confirm)
};
