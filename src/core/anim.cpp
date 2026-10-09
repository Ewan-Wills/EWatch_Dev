#include "anim.h"
#include <Arduino.h>
#include <math.h>

UI::UI(Arduino_Canvas *c) : canvas(c) {
  // Zero-init slots — id==kInvalidElement is the "free" marker.
  for (auto &e : elements) e.id = kInvalidElement;
  for (auto &t : tweens)   t.active = false;
}

void UI::setBackground(uint16_t rgb) { bgColor = rgb; }

// ---------- slot management ----------
Element *UI::findElement(ElementId id) {
  if (id == kInvalidElement) return nullptr;
  for (auto &e : elements) if (e.id == id) return &e;
  return nullptr;
}
int UI::slotForNewElement() {
  for (int i = 0; i < kMaxElements; i++) {
    if (elements[i].id == kInvalidElement) return i;
  }
  return -1;
}
int UI::slotForNewTween() {
  for (int i = 0; i < kMaxTweens; i++) {
    if (!tweens[i].active) return i;
  }
  return -1;
}

// ---------- creation ----------
static void rgb565ToRgb(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = (uint8_t)(((c >> 11) & 0x1F) << 3);
  g = (uint8_t)(((c >>  5) & 0x3F) << 2);
  b = (uint8_t)(( c        & 0x1F) << 3);
}

ElementId UI::rect(float x, float y, float w, float h, uint16_t color) {
  int s = slotForNewElement();
  if (s < 0) return kInvalidElement;
  Element &e = elements[s];
  e = Element{};
  e.id = nextId++;
  e.shape = Shape::Rect;
  e.x = x; e.y = y; e.w = w; e.h = h;
  rgb565ToRgb(color, e.r, e.g, e.b);
  return e.id;
}

ElementId UI::circle(float cx, float cy, float radius, uint16_t color) {
  int s = slotForNewElement();
  if (s < 0) return kInvalidElement;
  Element &e = elements[s];
  e = Element{};
  e.id = nextId++;
  e.shape = Shape::Circle;
  e.x = cx; e.y = cy; e.radius = radius;
  rgb565ToRgb(color, e.r, e.g, e.b);
  return e.id;
}

ElementId UI::triangle(float x1, float y1, float x2, float y2,
                       float x3, float y3, uint16_t color) {
  int s = slotForNewElement();
  if (s < 0) return kInvalidElement;
  Element &e = elements[s];
  e = Element{};
  e.id = nextId++;
  e.shape = Shape::Triangle;
  // Anchor at the centroid so rotation looks natural.
  float cx = (x1 + x2 + x3) / 3.0f;
  float cy = (y1 + y2 + y3) / 3.0f;
  e.x = cx; e.y = cy;
  e.vx[0] = x1 - cx; e.vy[0] = y1 - cy;
  e.vx[1] = x2 - cx; e.vy[1] = y2 - cy;
  e.vx[2] = x3 - cx; e.vy[2] = y3 - cy;
  rgb565ToRgb(color, e.r, e.g, e.b);
  return e.id;
}

ElementId UI::line(float x1, float y1, float x2, float y2, uint16_t color) {
  int s = slotForNewElement();
  if (s < 0) return kInvalidElement;
  Element &e = elements[s];
  e = Element{};
  e.id = nextId++;
  e.shape = Shape::Line;
  float cx = (x1 + x2) / 2.0f;
  float cy = (y1 + y2) / 2.0f;
  e.x = cx; e.y = cy;
  e.vx[0] = x1 - cx; e.vy[0] = y1 - cy;
  e.vx[1] = x2 - cx; e.vy[1] = y2 - cy;
  rgb565ToRgb(color, e.r, e.g, e.b);
  return e.id;
}

ElementId UI::text(float x, float y, const char *str, uint8_t size, uint16_t color) {
  int s = slotForNewElement();
  if (s < 0) return kInvalidElement;
  Element &e = elements[s];
  e = Element{};
  e.id = nextId++;
  e.shape = Shape::Text;
  e.x = x; e.y = y;
  e.text = str;
  e.textSize = size;
  rgb565ToRgb(color, e.r, e.g, e.b);
  return e.id;
}

