// Write app: small CNN character classifier (A-Z case-merged + 0-9).
// The strokes are drawn into a 28x28 image (rasterise) and classified by a
// 3-conv + 2-dense network with int8 weights (write_cnn_weights.h, generated
// by EWatch/ml-write/train.py from EMNIST + UJIpenchars2). See NOTES.md.
//
// Pure C++ (no Arduino), so the firmware, ewsim_host and the host bench all
// build the same file. rasterise() must stay identical to common.py.
#pragma once
#include <stdint.h>
#include "write_recog.h"

namespace wcnn {

constexpr int kG = 28;
constexpr int kClasses = 36;

bool init();                                   // scratch buffers + float weights (idempotent)
// Raw screen points (stroke ids) -> img[kG*kG] in [0,1]. false if empty.
bool rasterise(const wrec::RawPt *pts, int n, float *img);
// img -> softmax probabilities per class (kClasses). false if not initialised.
bool infer(const float *img, float *prob);
char classChar(int i);
// Convenience: top-k distinct classes, best first. score = probability.
int  classify(const wrec::RawPt *pts, int n, wrec::Cand *out, int maxOut);
// Time of the last classify() per stage (us): raster, conv1-3, dense.
extern uint32_t gStageUs[6];

}  // namespace wcnn
