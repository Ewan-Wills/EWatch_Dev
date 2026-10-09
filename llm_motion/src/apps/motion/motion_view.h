// Motion app — live view of the on-device motion language model.
#pragma once
#include "view.h"
#include "motion_engine.h"

class MotionView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  // Tokens stream at ~17/s while the wrist moves: 20 fps keeps the strip live.
  uint16_t desiredFrameMs() const override { return 50; }

private:
  void drawChrome();
  void drawLive(const MotionSnap &s);
  void drawMap();
  int      page = 0;              // 0 live, 1 action map
  bool     firstDraw = true;
  uint32_t shownVersion = 0xFFFFFFFF;
  uint32_t shownSecond = 0xFFFFFFFF;
};

View *motionViewPtr();
