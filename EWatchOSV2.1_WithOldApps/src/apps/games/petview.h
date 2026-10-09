// Virtual-pet screen — the face of the Tamagotchi app.
//
// Reads a PetSnapshot from pet.cpp each frame and paints a procedurally-drawn
// creature (no bitmap assets) into the shared frame canvas: a top stat HUD, an
// animated creature whose pose/expression follows its mood, a flavour status
// line, and a four-button action bar (Feed / Play / Clean / Sleep). Input —
// taps, swipes, and shakes — is translated into pet actions; haptics give the
// creature its personality.
#pragma once
#include "view.h"

class PetView : public View {
public:
  // One frame per render() call; ~15 fps keeps the pet animated.
  uint16_t desiredFrameMs() const override { return 66; }
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
};
