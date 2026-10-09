// Macropad — a touchscreen macro keyboard. Shows a grid of labelled tiles;
// a tap gives a haptic buzz and sends `MACRO <id>\n` over USB serial so a Mac
// companion (pc/macropad_listener.py) can perform the action.
#pragma once
#include "view.h"

class MacropadView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  // Visual/press state.
  int      pressedTile = -1;   // tile currently shown "pressed" (-1 = none)
  bool     firstDraw   = true;
  // Theme signature so a Settings->Display change forces a full repaint.
  uint16_t lastBg = 0, lastFg = 0, lastAccent = 0, lastLine = 0;

  // Geometry (computed in onEnter from the screen + grid constants).
  int16_t areaX = 0, areaY = 0, tileW = 0, tileH = 0, gap = 0;

  void tileRect(int idx, int16_t &x, int16_t &y) const;
  int  tileAt(uint16_t px, uint16_t py) const;      // -1 if none
  void drawTile(int idx, bool pressed);
  void drawAllTiles();
};
