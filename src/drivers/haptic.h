// DRV2603 haptic driver. EN pin gates power; PWM signal on the IN pin
// drives the LRA/ERM. Brief buzz on boot serves as an "I'm alive" cue.
#pragma once
#include <stdint.h>

void hapticBegin();
void hapticBuzz(uint8_t intensity, uint16_t duration_ms);
