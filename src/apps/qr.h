// QR share app. Three preset payloads (phone, email, website); pick one to
// display fullscreen for a phone camera to scan.
#pragma once
#include "view.h"

class QRCodeView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  enum class Mode : uint8_t { List, Display };
  Mode mode = Mode::List;
  int  selectedIdx = -1;
  bool needsRedraw = true;

  void renderList();
  void renderQR();
};
