// Canned synthetic IMU traces for `motion inject` (see tools/motionlm/export.py).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int16_t label, variant; int32_t offset, count; } MotionTraceRef;
extern const int16_t        kMotionTraceSamples[][3];
extern const MotionTraceRef kMotionTraces[];
extern const int            kMotionTraceCount;
#ifdef __cplusplus
}
#endif
