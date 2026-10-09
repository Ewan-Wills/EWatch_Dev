// Bluetooth LE mouse for EWatchOS.
//
// The mouse drives the watch the way a finger does, so every screen works
// unchanged:
//   * cursor     drawn over whatever is on screen (composited from the ewsim
//                bus-tap shadow, so it also works over views that draw
//                straight to the panel); hides after 5 s without movement
//   * left       press/hold/release = finger down/hold/up at the cursor.
//                click = tap, double-click = double tap, hold 0.8 s = long
//                press, quick flick-drag = swipe gesture, slow drag = drag
//                (sliders, scroll lists)
//   * wheel      scrolls: a short finger flick at the cursor, so lists
//                scroll with their own physics
//   * tilt/pan   horizontal wheel = swipe left / right (pages)
//   * right      back (same as a side-button press)
//   * middle     home (watch face)
//   * back btn   back (5-button mice)
//
// Pairing lives in the Mouse app (launcher). The paired mouse is remembered
// and reconnected automatically while Mouse is on. Mouse movement wakes a
// dimmed or dark screen. While connected the watch doesn't enter real light
// sleep (bluedroid would drop the link), so battery drain is higher.
#pragma once
#include "view.h"

// ---- controller.cpp / power_mgr.cpp hooks (all cheap, any task) ----
void    mouseTouch(bool &live, uint16_t &x, uint16_t &y);   // virtual finger
Gesture mouseTakeGesture(uint16_t &x, uint16_t &y);
bool    mouseButton();                                     // virtual side button
int     mouseTakeGoto();                                   // -1 or Screen id
bool    mouseTakeWake();                                   // moved while screen off
bool    mouseTakeActivity();                               // moved since last call
bool    mouseConnected();
bool    mouseCursorActive();                               // cursor showing / moving
void    mouseCursorPaint();                                // render task, after render()
void    mouseBegin();                                      // boot: auto-start if enabled
bool    mouseHandleLine(char *line);                       // ewlog console: `msim ...` test hook

class MouseView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 250; }
private:
  bool firstDraw = true;
  uint32_t shownGen = 0xFFFFFFFF;
  void drawAll();
};
