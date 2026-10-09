// Particle sandbox demo. Three modes cycled with swipe up/down:
//   Fireworks — auto-launching rockets; tap anywhere to launch your own.
//   Fountain  — continuous spray from the bottom; tilt bends it, touch moves it.
//   Orbit     — particles swarm the finger (or screen centre) with trails.
// Exit with the physical button or a right-swipe (global back), like Media/QR.
#pragma once
#include "view.h"

class ParticlesView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 33; }   // ~30 fps

private:
  uint8_t  mode = 0;              // 0 fireworks, 1 fountain, 2 orbit
  uint32_t modeLabelUntil = 0;    // show mode name until this millis()
  bool     touchDown = false;
  int16_t  touchX = 120, touchY = 140;
  uint32_t lastSpawnMs = 0;
  uint32_t frameCount = 0;

  void resetParticles();
  void step();                    // advance + draw one frame
};
