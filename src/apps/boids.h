// Flock demo — two ambient scenes in one app, toggled with a tap:
//   Boids     — a flock with separation/alignment/cohesion; tilt is wind,
//               touch-and-hold scatters them like a predator.
//   Starfield — 3D fly-through; tilt steers, touch-and-hold engages warp.
// Exit with the physical button or a right-swipe (global back).
#pragma once
#include "view.h"

class BoidsView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 33; }   // ~30 fps

private:
  uint8_t  scene = 0;             // 0 boids, 1 starfield
  uint32_t sceneLabelUntil = 0;
  bool     touchDown = false;
  int16_t  touchX = 120, touchY = 140;
  // Tap-vs-hold: a quick TouchUp with no drag toggles the scene.
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressMs = 0;
  bool     pressMoved = false;

  void resetScene();
  void stepBoids();
  void stepStars();
};