// ---------- removal ----------
void UI::remove(ElementId id) {
  Element *e = findElement(id);
  if (!e) return;
  // Cancel any tween targeting this element.
  for (auto &t : tweens) if (t.target == id) t.active = false;
  e->id = kInvalidElement;
}
void UI::removeAll() {
  for (auto &e : elements) e.id = kInvalidElement;
  for (auto &t : tweens)   t.active = false;
  nextId = 1;
}

// ---------- direct setters ----------
void UI::setPos     (ElementId id, float x, float y)  { Element *e=findElement(id); if(e){e->x=x;e->y=y;} }
void UI::setX       (ElementId id, float x)            { Element *e=findElement(id); if(e) e->x=x; }
void UI::setY       (ElementId id, float y)            { Element *e=findElement(id); if(e) e->y=y; }
void UI::setSize    (ElementId id, float w, float h)  { Element *e=findElement(id); if(e){e->w=w;e->h=h;} }
void UI::setRadius  (ElementId id, float r)            { Element *e=findElement(id); if(e) e->radius=r; }
void UI::setRotation(ElementId id, float rad)          { Element *e=findElement(id); if(e) e->rotation=rad; }
void UI::setScale   (ElementId id, float sx, float sy){ Element *e=findElement(id); if(e){e->scaleX=sx;e->scaleY=sy;} }
void UI::setOpacity (ElementId id, float a)            { Element *e=findElement(id); if(e) e->opacity=a<0?0:(a>1?1:a); }
void UI::setColor   (ElementId id, uint16_t c)         { Element *e=findElement(id); if(e) rgb565ToRgb(c, e->r, e->g, e->b); }
void UI::setColor   (ElementId id, uint8_t r, uint8_t g, uint8_t b) {
  Element *e=findElement(id); if(e){ e->r=r; e->g=g; e->b=b; }
}
void UI::setVisible (ElementId id, bool v)             { Element *e=findElement(id); if(e) e->visible=v; }

Element *UI::get(ElementId id) { return findElement(id); }

// ---------- hit test ----------
ElementId UI::hitTest(int16_t px, int16_t py) {
  // Iterate in reverse so the topmost (most recently created) wins.
  for (int i = kMaxElements - 1; i >= 0; i--) {
    const Element &e = elements[i];
    if (e.id == kInvalidElement || !e.visible) continue;
    float dx = (float)px - e.x;
    float dy = (float)py - e.y;
    switch (e.shape) {
      case Shape::Rect: {
        float w = e.w * e.scaleX, h = e.h * e.scaleY;
        if (fabsf(dx) <= w/2 && fabsf(dy) <= h/2) return e.id;
        break;
      }
      case Shape::Circle: {
        float r = e.radius * 0.5f * (e.scaleX + e.scaleY);
        if (dx*dx + dy*dy <= r*r) return e.id;
        break;
      }
      case Shape::Triangle: {
        // Cheap AABB of vertex offsets — close enough for finger taps.
        float mnx = e.vx[0], mxx = e.vx[0];
        float mny = e.vy[0], mxy = e.vy[0];
        for (int j = 1; j < 3; j++) {
          if (e.vx[j] < mnx) mnx = e.vx[j];
          if (e.vx[j] > mxx) mxx = e.vx[j];
          if (e.vy[j] < mny) mny = e.vy[j];
          if (e.vy[j] > mxy) mxy = e.vy[j];
        }
        if (dx >= mnx && dx <= mxx && dy >= mny && dy <= mxy) return e.id;
        break;
      }
      default: break;
    }
  }
  return kInvalidElement;
}

// ---------- animation ----------
void UI::animate(ElementId id, Prop prop, float to,
                 uint32_t durationMs, Easing easing, uint32_t delayMs) {
  if (!findElement(id)) return;
  int s = slotForNewTween();
  if (s < 0) return;
  Tween &t = tweens[s];
  t.target     = id;
  t.prop       = prop;
  t.from       = 0;             // snapshot when the delay expires (so chained tweens compose)
  t.to         = to;
  t.startMs    = millis() + delayMs;
  t.durationMs = durationMs == 0 ? 1 : durationMs;
  t.easing     = easing;
  t.active     = true;
  t.started    = false;
}

