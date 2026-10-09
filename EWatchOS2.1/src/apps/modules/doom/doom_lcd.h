// Doom: GDMA pixel streaming to the display (doom_lcd.cpp).
#pragma once
#include <stdint.h>

bool doomLcdDmaBegin();                               // false -> use the CPU path
void doomLcdDmaEnd();
bool doomLcdDmaSend(const void *beRgb565, uint32_t bytes);   // after startWrite + writeAddrWindow
bool doomLcdDmaWait();                                // before endWrite / reusing the buffer
