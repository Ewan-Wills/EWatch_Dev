// StreamView — mirror a Mac window/region to the watch over WiFi.
//
// The Mac companion (companions/screenshare/screenshare.py --wifi, or the
// companions/livestream/stream_to_watch.py wrapper) connects to TCP port 7878
// on ewatch.local and sends the same packets as the USB Screenshare app:
// changed rectangles as JPEG, one packet in flight. Format, replies and flow
// control: core/ewstream.h. A core-0 task owns the socket; the render task
// only decodes and pushes ready frames, so back/goto/power keep working.
//
// Whole file gated on EW_APP_LIVESTREAM AND EWATCH_ENABLE_WIFI.
#pragma once
#if defined(EW_APP_LIVESTREAM) && EW_APP_LIVESTREAM && \
    defined(EWATCH_ENABLE_WIFI)   && EWATCH_ENABLE_WIFI
#include "view.h"

class StreamView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 1; }   // see ScreenshareView

private:
  enum class St : uint8_t { NoWifi, NoMem, Live };
  St       st = St::NoWifi;
  bool     showing = false, drawn = false;
  uint32_t lastStatMs = 0, statFrames = 0;
  void drawWaiting();
  void drawError(const char *msg, const char *hint);
};

#endif
