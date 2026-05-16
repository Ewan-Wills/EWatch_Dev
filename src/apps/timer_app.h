// Countdown timer app. Three modes:
//   Setting  — pick H/M/S, then START
//   Running  — live countdown + CANCEL
//   Fired    — "TIME UP" alarm with pulsing haptic until dismissed
//
// The timer is anchored to an absolute RTC-epoch deadline (apptimer.cpp), so
// the controller can detect expiry on any RTC read and enterDeepSleep() can
// arm a hardware wake-up — the watch alarms even if it went to sleep.
#pragma once
#include "view.h"

class TimerView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  enum class Mode : uint8_t { Setting, Running, Fired };
  Mode     mode = Mode::Setting;
  bool     firstDraw = true;

  // Duration being set.
  uint8_t  setH = 0, setM = 5, setS = 0;

  // Cached display state.
  uint32_t lastShownRemain = 0xFFFFFFFF;
  uint32_t lastBuzzMs = 0;

  // Alert state: timer-fired flashing-bg + triplet-buzz pattern.
  uint32_t alertCycleStartMs = 0;   // start of the current ~1.2 s pattern cycle
  uint8_t  alertPulseStage   = 0;   // 0..3 — pulses already fired this cycle
  int8_t   lastAlertFlip     = -1;  // -1 = unknown, 0/1 = drawn state

  void enterSetting();
  void enterRunning();
  void enterFired();

  void drawSetting();
  void drawSettingDigits();
  void drawRunning(uint32_t remainSec);
  void drawFired();

  void bumpField(int col, int8_t dir);
};
