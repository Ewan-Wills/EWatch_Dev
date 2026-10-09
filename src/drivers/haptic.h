// Haptic driver — DRV2603 motor driver controlling an LRA/ERM.
//
// EN pin gates power; PWM on the IN pin sets vibration intensity.
//
// Contract:
//   * hapticBegin() must run before any hapticBuzz call. Spawns a small
//     dedicated FreeRTOS task that owns motor timing — `hapticBuzz` itself
//     is fire-and-forget so it can be called from any task / ISR-safe
//     context without blocking.
//   * Buzz requests are queued (depth 4). If the motor is busy and the
//     queue is full, the new request is silently dropped — UI feedback is
//     not worth backpressure.
//   * The strength multiplier (0..100 %) is applied to every queued buzz.
//     0 silences the motor entirely. Set from the Settings → Haptics page
//     and the web form; mirrored here so the driver doesn't pull in model.h.
#pragma once
#include <stdint.h>

void hapticBegin();
void hapticBuzz(uint8_t intensity, uint16_t duration_ms);
void hapticSetStrengthPct(uint8_t pct);
