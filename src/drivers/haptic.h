// DRV2603 haptic driver. EN pin gates power; PWM signal on the IN pin
// drives the LRA/ERM. Brief buzz on boot serves as an "I'm alive" cue.
#pragma once
#include <stdint.h>

void hapticBegin();
void hapticBuzz(uint8_t intensity, uint16_t duration_ms);

// Global strength multiplier applied to every hapticBuzz call. 0 = off,
// 100 = full motor. Set from settings (web + on-watch); the model field
// `hapticStrength` is mirrored here so the driver doesn't pull in model.h.
void hapticSetStrengthPct(uint8_t pct);
