// Stopwatch app — count-up timer anchored to the RTC epoch so it keeps
// running correctly across deep sleep. State lives in apptimer.cpp.
#pragma once
#include "view.h"

class StopwatchView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  bool     firstDraw = true;
  uint32_t lastShownElapsed = 0xFFFFFFFF;
  bool     lastShownRunning = false;

  void drawTime(uint32_t elapsedMs, bool running);
  void drawButtons(bool running);
};
