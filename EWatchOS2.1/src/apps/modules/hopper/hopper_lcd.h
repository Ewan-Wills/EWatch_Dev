// Hopper: GDMA pixel streaming to the display (hopper_lcd.cpp).
#pragma once
#include <stdint.h>

bool hopLcdDmaBegin();                               // false -> use the CPU path
void hopLcdDmaEnd();
bool hopLcdDmaSend(const void *beRgb565, uint32_t bytes);   // after startWrite + writeAddrWindow
bool hopLcdDmaWait();                                // before endWrite / reusing the buffer
