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

class MediaView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  enum class Mode : uint8_t { List, Player };

  Mode mode = Mode::List;
  bool needsRedraw = true;
  int  selectedIdx = -1;
  int  top = 0;

  // Tap-vs-swipe disambiguation for the list.
  bool     pressActive   = false;
  bool     pressConsumed = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressTime = 0;
  int      pressSlot = -1;

  void renderList();
  void play(const MediaAsset &a);
  void drawImage(const MediaAsset &a);
  void drawBuiltinSample();
  bool playVideo(const MediaAsset &a);     // returns false if user backed out
  void backToList();
};
