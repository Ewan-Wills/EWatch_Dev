// IslandView — the cozy low-poly floating-island watch face.
//
// A small living 3D diorama that tells the time three ways: ambient (sky / sun
// / light driven by the RTC), monumental (the clock-tower hands), and — later —
// interactive (walk around, put out the campfire to power off).
//
// This is registered as an app (Screen::Island) so it's launchable from the
// carousel for testing; promoting it to the default face is a one-line change
// in viewsInit().  See CLAUDE.md → "Adding an app".
#pragma once
#include "view.h"

class IslandView : public View {
public:
  // One frame per render() call; ~20 fps for the living diorama.
  uint16_t desiredFrameMs() const override { return 50; }
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override;

private:
  bool   sceneBuilt = false;   // geometry is baked once, lazily
  int    lastMinute = -1;      // force a clock-hand repaint on change
};
