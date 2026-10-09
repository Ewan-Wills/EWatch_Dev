// EWatchOS motion kit — easing curves, frame-time tweens, and touch scroll
// physics (momentum fling + rubber-band overscroll + snap).
//
// Everything here is pure math over millis(); no drawing, no tasks. Views use
// these from their own render loops (the carousel-style "block while
// animating, drain events at ~33 ms" pattern).
#pragma once
#include <Arduino.h>

// ---------- easing ----------
float easeOutCubic(float t);          // fast start, gentle settle (default)
float easeInOutCubic(float t);
float easeOutQuint(float t);          // extra-snappy settle
float easeOutBack(float t);           // small overshoot — "pop"
float easeOutElastic(float t);        // springy — use sparingly

// ---------- one-shot tween ----------
// Value animator: start(from, to, ms, easing). value() samples against
// millis(); done() when finished. Cheap enough to keep several per view.
struct Tween {
  float    from = 0, to = 0;
  uint32_t t0 = 0, dur = 0;
  float  (*ease)(float) = easeOutCubic;

  void start(float f, float t, uint32_t ms, float (*e)(float) = easeOutCubic) {
    from = f; to = t; dur = ms; ease = e; t0 = millis();
  }
  void jump(float v) { from = to = v; dur = 0; }
  bool done() const { return dur == 0 || millis() - t0 >= dur; }
  float value() const {
    if (done()) return to;
    float p = (float)(millis() - t0) / (float)dur;
    return from + (to - from) * ease(p);
  }
};

// ---------- scroll physics ----------
// 1-D scroll position with momentum and rubber-band edges, in content pixels.
//   grab(y)    on Touch          — finger down, kill momentum
//   drag(y)    on TouchHold      — track the finger (with edge resistance)
//   release()  on TouchUp        — start the fling / snap-back
//   step()     once per frame    — advance physics; true while still moving
// pos() is the scroll offset (0 = top). maxScroll is set by the view.
struct ScrollPhys {
  float pos = 0;            // current offset, px
  float maxScroll = 0;      // content height - viewport height (>= 0)
  float velocity = 0;       // px/s (fling)
  bool  dragging = false;

  // Tunables — chosen to feel like a small round-screen device, verified by
  // eye at 30 fps: ~1.6 s glide from a hard fling, firm edge resistance.
  static constexpr float kFriction   = 2.3f;    // exponential decay, 1/s
  static constexpr float kMinFling   = 40.0f;   // px/s — below this, stop
  static constexpr float kEdgeSpring = 14.0f;   // snap-back stiffness, 1/s
  static constexpr float kEdgeResist = 0.45f;   // finger drag past the edge

  void grab(int16_t y) {
    dragging = true; velocity = 0;
    lastY = y; lastMs = millis();
  }
  void drag(int16_t y) {
    if (!dragging) { grab(y); return; }
    uint32_t now = millis();
    float dy = (float)(lastY - y);              // finger up = content up
    // Past an edge the finger only moves the content a fraction — the
    // "stretch" that tells the user they've hit the end.
    if (pos < 0 || pos > maxScroll) dy *= kEdgeResist;
    pos += dy;
    uint32_t dt = now - lastMs;
    if (dt > 0) {
      float v = dy * 1000.0f / (float)dt;
      velocity = 0.5f * velocity + 0.5f * v;    // smoothed sample
    }
    lastY = y; lastMs = now;
  }
  void release() { dragging = false; }

  // Advance one frame. Returns true while motion continues (keep animating).
  bool step() {
    uint32_t now = millis();
    float dt = (now - stepMs) / 1000.0f;
    stepMs = now;
    if (dt <= 0 || dt > 0.25f) dt = 0.033f;
    if (dragging) return true;

    bool moving = false;
    if (pos < 0 || pos > maxScroll) {
      // Rubber-band spring back to the nearest edge; kill any fling.
      float target = (pos < 0) ? 0 : maxScroll;
      float d = target - pos;
      pos += d * fminf(kEdgeSpring * dt, 1.0f);
      velocity = 0;
      if (fabsf(target - pos) < 0.5f) pos = target; else moving = true;
    } else if (fabsf(velocity) > kMinFling) {
      pos += velocity * dt;
      velocity *= expf(-kFriction * dt);
      moving = true;
    } else {
      velocity = 0;
    }
    return moving;
  }

  bool moving() const {
    return dragging || fabsf(velocity) > kMinFling ||
           pos < -0.5f || pos > maxScroll + 0.5f;
  }

private:
  int16_t  lastY = 0;
  uint32_t lastMs = 0, stepMs = 0;
};
