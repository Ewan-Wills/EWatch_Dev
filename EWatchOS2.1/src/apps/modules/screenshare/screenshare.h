// Screenshare — mirror a Mac window/region to the watch face over USB.
//
// The Mac companion (companions/screenshare/screenshare.py) captures the
// screen, scales it to 240x280, sends only the rectangle that changed as a
// JPEG, and keeps one packet in flight. Packet format, replies and flow
// control: core/ewstream.h (shared with the WiFi livestream module).
//
// USB: while packets flow, the HWCDC driver's RX interrupt is masked and a
// core-0 task reads the USB-Serial/JTAG FIFO directly. The FIFO is only 64 B,
// but while it's full the host is NAKed, so nothing is dropped and there are
// no per-block ACK round trips (the old ACK-per-2-KiB protocol is gone).
// Between streams HWCDC owns RX again. Replies are written straight to the
// USB IN FIFO (see usbWrite). Text lines between packets go to ewsim, so ewt
// (snap, state, btn, goto) works while the app is open and no host streams.
#pragma once
#include "view.h"

class ScreenshareView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  // Ask for continuous render() calls: each one waits up to 10 ms for a
  // received frame. <= 66 also keeps the CPU at 240 MHz (controller.cpp).
  uint16_t desiredFrameMs() const override { return 1; }

private:
  bool     live = false;              // USB takeover active (not under keepcon)
  bool     showing = false;           // frames on screen (vs waiting banner)
  bool     waitingDrawn = false;
  uint32_t lastStatMs = 0, statFrames = 0;
  void drawWaiting();
};
