// StarFox-lite — on-rails wireframe space shooter.
//
// First-person on-rails flight: a starfield streaks past, enemies scale in from
// the vanishing point, you tilt to aim a reticle, tap to fire, and barrel-roll
// (swipe) to dodge. Shield/score/wave loop with a boss every 5 waves; high
// score persists to NVS.
//
// All game state lives in a single file-static struct in the .cpp (there is
// exactly one view instance), so render() and onEvent() share it cleanly —
// the previous build kept state in render()-local statics, which is why
// onEvent() couldn't hit-test ("touch bugs").
#pragma once
#include "view.h"

class StarfoxShooterView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override;
};
