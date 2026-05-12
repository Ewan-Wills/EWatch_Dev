// CST816S touchscreen controller. Shares the main I2C bus with the RTC and
// accelerometer. Sets touchPending = true from an ISR; main loop drains it.
#pragma once
#include <CST816S.h>

extern CST816S touchpad;
extern volatile bool touchPending;

void touchBegin();
