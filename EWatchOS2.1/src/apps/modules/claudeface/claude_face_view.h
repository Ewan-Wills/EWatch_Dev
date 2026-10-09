// ClaudeFace as an EWatchOS app: the Claude Code face (agent state, usage
// rings, turn timer) fed over USB by the Mac bridge (companions/claudeface).
// Drawn landscape: the watch on its side, top edge pointing right. While it
// is on screen the watch doesn't sleep. Swipe down (viewer frame) = leave.
#pragma once
#include "view.h"

class ClaudeFaceView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 33; }
};

void claudeFaceModuleBegin();   // boot: settings + link (the console feeds it)
