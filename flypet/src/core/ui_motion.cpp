#include "ui_motion.h"
#include <math.h>

float easeOutCubic(float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float easeInOutCubic(float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  if (t < 0.5f) return 4.0f * t * t * t;
  float u = -2.0f * t + 2.0f;
  return 1.0f - u * u * u * 0.5f;
}

float easeOutQuint(float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  float u = 1.0f - t;
  float u2 = u * u;
  return 1.0f - u2 * u2 * u;
}

float easeOutBack(float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

float easeOutElastic(float t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  const float c4 = (2.0f * (float)M_PI) / 3.0f;
  return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}
