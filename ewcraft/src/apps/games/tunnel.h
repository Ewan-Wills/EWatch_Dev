// Wireframe endless tunnel runner — the EWatch showpiece.
//
// You fly forever down a glowing wireframe tube. Tilt the watch to orbit a
// marker around the tunnel wall and line it up with the gap in each oncoming
// obstacle ring; miss the gap and you crash. Distance is the score; the best
// run is persisted to NVS (namespace "tracer").
//
// Unlike the other views this one renders into the shared PSRAM frame canvas
// and runs its OWN frame loop inside render() — it does not return between
// frames while the game is live, so the frame rate is decoupled from the
// model-dirty render cadence and stays high and steady. The loop feeds the
// task watchdog every frame and drains the event queue itself.
#pragma once
#include "view.h"

class TunnelRacerView : public View {
public:
  // Nonzero so taskRender raises the CPU to 240 MHz before render() blocks —
  // the wireframe projection was written against the full clock.
  uint16_t desiredFrameMs() const override { return 33; }
  void onEnter() override;
  void onExit()  override {}
  void render()  override;                  // runs the game loop until we leave
  void onEvent(const Event &e) override;    // only used in the no-canvas fallback
};
