// Claude Face — the face itself, for the stand-alone firmware. Pure drawing
// lives in claude_face_draw.*, the USB link in cf_link.*, the policy maths
// in cf_policy.h; this module ties them to the UI loop (main.cpp):
//
//   * claudeFaceTick()   — every UI-loop pass: consumes the link snapshot,
//                          runs the haptic state machine, applies the
//                          backlight policy and renders (dirty regions
//                          through the full-frame canvas, landscape).
//   * claudeFace<Input>  — touch / gesture / button / motion, in NATIVE panel
//                          terms; converted to the landscape viewer's frame
//                          here (gesture table in claude_face.cpp).
//
// All functions run on the UI loop unless noted.
#pragma once
#include <stdint.h>

class Arduino_Canvas;
class Stream;

// What the UI loop knows about the world (RTC + battery), per tick.
struct CFEnv {
  bool     rtcOk;
  uint16_t year;
  uint8_t  month, day, hour, minute, second;   // RTC local wall time
  bool     batOk;
  uint8_t  batPct;
};

void claudeFaceBegin(Arduino_Canvas *cv);   // nullptr = draw straight to the panel
void claudeFaceTick(const CFEnv &env);

// Input (native panel coordinates; chip gesture ids as in cf_policy.h).
void claudeFaceTouchDown(uint16_t x, uint16_t y);
void claudeFaceTouchHold(uint16_t x, uint16_t y);
void claudeFaceTouchUp(uint16_t x, uint16_t y);
void claudeFaceGesture(uint8_t chipGesture);
void claudeFaceButtonShort();
void claudeFaceWake();                      // wrist raise / button down: 100 % for 10 s

// Console helpers (link task; read counters without locking — diagnostic
// output only).
void claudeFaceStatsPrint(Stream &out);
bool claudeFaceRequestTestBuzz(const char *which);   // blocked|done|cross|reset
