// Draw app: finger painting with colours, sizes, eraser, undo, clear and
// send (eWatch Cloud, as an image). State lives in draw_view.cpp statics so
// the drawing survives leaving the app (PSRAM, while powered).
#pragma once
#include "view.h"

class DrawView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  // 16 ms while a stroke is going down, else 50 (never 0: like Write, the
  // app holds 240 MHz while open; render() returns at once when idle).
  uint16_t desiredFrameMs() const override;
};
