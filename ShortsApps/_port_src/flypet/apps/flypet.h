// flypet — a virtual-fly tamagotchi.
//
// A procedurally-drawn housefly lives on the watch as a pet. Its behaviour is
// driven by a Mac-side fly "brain" simulation (a nod to the Drosophila
// connectome work) that streams commands over USB serial:
//
//     FLY action=<walk|groom|rest|feed|wander> x=<0..100> y=<0..100>
//         hunger=<0..100> energy=<0..100> mood=<0..100> [state=<label>]\n
//
// The HEAVY sim is entirely Mac-side; the watch only animates a believable fly
// with state (hunger / energy / mood) and a handful of behaviours
// (walk / groom / rest / feed), plus an autonomous wander mode so the pet is
// alive even with no PC attached. Tapping the fly drops food and sends a
// `>>FEED x=.. y=..` event back to the PC. See NOTES.md for the full protocol.
//
// Drawing follows the existing `tamagotchi` app: no bitmap assets, everything
// is vector-drawn into the shared frame canvas with delta-free full-canvas
// redraws (the canvas is a PSRAM back-buffer, so this does not flicker).
#pragma once
#include "view.h"

class FlyPetView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
};
