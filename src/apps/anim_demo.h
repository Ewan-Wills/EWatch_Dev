// Animation framework showcase. Three shapes + a 2x3 button pad triggers
// each property class (move / rotate / scale / fade / color / reset). Tap a
// shape to pulse just that one. See src/core/anim.h for the framework API.
#pragma once
#include "view.h"

class AnimDemoView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &) override {}     // unused — render() drains its own events
};