void UI::animateColor(ElementId id, uint16_t toRgb,
                      uint32_t durationMs, Easing easing, uint32_t delayMs) {
  uint8_t r, g, b;
  rgb565ToRgb(toRgb, r, g, b);
  animate(id, Prop::R, (float)r, durationMs, easing, delayMs);
  animate(id, Prop::G, (float)g, durationMs, easing, delayMs);
  animate(id, Prop::B, (float)b, durationMs, easing, delayMs);
}

void UI::cancelAnimations(ElementId id) {
  for (auto &t : tweens) if (t.target == id) t.active = false;
}
void UI::cancelAllAnimations() {
  for (auto &t : tweens) t.active = false;
}
bool UI::isAnimating() const {
  for (const auto &t : tweens) if (t.active) return true;
  return false;
}

// ---------- per-property accessors ----------
void UI::applyProperty(Element &e, Prop p, float v) {
  switch (p) {
    case Prop::X:        e.x = v; break;
    case Prop::Y:        e.y = v; break;
    case Prop::W:        e.w = v; break;
    case Prop::H:        e.h = v; break;
    case Prop::Radius:   e.radius = v; break;
    case Prop::Rotation: e.rotation = v; break;
    case Prop::ScaleX:   e.scaleX = v; break;
    case Prop::ScaleY:   e.scaleY = v; break;
    case Prop::Opacity:  e.opacity = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case Prop::R:        e.r = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); break;
    case Prop::G:        e.g = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); break;
    case Prop::B:        e.b = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); break;
  }
}
float UI::readProperty(const Element &e, Prop p) const {
  switch (p) {
    case Prop::X:        return e.x;
    case Prop::Y:        return e.y;
    case Prop::W:        return e.w;
    case Prop::H:        return e.h;
    case Prop::Radius:   return e.radius;
    case Prop::Rotation: return e.rotation;
    case Prop::ScaleX:   return e.scaleX;
    case Prop::ScaleY:   return e.scaleY;
    case Prop::Opacity:  return e.opacity;
    case Prop::R:        return (float)e.r;
    case Prop::G:        return (float)e.g;
    case Prop::B:        return (float)e.b;
  }
  return 0;
}

// ---------- easing ----------
float UI::ease(Easing kind, float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  switch (kind) {
    case Easing::Linear:         return t;
    case Easing::EaseInQuad:     return t * t;
    case Easing::EaseOutQuad:    return 1.f - (1.f - t) * (1.f - t);
    case Easing::EaseInOutQuad:  return t < 0.5f ? 2*t*t : 1 - 2*(1-t)*(1-t);
    case Easing::EaseInCubic:    return t * t * t;
    case Easing::EaseOutCubic:   { float u = 1 - t; return 1 - u*u*u; }
    case Easing::EaseInOutCubic: {
      if (t < 0.5f) return 4*t*t*t;
      float u = 1 - t;
      return 1 - 4*u*u*u;
    }
    case Easing::EaseOutBounce: {
      const float n1 = 7.5625f, d1 = 2.75f;
      if (t < 1.f/d1)        return n1 * t * t;
      if (t < 2.f/d1)        { float u = t - 1.5f/d1;   return n1*u*u + 0.75f; }
      if (t < 2.5f/d1)       { float u = t - 2.25f/d1;  return n1*u*u + 0.9375f; }
      { float u = t - 2.625f/d1; return n1*u*u + 0.984375f; }
    }
    case Easing::EaseOutBack: {
      const float c1 = 1.70158f, c3 = c1 + 1.f;
      float u = t - 1;
      return 1 + c3 * u * u * u + c1 * u * u;
    }
  }
  return t;
}

// ---------- tick ----------
void UI::tick() {
  uint32_t now = millis();
  for (auto &t : tweens) {
    if (!t.active) continue;
    if ((int32_t)(now - t.startMs) < 0) continue;       // still in delay window
    Element *e = findElement(t.target);
    if (!e) { t.active = false; continue; }
    if (!t.started) {
      t.from = readProperty(*e, t.prop);
      t.started = true;
    }
    uint32_t elapsed = now - t.startMs;
    if (elapsed >= t.durationMs) {
      applyProperty(*e, t.prop, t.to);
      t.active = false;
    } else {
      float p = (float)elapsed / (float)t.durationMs;
      float v = t.from + (t.to - t.from) * ease(t.easing, p);
      applyProperty(*e, t.prop, v);
    }
  }
}

