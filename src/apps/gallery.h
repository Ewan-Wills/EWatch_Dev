// Photos gallery — browse LittleFS photos; long-press sets the background.
#pragma once
#include "view.h"

class GalleryView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;

private:
  void bump();
  int  n = 0, cur = 0;
  bool dirty = true;
};
