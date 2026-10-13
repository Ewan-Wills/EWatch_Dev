// Draw module: hooks the OS calls (all guarded by EW_APP_DRAW at the call site).
#pragma once
#include <stdint.h>

// power_mgr: keep the screen on while someone is drawing (last input < 60 s).
bool drawKeepAwake();

// ewlog console (tests): `draw state` -> "DRAWSTATE ops=.. pts=.. undo=.. ..."
bool drawConsole(const char *line);

// FNV-1a over RGB565 pixels (h = (h ^ px) * 16777619, start 2166136261).
// Logged for the drawing area after every edit and with every send, so a
// test can prove the sent pixels are the picture on screen.
uint32_t drawChecksum(const uint16_t *px, uint32_t n);