// ---------- render ----------
uint16_t UI::blendColor(const Element &e) const {
  // Linear blend between (r,g,b) and the canvas's background color, scaled
  // by opacity. The blend is approximate — it ignores whatever the canvas
  // actually contains under the element — but it gives a usable fade.
  uint8_t br = (uint8_t)(((bgColor >> 11) & 0x1F) << 3);
  uint8_t bg = (uint8_t)(((bgColor >>  5) & 0x3F) << 2);
  uint8_t bb = (uint8_t)(( bgColor        & 0x1F) << 3);
  float a = e.opacity;
  uint8_t r = (uint8_t)(br + (e.r - br) * a);
  uint8_t g = (uint8_t)(bg + (e.g - bg) * a);
  uint8_t b = (uint8_t)(bb + (e.b - bb) * a);
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void UI::drawElement(const Element &e) {
  if (!canvas) return;
  uint16_t color = blendColor(e);
  switch (e.shape) {
    case Shape::Rect: {
      float w = e.w * e.scaleX;
      float h = e.h * e.scaleY;
      if (fabsf(e.rotation) < 0.001f) {
        canvas->fillRect((int16_t)(e.x - w/2), (int16_t)(e.y - h/2),
                         (int16_t)w, (int16_t)h, color);
      } else {
        // Rotate the four corners about (e.x, e.y); fill as two triangles.
        float ca = cosf(e.rotation), sa = sinf(e.rotation);
        float hw = w/2, hh = h/2;
        float lx[4] = { -hw,  hw,  hw, -hw };
        float ly[4] = { -hh, -hh,  hh,  hh };
        int16_t sx[4], sy[4];
        for (int i = 0; i < 4; i++) {
          sx[i] = (int16_t)(e.x + lx[i]*ca - ly[i]*sa);
          sy[i] = (int16_t)(e.y + lx[i]*sa + ly[i]*ca);
        }
        canvas->fillTriangle(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], color);
        canvas->fillTriangle(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], color);
      }
      break;
    }
    case Shape::Circle: {
      float r = e.radius * 0.5f * (e.scaleX + e.scaleY);
      if (r < 1) r = 1;
      canvas->fillCircle((int16_t)e.x, (int16_t)e.y, (int16_t)r, color);
      break;
    }
    case Shape::Triangle: {
      float ca = cosf(e.rotation), sa = sinf(e.rotation);
      int16_t sx[3], sy[3];
      for (int i = 0; i < 3; i++) {
        float lx = e.vx[i] * e.scaleX;
        float ly = e.vy[i] * e.scaleY;
        sx[i] = (int16_t)(e.x + lx*ca - ly*sa);
        sy[i] = (int16_t)(e.y + lx*sa + ly*ca);
      }
      canvas->fillTriangle(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], color);
      break;
    }
    case Shape::Line: {
      float ca = cosf(e.rotation), sa = sinf(e.rotation);
      int16_t sx[2], sy[2];
      for (int i = 0; i < 2; i++) {
        float lx = e.vx[i] * e.scaleX;
        float ly = e.vy[i] * e.scaleY;
        sx[i] = (int16_t)(e.x + lx*ca - ly*sa);
        sy[i] = (int16_t)(e.y + lx*sa + ly*ca);
      }
      canvas->drawLine(sx[0], sy[0], sx[1], sy[1], color);
      break;
    }
    case Shape::Text: {
      if (!e.text) break;
      canvas->setTextColor(color, bgColor);
      canvas->setTextSize(e.textSize);
      canvas->setCursor((int16_t)e.x, (int16_t)e.y);
      canvas->print(e.text);
      break;
    }
  }
}

void UI::render() {
  if (!canvas) return;
  canvas->fillScreen(bgColor);
  // Front-to-back creation order; lower index drawn first (bottom).
  for (auto &e : elements) {
    if (e.id == kInvalidElement || !e.visible) continue;
    drawElement(e);
  }
}
