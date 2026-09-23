// Claude Face — watch-face glue for AuroraOS (the "Claude" entry in
// watchface.cpp's kFaces[]). Pure drawing lives in claude_face_draw.*, the
// USB link in cf_link.*, the policy maths in cf_policy.h; this module ties
// them to the render task:
//
//   * claudeFaceTick()  — called by taskRender every loop, on EVERY screen:
//                         consumes the link snapshot, runs the haptic state
//                         machine (the watch is a notifier), and applies the
//                         power hold + backlight policy while the face shows.
//   * claudeFace*()     — WatchFaceView delegates to these while the Claude
//                         face is the selected face: dirty-region rendering
//                         through the shared PSRAM frame canvas, paintTo for
//                         slide transitions, gestures, long-press detail page.
//
// All functions run on the render task unless noted.
#pragma once
#include <stdint.h>
#include "event.h"

class Arduino_Canvas;
class Stream;

// Index of "Claude" in watchface.cpp kFaces[] (appended LAST so the stored
// indices of the existing faces never shift). storage.cpp uses it for the
// one-time default migration ("cfDefault" NVS flag).
#define CF_FACE_INDEX 5

void     claudeFaceTick();
void     claudeFaceOnEnter();
void     claudeFaceOnExit();
void     claudeFaceRender(Arduino_Canvas *cv);   // nullptr = direct-to-gfx fallback
bool     claudeFacePaintTo(Arduino_Canvas *cv);  // full frame, no flush
bool     claudeFaceOnEvent(const Event &e);      // true = consumed
uint16_t claudeFaceFrameMs();                    // View::desiredFrameMs() answer

// Console helpers (ewlog console task; reads counters without locking —
// diagnostic output only).
void claudeFaceStatsPrint(Stream &out);
bool claudeFaceRequestTestBuzz(const char *which);   // blocked|done|cross|reset
