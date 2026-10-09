// GUI animation framework.
//
// Retained-mode scene graph + tween system that draws to an Arduino_Canvas.
// Designed to be terse to drive from code:
//
//     UI ui(canvas);
//     auto r = ui.rect(100, 100, 50, 50, RED);
//     ui.animate(r, Prop::X,        200, 1000, Easing::EaseOutQuad);
//     ui.animate(r, Prop::Rotation, 6.283f, 1500);
//     ui.animateColor(r, BLUE, 800);
//     while (running) {
//       ui.tick();    // advance tweens
//       ui.render();  // paint to canvas (caller flushes)
//     }
//
// Animations are fire-and-forget: tick() advances every active tween against
// millis(), so multiple animations on the same or different elements run
// concurrently. Each element's position is its centre (rectangles too) so
// rotation / scale don't pull the shape off-anchor.
//
// Memory profile: fixed-size storage (kMaxElements * sizeof(Element) +
// kMaxTweens * sizeof(Tween)) — no heap churn while animating.
#pragma once
#include <stdint.h>
#include <Arduino_GFX_Library.h>

enum class Shape : uint8_t {
  Rect,
  Circle,
  Triangle,
  Line,
  Text,
};

enum class Prop : uint8_t {
  X, Y,
  W, H,
  Radius,
  Rotation,        // radians
  ScaleX, ScaleY,
  Opacity,         // 0..1
  R, G, B,         // 0..255
};

enum class Easing : uint8_t {
  Linear,
  EaseInQuad,  EaseOutQuad,  EaseInOutQuad,
  EaseInCubic, EaseOutCubic, EaseInOutCubic,
  EaseOutBounce,
  EaseOutBack,
};

using ElementId = uint16_t;
constexpr ElementId kInvalidElement = 0;

// Visual state of a single drawable. All transforms pivot on (x, y) — for
// shapes built from vertex offsets (triangle, line) the offsets stored in
// vx/vy are RELATIVE to (x, y), so animating x/y moves the whole shape and
// animating rotation spins it about its own anchor.
struct Element {
  ElementId id     = kInvalidElement;
  Shape     shape  = Shape::Rect;
  bool      visible = true;

  // Position / size.
  float x = 0, y = 0;         // anchor (centre for rect/circle/text baseline-left)
  float w = 0, h = 0;         // rect width/height (rect only)
  float radius = 0;           // circle radius

  // Transform.
  float rotation = 0;         // radians
  float scaleX = 1, scaleY = 1;

  // Style.
  float opacity = 1.0f;       // 0..1; multiplies toward bgColor at draw time
  uint8_t r = 255, g = 255, b = 255;

  // Triangle (3 verts) or line (2 verts) offsets, relative to (x, y).
  float vx[3] = { 0, 0, 0 };
  float vy[3] = { 0, 0, 0 };

  // Text payload. Pointer must outlive the element (string literals are fine).
  const char *text = nullptr;
  uint8_t     textSize = 1;
};

struct Tween {
  ElementId target = kInvalidElement;
  Prop      prop   = Prop::X;
  float     from = 0.0f, to = 0.0f;
  uint32_t  startMs    = 0;       // absolute millis() at which interpolation begins
  uint32_t  durationMs = 0;
  Easing    easing  = Easing::Linear;
  bool      active  = false;
  bool      started = false;      // false while we're still inside the delay window
};

class UI {
public:
  explicit UI(Arduino_Canvas *canvas);

  void setBackground(uint16_t rgb565);

  // -------- creation --------
  ElementId rect    (float x, float y, float w, float h, uint16_t color);
  ElementId circle  (float cx, float cy, float radius, uint16_t color);
  ElementId triangle(float x1, float y1, float x2, float y2,
                     float x3, float y3, uint16_t color);
  ElementId line    (float x1, float y1, float x2, float y2, uint16_t color);
  ElementId text    (float x, float y, const char *str, uint8_t size, uint16_t color);

  // -------- removal --------
  void remove(ElementId id);
  void removeAll();

  // -------- direct setters (no tween) --------
  void setPos     (ElementId id, float x, float y);
  void setX       (ElementId id, float x);
  void setY       (ElementId id, float y);
  void setSize    (ElementId id, float w, float h);
  void setRadius  (ElementId id, float r);
  void setRotation(ElementId id, float radians);
  void setScale   (ElementId id, float sx, float sy);
  void setOpacity (ElementId id, float a01);
  void setColor   (ElementId id, uint16_t rgb565);
  void setColor   (ElementId id, uint8_t r, uint8_t g, uint8_t b);
  void setVisible (ElementId id, bool v);

  // -------- read-back --------
  Element  *get(ElementId id);
  ElementId hitTest(int16_t px, int16_t py);

  // -------- animation --------
  void animate(ElementId id, Prop prop, float to,
               uint32_t durationMs,
               Easing easing  = Easing::Linear,
               uint32_t delayMs = 0);
  void animateColor(ElementId id, uint16_t toRgb565,
                    uint32_t durationMs,
                    Easing easing  = Easing::Linear,
                    uint32_t delayMs = 0);
  void cancelAnimations(ElementId id);
  void cancelAllAnimations();
  bool isAnimating() const;

  // -------- frame --------
  void tick();        // advance tweens against millis()
  void render();      // paint into canvas (no flush)

private:
  static constexpr int kMaxElements = 24;
  static constexpr int kMaxTweens   = 64;

  Arduino_Canvas *canvas;
  uint16_t bgColor = 0x0000;
  Element  elements[kMaxElements];
  Tween    tweens[kMaxTweens];
  uint16_t nextId = 1;

  Element *findElement(ElementId id);
  int      slotForNewElement();
  int      slotForNewTween();
  void     applyProperty(Element &e, Prop p, float v);
  float    readProperty(const Element &e, Prop p) const;
  uint16_t blendColor(const Element &e) const;
  void     drawElement(const Element &e);

  static float ease(Easing kind, float t);
};
