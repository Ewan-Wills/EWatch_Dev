// EWatchOS icon set — parametric vector glyphs drawn with GFX primitives.
//
// Every launcher row / control-centre tile / face chip pulls from this one
// table so the whole UI shares a single visual language. Icons are drawn
// centered on (cx, cy) inside a nominal box of `s` px (roughly the glyph
// height); strokes scale with s so the same icon works at 20 px and 60 px.
#pragma once
#include <Arduino_GFX_Library.h>

enum class Icon : uint8_t {
  Stopwatch, Timer, Music, QR, Photo, Cube, Sparkle, Bird,
  Skull, Tunnel, Ship, Car, Paw, Island, Frog,
  Gear, Wrench, Info, Power, Moon, Sun, Torch, Wifi,
  Play, Pause, Next, Prev, Check, Cross, Clock, Vibrate, Memory, Bluetooth,
  Pen,
  Book,
};

// Draw the glyph. `color` is the main stroke; `bg` is used by the few icons
// that punch holes (pass the surface color they sit on).
void drawIcon(Arduino_GFX *d, Icon ic, int16_t cx, int16_t cy, int16_t s,
              uint16_t color, uint16_t bg);

// The squircle app tile: rounded box in `tile` color + glyph on top.
void drawIconTile(Arduino_GFX *d, Icon ic, int16_t x, int16_t y, int16_t box,
                  uint16_t tile, uint16_t glyph);
