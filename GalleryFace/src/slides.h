// Painting slideshow: each painting holds, slowly pans if it is taller than
// the screen, then crossfades into the next. Paintings shorter than the
// screen sit centred on black.
//
// Two decode slots in PSRAM. The renderer (core 1) owns the current slot;
// the decoder task (core 0) fills the other slot with the next painting and
// then raises nextReady. The renderer reads the other slot only after
// nextReady, and hands a slot back only by clearing nextReady (after
// updating nextImg), so no locks are needed.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace slides {
constexpr int16_t  W = 240, H = 280;
constexpr uint32_t HOLD_MS = 7000;     // fully shown
constexpr uint32_t FADE_MS = 1500;     // crossfade

extern void *(*allocPixels)(size_t bytes);   // platform hook (PSRAM on the watch)

bool begin(int firstImg, uint32_t nowMs);    // decodes the first painting; false = no RAM
bool decodeStep();                           // decoder task: false = nothing to do
int  render(uint32_t nowMs, uint16_t *fb);   // fill fb (W*H); returns painting to caption
int  count();
const char *title(int i);
uint32_t decodeFailures();

// Test hooks.
bool decodeInto(int img, uint16_t *px);      // decode painting img into W*h pixels
int16_t height(int i);
void view(const uint16_t *px, int16_t h, float pan, uint16_t *fb);   // one painting, no fade
extern bool testBreakBlend;                  // checker control: corrupt the blend
}
