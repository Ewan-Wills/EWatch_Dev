// EWatchOS design system — the visual vocabulary every system screen shares.
//
// Dark-first, Apple-Watch-inspired: near-black ground, white type, one vivid
// accent, content on soft "card" surfaces with big corner radii. All colors
// are RGB565. The user's theme (model.bgColor/fgColor/accentColor) still
// wins where set — auraTheme() merges it over these defaults.
//
// Layout constants assume the 240x280 portrait panel.
#pragma once
#include <Arduino_GFX_Library.h>
#include "view.h"        // ThemeColors / theme()

// ---------- palette ----------
namespace aura {

// Ground + surfaces (fixed; the user's bgColor replaces kBg when non-black).
constexpr uint16_t kBg        = 0x0000;                        // true black (OLED-ish)
constexpr uint16_t kCard      = 0x18E3;                        // #1C1C1E charcoal
constexpr uint16_t kCardHi    = 0x2965;                        // pressed / focused card
constexpr uint16_t kLine      = 0x39C7;                        // hairlines
constexpr uint16_t kTextDim   = 0x94B2;                        // secondary text
constexpr uint16_t kText      = 0xFFFF;                        // primary text

// Signal colors.
constexpr uint16_t kBlue      = 0x04BF;   // #0A84FF default accent
constexpr uint16_t kGreen     = 0x3666;   // #30D158 success / battery ok
constexpr uint16_t kOrange    = 0xFCC2;   // #FF9F0A warning / charging
constexpr uint16_t kRed       = 0xFA8A;   // #FF453A danger / low battery
constexpr uint16_t kTeal      = 0x3E9C;   // cyan-teal
constexpr uint16_t kPurple    = 0xBA7F;   // #BF5AF2
constexpr uint16_t kPink      = 0xFB58;   // #FF6482
constexpr uint16_t kYellow    = 0xFEA6;   // #FFD60A

// Metrics.
constexpr int16_t  W          = 240;
constexpr int16_t  H          = 280;
constexpr int16_t  kGutter    = 12;      // outer margin
constexpr int16_t  kRadius    = 18;      // card corner radius
constexpr int16_t  kRowH      = 64;      // launcher / form row height
constexpr int16_t  kRowGap    = 10;      // vertical gap between rows

}  // namespace aura

// Effective theme for system screens: user's colors where they've customized,
// Aurora defaults elsewhere. (A user still on the old navy default gets the
// Aurora blue accent instead.)
struct AuraTheme {
  uint16_t bg, card, cardHi, line, text, textDim, accent;
};
AuraTheme auraTheme();

// ---------- shared drawing helpers (implemented in ui_style.cpp) ----------
// All take the destination explicitly so they work on the frame canvas and
// (rarely) directly on gfx.

// Filled rounded-rect card. r clamped to h/2.
void auraCard(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
              int16_t r, uint16_t color);

// Pill (fully-rounded capsule) with centered bitmap-font text.
void auraPill(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
              uint16_t bg, uint16_t fg, const char *label, uint8_t textSize = 1);

// Centered text helpers (bitmap font).
void auraTextCentered(Arduino_GFX *d, int16_t cx, int16_t y, const char *s,
                      uint8_t size, uint16_t color);
int16_t auraTextWidth(const char *s, uint8_t size);

// FreeFont text centered on cx with baseline y (uses the given font).
void auraFontCentered(Arduino_GFX *d, const GFXfont *f, int16_t cx, int16_t baselineY,
                      const char *s, uint16_t color);
int16_t auraFontWidth(Arduino_GFX *d, const GFXfont *f, const char *s);

// Small round "chip" dot + label row (status chips on the watch face).
void auraChip(Arduino_GFX *d, int16_t x, int16_t y, uint16_t dotColor,
              const char *label, uint16_t textColor);

// Battery indicator: small arc-style ring with % fill color-coded.
void auraBatteryRing(Arduino_GFX *d, int16_t cx, int16_t cy, int16_t r,
                     uint8_t pct, bool low);

// Toggle switch (iOS style), w x h ~ 44x26. on=accent track, knob slides.
// t = 0..1 animation position of the knob (0=off, 1=on).
void auraToggle(Arduino_GFX *d, int16_t x, int16_t y, float t, uint16_t accent);

// Horizontal slider track + filled portion + knob. val01 in 0..1.
void auraSlider(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, float val01,
                uint16_t accent);

// Back chevron affordance (top-left) — subtler than the old block button.
void auraBackChevron(Arduino_GFX *d, uint16_t color);
bool auraTappedBack(uint16_t x, uint16_t y);

// Vertical scrollbar hint (right edge) while a list scrolls.
void auraScrollbar(Arduino_GFX *d, float pos, float maxScroll, int16_t viewH);
