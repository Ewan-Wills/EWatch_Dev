// The turning CT head: plays the pre-rendered frames (src/frames_data.h) at a
// fixed rate. Four turns - skin, skull, brain, ventricles - with a scan-plane
// wipe into the next layer baked into the last part of each turn.
//
// Two decode slots in PSRAM. The renderer (core 1) owns the current slot; the
// decoder task (core 0) fills the other slot with the next frame and then
// raises nextReady. The renderer reads the other slot only after nextReady,
// and changes nextFrame / hands the slot back only by clearing nextReady, so
// no locks are needed (same scheme as GalleryFace's slides).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace head {
constexpr int16_t  W = 240, H = 280;
constexpr uint32_t FRAME_MS = 50;              // 20 fps: one turn = 96 frames = 4.8 s

extern void *(*allocPixels)(size_t bytes);     // platform hook (PSRAM on the watch)

bool begin(int firstFrame, uint32_t nowMs);    // decodes the first frame; false = no RAM
bool decodeStep();                             // decoder task: false = nothing to do
int  tick(uint32_t nowMs);                     // advance to the frame due now; returns its index
void copyCurrent(uint16_t *fb);                // copy the current frame into fb (W*H)
int  render(uint32_t nowMs, uint16_t *fb);     // tick + copyCurrent
void skipToLayer(int delta);                   // jump to the start of the next/previous layer's turn
int  count();
int  layerOf(int frame);                       // layer shown (0 skin .. 3 ventricles)
const char *layerName(int layer);
uint32_t decodeFailures();
uint32_t lateFrames();                         // frames shown late because the decoder lagged

// Test hooks.
bool decodeInto(int frame, uint16_t *px);
}
