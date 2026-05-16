// QR share app. A full-screen QR carousel — swipe up/down to move between
// preset payloads (phone / email / website / LinkedIn). Exits only on the
// physical button or a back-swipe, never on a plain tap. render() blocks and
// drains its own events, so the watch also won't auto-sleep while it's open.
#pragma once
#include "view.h"

class QRCodeView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &) override {}   // render() drains its own events

private:
  int index = 0;
};
