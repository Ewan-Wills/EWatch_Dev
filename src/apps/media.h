// Media viewer app. Lists images and videos baked into the firmware as
// PROGMEM RGB565 arrays, plays the selected one. The chosen storage format
// is the native panel format (RGB565) so no decode work is needed on the
// MCU — Arduino_GFX streams pixels straight from flash to the display.
//
// Generate assets with tools/encode_media.py and register them in
// src/apps/assets/manifest.h.
#pragma once
#include <stdint.h>
#include "view.h"

enum class MediaKind : uint8_t {
  Image,
  Video,
  BuiltinSample,    // procedural test pattern, no asset data needed
};

struct MediaAsset {
  const char     *name;
  MediaKind       kind;
  int16_t         w;
  int16_t         h;
  uint16_t        fps;          // 0 for image / sample
  uint16_t        frames;       // 1 for image / sample
  const uint16_t *pixels;       // PROGMEM, frames * w * h entries (RGB565)
};

// Full-screen swipe carousel: swipe up/down to move between assets, exit only
// on the physical button or a back-swipe (never a plain tap). render() blocks
// and drains its own events, so the watch also won't auto-sleep while open.
class MediaView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &) override {}   // render() drains its own events

private:
  int index = 0;
};
