// Craft — a small Minecraft-style voxel game for the EWatch.
//
// Clean-room per-pixel voxel raycaster (Minecraft4k-style technique, no code
// from it): 64x64x32 island world, 4-bit blocks, procedural 16x16 textures,
// rendered at 120x140 on both cores and doubled to 240x280.
//
// Xbox controller (Xbox Pad app pairs it):
//   left stick move (click = sprint)   right stick look
//   A jump / swim / fly up             B fly down
//   RT mine                            LT place
//   LB / RB or d-pad left/right: block  Y fly on/off
//   View: FPS counter                  Menu or side button: quit
// Touch fallback: drag left third = move, drag elsewhere = look, tap = mine,
// long press = place, double tap = jump, tap the hotbar = pick a block.
// Idle with no controller: slow attract-mode camera pan.
#pragma once
#include "view.h"

class CraftView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 1; }
};
