// hidctl — control the watch with a keyboard / mouse.
//
// FEASIBILITY (see NOTES.md for the full write-up): USB-host is NOT wired on
// this board — the single USB port is the ESP32-S3 native-USB *device* port
// used for charge + flash, and there is no VBUS-source / host-enable / 5 V
// boost pin anywhere in pins.h. So the watch cannot act as a USB host and
// power a keyboard/mouse. The real wireless path is BLE-HID *host* (central),
// which the S3 radio supports, but a robust HID-central + report-descriptor
// parser is too heavy to land compile-clean + verifiable for this MVP.
//
// This app therefore ships the full on-watch half — a Notes editor + a mouse
// cursor overlay that can tap the UI — behind a clean, transport-agnostic
// INPUT SEAM. Today the seam is driven by a USB-serial feeder (pc/hid_feeder.py
// reads the Mac's real keyboard + mouse and streams decoded events over CDC).
// Dropping in a BLE-HID-central transport later means only implementing a new
// producer that calls the same hidFeed*() functions below — the Notes UI and
// cursor code do not change.

#pragma once
#include "view.h"

namespace hid {

// Non-printable keys the Notes editor understands. Printable characters come
// through hidFeedKeyChar() as raw ASCII; everything else is one of these.
enum class SpecialKey : uint8_t {
  Enter, Backspace, Tab, Esc, Left, Right, Up, Down, Home, End
};

// ---- INPUT SEAM --------------------------------------------------------
// A transport (the serial feeder now; a BLE-HID central later) calls these
// to inject input. They are thread-safe: they enqueue onto a small internal
// ring and nudge the render task to repaint. They are safe to call before the
// Notes screen is open (events are simply dropped if the app is not active),
// so a transport does not need to know the view lifecycle.
void hidFeedKeyChar(char c);              // a printable ASCII char (32..126)
void hidFeedKeySpecial(SpecialKey k);     // Enter / Backspace / arrows / ...
void hidFeedMouseMove(int dx, int dy);    // relative mouse motion, signed
void hidFeedMouseButton(bool down);       // left button state (edge = click)

}  // namespace hid

class HidCtlView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  void handleTap(uint16_t x, uint16_t y);   // shared by touch + mouse-click
  bool drainInput();                         // apply queued key/mouse events
};
