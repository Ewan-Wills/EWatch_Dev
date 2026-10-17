// Haptic Lab (Apps -> System -> Haptic Lab): every effect in the haptic library as
// a tile. Tap a tile to feel it. Four pages of six; swipe up/down (or tap the
// page dots) to change page. Each tile draws the effect's drive waveform:
// above the line = forward drive, below = brake.
#include <Arduino.h>
#include "display.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "system_views.h"
#include "ewlog.h"

namespace {

struct Page { const char *title; uint16_t color; Haptic fx[6]; };

const Page kPages[] = {
  { "Crisp",    aura::kBlue,
    { Haptic::Tick, Haptic::Click, Haptic::DoubleClick, Haptic::Snap, Haptic::Thud, Haptic::Bump } },
  { "Shaped",   aura::kTeal,
    { Haptic::Swell, Haptic::Fade, Haptic::Wave, Haptic::Buzz, Haptic::Purr, Haptic::Rumble } },
  { "Rhythm",   aura::kPurple,
    { Haptic::Engine, Haptic::Heartbeat, Haptic::Knock, Haptic::Gallop, Haptic::LongPress, Haptic::ToggleOn } },
  { "Messages", aura::kOrange,
    { Haptic::ToggleOff, Haptic::Success, Haptic::Error, Haptic::Warning, Haptic::Notify, Haptic::Alarm } },
};
constexpr int kPageCount = sizeof(kPages) / sizeof(kPages[0]);

constexpr int16_t TILE_W = 106, TILE_H = 58, GAP = 6;
constexpr int16_t GRID_X = (240 - 2 * TILE_W - GAP) / 2, GRID_Y = 56;
constexpr int16_t DOTS_Y = 262;

uint32_t effectMs(const HapticEffect &e) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < e.count; i++) t += e.steps[i].ms;
  return t;
}

class HapticLabView : public View {
public:
  void onEnter() override { uiClearAll(); full = true; lit = unlit = -1; }

  uint16_t desiredFrameMs() const override { return lit >= 0 ? 40 : 0; }

  void render() override {
    if (!gfx) return;
    if (full) {
      uiClearAll();
      drawTitleBar("Haptic Lab");
      for (int i = 0; i < 6; i++) drawTile(i, false);
      drawDots();
      full = false;
    }
    if (unlit >= 0) { drawTile(unlit, false); unlit = -1; }
    if (lit >= 0 && !litDrawn) { drawTile(lit, true); litDrawn = true; }
    if (lit >= 0 && millis() >= litUntil) { int i = lit; lit = -1; drawTile(i, false); }
  }

  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort ||
        (e.type == EventType::Touch && tappedBack(e.x, e.y))) {
      hapticStop();
      switchTo(Screen::SystemApps);
      return;
    }
    if (e.type == EventType::Gesture) {
      if (e.gesture == Gesture::SwipeUp)   { turn(+1); return; }
      if (e.gesture == Gesture::SwipeDown) { turn(-1); return; }
      return;
    }
    if (e.type != EventType::Touch) return;
    if (e.y >= DOTS_Y - 12) { turn(e.x < 120 ? -1 : +1); return; }
    for (int i = 0; i < 6; i++) {
      int16_t x, y; tileXY(i, x, y);
      if (!uiInRect(e.x, e.y, x, y, TILE_W, TILE_H)) continue;
      const HapticEffect &fx = hapticEffect(kPages[page].fx[i]);
      hapticStop();                 // the tapped effect plays now, not queued
      hapticPlay(fx);
      EWLOGI("HLAB", "play %s", fx.name);
      if (lit >= 0 && lit != i) unlit = lit;
      lit = i;
      litDrawn = false;
      litUntil = millis() + max<uint32_t>(effectMs(fx) + 30, 250);
      return;
    }
  }

private:
  int      page = 0;
  bool     full = true;
  int      lit = -1, unlit = -1;   // tile lit while its effect plays
  bool     litDrawn = false;
  uint32_t litUntil = 0;

  void turn(int d) {
    page = (page + d + kPageCount) % kPageCount;
    lit = unlit = -1;
    full = true;
    hapticPlay(Haptic::Tick);
  }

  static void tileXY(int i, int16_t &x, int16_t &y) {
    x = GRID_X + (i % 2) * (TILE_W + GAP);
    y = GRID_Y + (i / 2) * (TILE_H + GAP);
  }

  void drawTile(int i, bool on) {
    ThemeColors th = theme();
    const Page &p = kPages[page];
    const HapticEffect &fx = hapticEffect(p.fx[i]);
    int16_t x, y; tileXY(i, x, y);
    uint16_t bg = on ? p.color : th.bg;
    uint16_t fg = on ? contrastFor(p.color) : th.fg;
    gfx->fillRoundRect(x, y, TILE_W, TILE_H, 10, bg);
    gfx->drawRoundRect(x, y, TILE_W, TILE_H, 10, p.color);

    uint8_t size = strlen(fx.name) <= 8 ? 2 : 1;
    int16_t tw = (int16_t)strlen(fx.name) * 6 * size;
    gfx->setFont(nullptr);
    gfx->setTextSize(size);
    gfx->setTextColor(fg, bg);
    gfx->setCursor(x + (TILE_W - tw) / 2, y + (size == 2 ? 8 : 12));
    gfx->print(fx.name);

    // Waveform: level over time, baseline 2/3 down the plot (brakes are short).
    const int16_t px = x + 8, pw = TILE_W - 16, py = y + 30, ph = 24;
    const int16_t base = py + ph * 2 / 3;
    uint16_t wave = on ? fg : p.color;
    gfx->drawFastHLine(px, base, pw, on ? fg : th.line);
    uint32_t total = effectMs(fx);
    if (!total) return;
    uint32_t t = 0;
    int16_t lastX = px, lastY = base;
    for (uint8_t s = 0; s < fx.count; s++) {
      const HapticStep &st = fx.steps[s];
      auto yOf = [&](int lvl) {
        return (int16_t)(lvl >= 0 ? base - lvl * (ph * 2 / 3) / 127
                                  : base - lvl * (ph / 3) / 127);
      };
      int16_t x0 = px + (int16_t)(t * pw / total);
      t += st.ms;
      int16_t x1 = px + (int16_t)(t * pw / total);
      int16_t y0 = yOf(st.from), y1 = yOf(st.to);
      gfx->drawLine(lastX, lastY, x0, y0, wave);   // vertical edge
      gfx->drawLine(x0, y0, x1, y1, wave);
      lastX = x1; lastY = y1;
    }
    gfx->drawLine(lastX, lastY, lastX, base, wave);
  }

  void drawDots() {
    ThemeColors th = theme();
    gfx->fillRect(0, DOTS_Y - 12, 240, 280 - (DOTS_Y - 12), th.bg);
    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextColor(kPages[page].color, th.bg);
    const char *t = kPages[page].title;
    gfx->setCursor(20, DOTS_Y - 3);
    gfx->print(t);
    for (int i = 0; i < kPageCount; i++) {
      int16_t cx = 240 - 20 - (kPageCount - 1 - i) * 14;
      if (i == page) gfx->fillCircle(cx, DOTS_Y, 4, kPages[page].color);
      else           gfx->drawCircle(cx, DOTS_Y, 4, th.line);
    }
  }
};

HapticLabView sLab;

}  // namespace

View *hapticLabViewPtr() { return &sLab; }
