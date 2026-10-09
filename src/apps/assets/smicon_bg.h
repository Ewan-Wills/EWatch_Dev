// SM-logo watermark background, 240x280 RGB565. The pixel data lives in
// smicon_bg_data.inc (generated from SMIcon.svg) and is compiled exactly once
// by smicon_bg_data.cpp — everyone else links against this extern so the
// 131 KiB array isn't duplicated per translation unit.
#pragma once
#include <stdint.h>

#define SMICON_BG_W 240
#define SMICON_BG_H 280

extern const uint16_t SMICON_BG[SMICON_BG_W * SMICON_BG_H];
