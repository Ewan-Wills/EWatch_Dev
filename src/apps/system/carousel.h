// Carousel — one big tile (~3/4 of the screen) shown at a time, with eased
// vertical scroll between selections, an animated entrance, a press pulse
// on tap, and live page-indicator dots. Uses the shared frame canvas so
// transitions stay flicker-free. The render() loop blocks while animation
// is in flight (same pattern the video player uses) and returns when settled
// so the controller's auto-sleep timer still works.
#pragma once
#include <Arduino_GFX_Library.h>
#include "view.h"

struct AppEntry {
  const char *name;
  uint16_t    color;      // legacy field — tiles paint in the theme accent
  Screen      target;
};

class CarouselView : public View {
public:
  CarouselView(const AppEntry *entries, int n,
               const char *title, Screen back, bool wrap = false)
    : entries(entries), N(n), title(title), backScreen(back), wrapAround(wrap) {}

  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;

private:
  const AppEntry *entries;
  const int       N;
  const char     *title;
  const Screen    backScreen;
  const bool      wrapAround;

  Arduino_Canvas *canvas = nullptr;
  int      cur = 0;
  float    scrollPos = 0.f;
  float    scrollFrom = 0.f, scrollTo = 0.f;
  uint32_t scrollStart = 0;
  uint32_t scrollDuration = 350;

  uint32_t pulseStart = 0;
  static const uint32_t kPulseMs = 220;

  uint32_t exitAtMs = 0;
  Screen   exitTarget = Screen::Watch;

  bool     pressActive = false;
  bool     pressMoved  = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressTime = 0;
  static const int kMoveThreshSq = 16 * 16;

  // Tile sizing: ~3/4 of the screen height for the active card.
  static const int16_t TILE_W   = 200;
  static const int16_t TILE_H   = 210;       // 280 * 0.75 ≈ 210
  static const int16_t TILE_CY  = 152;       // center of stage
  static const int16_t STRIDE   = TILE_H + 24;

  bool  isAnimating() const;
  static float easeOutCubic(float t);
  void  advanceScrollTween();
  float currentPulseScale();
  void  startScroll(int newIndex);
  void  drawFrame();
  void  drawChrome(const ThemeColors &t);
  void  drawTiles(const ThemeColors &t);
  void  drawIndicator(const ThemeColors &t);
  void  handleEvent(const Event &e);
  bool  hitCurrentTile(int x, int y);
};
