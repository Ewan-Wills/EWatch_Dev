// Particle sandbox — see particles.h. Renders into the shared PSRAM frame
// canvas with an exponential fade each frame, which gives every particle a
// free motion trail: dim the previous frame, plot the new positions, flush.
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <esp_random.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "particles.h"

static const int16_t W = 240, H = 280;

// ---------- particle pool ----------
// One pool shared by all modes. `life` counts down in frames; 0 = free slot.
struct P {
  float    x, y, vx, vy;
  uint16_t life;
  uint8_t  hue;      // 0..255 colour wheel
  uint8_t  kind;     // fireworks: 0 = rocket, 1 = spark; other modes ignore
};
static const int kMaxP = 700;
static P pool[kMaxP];

static int alloc() {
  for (int i = 0; i < kMaxP; i++) if (pool[i].life == 0) return i;
  return -1;
}

static inline uint32_t rnd() { return esp_random(); }
static inline float frnd() { return (float)(rnd() & 0xFFFF) / 65535.0f; }

// Fast HSV-ish colour wheel -> RGB565. s=v=1. hue 0..255.
static uint16_t wheel565(uint8_t h, uint8_t brightness /*0..255*/) {
  uint8_t r, g, b;
  uint8_t seg = h / 43;             // 6 segments of ~43
  uint8_t rem = (h - seg * 43) * 6; // 0..255 within segment
  switch (seg) {
    default:
    case 0: r = 255;       g = rem;         b = 0;         break;
    case 1: r = 255 - rem; g = 255;         b = 0;         break;
    case 2: r = 0;         g = 255;         b = rem;       break;
    case 3: r = 0;         g = 255 - rem;   b = 255;       break;
    case 4: r = rem;       g = 0;           b = 255;       break;
    case 5: r = 255;       g = 0;           b = 255 - rem; break;
  }
  r = (uint16_t)r * brightness / 255;
  g = (uint16_t)g * brightness / 255;
  b = (uint16_t)b * brightness / 255;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Dim every pixel toward black: halves each RGB565 channel. The mask clears
// the bit that would otherwise bleed between channels after the shift.
static void fadeFrame(uint16_t *fb, int dimShift) {
  const uint32_t mask = (dimShift == 1) ? 0x7BEF7BEF : 0x39E739E7;
  uint32_t *p = (uint32_t *)fb;
  int n = (W * H) / 2;
  for (int i = 0; i < n; i++) {
    p[i] = (p[i] >> dimShift) & mask;
  }
}

static inline void plot(uint16_t *fb, int x, int y, uint16_t c) {
  if (x < 0 || x >= W - 1 || y < 0 || y >= H - 1) return;
  uint16_t *row = fb + y * W + x;
  row[0] = c; row[1] = c;
  row[W] = c; row[W + 1] = c;
}

// Tilt gravity in screen coordinates. Matches the IMU-gestures tilt ball
// convention (ball rolls downhill): downhill = (-ax, -ay) in screen frame.
static void tiltGravity(float &gx, float &gy) {
  int16_t ax, ay;
  { ModelLock lk; ax = model.ax; ay = model.ay; }
  gx = -(float)ax / 4096.0f;
  gy = -(float)ay / 4096.0f;
}

void ParticlesView::resetParticles() {
  memset(pool, 0, sizeof(pool));
}

void ParticlesView::onEnter() {
  resetParticles();
  touchDown = false;
  lastSpawnMs = 0;
  frameCount = 0;
  modeLabelUntil = millis() + 1500;
  Arduino_Canvas *canvas = frameCanvas();
  if (canvas) { canvas->fillScreen(BLACK); canvas->flush(); }
}

void ParticlesView::onExit() {
  resetParticles();
}

void ParticlesView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp || e.gesture == Gesture::SwipeDown) {
      int d = (e.gesture == Gesture::SwipeUp) ? 1 : -1;
      mode = (uint8_t)((mode + 3 + d) % 3);
      resetParticles();
      modeLabelUntil = millis() + 1500;
      hapticBuzz(50, 50);
    }
    return;
  }
  if (e.type == EventType::Touch || e.type == EventType::TouchHold) {
    touchDown = true;
    touchX = (int16_t)e.x; touchY = (int16_t)e.y;
    if (mode == 0 && e.type == EventType::Touch) {
      // Fireworks: a tap launches a rocket from the bottom toward the tap x.
      int i = alloc();
      if (i >= 0) {
        pool[i] = { (float)e.x, (float)(H - 4),
                    (frnd() - 0.5f) * 0.6f, -(4.5f + frnd() * 1.5f),
                    (uint16_t)(46 + (rnd() % 20)), (uint8_t)rnd(), 0 };
        hapticBuzz(40, 30);
      }
    }
    return;
  }
  if (e.type == EventType::TouchUp) { touchDown = false; return; }
}

void ParticlesView::render() {
  step();
}

void ParticlesView::step() {
  Arduino_Canvas *canvas = frameCanvas();
  if (!canvas) return;
  uint16_t *fb = canvas->getFramebuffer();
  if (!fb) return;

  frameCount++;
  uint32_t now = millis();
  float gx, gy;
  tiltGravity(gx, gy);

  // Half-brightness fade per frame ≈ 5-frame motion trails.
  fadeFrame(fb, 1);

  // ---- spawn per mode ----
  if (mode == 0) {
    // Auto-launch a rocket roughly every 1.3 s.
    if (now - lastSpawnMs > 1300) {
      lastSpawnMs = now;
      int i = alloc();
      if (i >= 0) {
        pool[i] = { 30.0f + frnd() * (W - 60), (float)(H - 4),
                    (frnd() - 0.5f) * 0.6f, -(4.2f + frnd() * 1.8f),
                    (uint16_t)(44 + (rnd() % 24)), (uint8_t)rnd(), 0 };
      }
    }
  } else if (mode == 1) {
    // Fountain: continuous spray. Emitter follows the finger while held,
    // otherwise sits bottom-centre.
    float ex = touchDown ? touchX : (W / 2);
    float ey = touchDown ? touchY : (H - 6);
    for (int s = 0; s < 8; s++) {
      int i = alloc();
      if (i < 0) break;
      float ang = -1.5708f + (frnd() - 0.5f) * 0.9f;   // up ± ~26°
      float spd = 2.5f + frnd() * 2.5f;
      pool[i] = { ex, ey, cosf(ang) * spd, sinf(ang) * spd,
                  (uint16_t)(50 + (rnd() % 40)),
                  (uint8_t)(140 + (rnd() % 60)), 1 };   // blue-cyan water hues
    }
  } else {
    // Orbit: keep the pool topped up with swarmers.
    for (int s = 0; s < 4; s++) {
      int i = alloc();
      if (i < 0) break;
      pool[i] = { frnd() * W, frnd() * H,
                  (frnd() - 0.5f) * 2.f, (frnd() - 0.5f) * 2.f,
                  (uint16_t)(300 + (rnd() % 300)),
                  (uint8_t)rnd(), 1 };
    }
  }

  // ---- update + draw ----
  float atX = touchDown ? touchX : (W / 2);
  float atY = touchDown ? touchY : (H / 2 + 20);

  for (int i = 0; i < kMaxP; i++) {
    P &p = pool[i];
    if (p.life == 0) continue;
    p.life--;

    if (mode == 0) {
      if (p.kind == 0) {
        // Rocket: rises against fixed "down", slight tilt influence.
        p.vy += 0.06f;
        p.vx += gx * 0.05f;
        p.x += p.vx; p.y += p.vy;
        plot(fb, (int)p.x, (int)p.y, wheel565(p.hue, 255));
        // Burst at apex (vy crosses 0) or when life runs out.
        if (p.vy >= -0.2f || p.life == 0) {
          uint8_t baseHue = p.hue;
          float bx = p.x, by = p.y;
          p.life = 0;
          hapticBuzz(90, 40);
          for (int s = 0; s < 70; s++) {
            int j = alloc();
            if (j < 0) break;
            float ang = frnd() * 6.2832f;
            float spd = frnd() * 3.4f;
            pool[j] = { bx, by, cosf(ang) * spd, sinf(ang) * spd,
                        (uint16_t)(26 + (rnd() % 34)),
                        (uint8_t)(baseHue + (rnd() % 40) - 20), 1 };
          }
          continue;
        }
      } else {
        // Spark: gravity (fixed down + tilt), air drag, fades with life.
        p.vy += 0.085f + gy * 0.03f;
        p.vx += gx * 0.03f;
        p.vx *= 0.985f; p.vy *= 0.985f;
        p.x += p.vx; p.y += p.vy;
        uint8_t bright = (uint8_t)(p.life > 50 ? 255 : p.life * 5);
        plot(fb, (int)p.x, (int)p.y, wheel565(p.hue, bright));
      }
    } else if (mode == 1) {
      // Fountain droplet: full tilt gravity + wall bounce.
      p.vx += gx * 0.28f;
      p.vy += 0.20f + gy * 0.28f;
      p.x += p.vx; p.y += p.vy;
      if (p.x < 1)     { p.x = 1;     p.vx = -p.vx * 0.55f; }
      if (p.x > W - 2) { p.x = W - 2; p.vx = -p.vx * 0.55f; }
      if (p.y > H - 2) { p.y = H - 2; p.vy = -p.vy * 0.45f;
                         if (fabsf(p.vy) < 0.4f) p.life = (p.life > 6) ? 6 : p.life; }
      if (p.y < 1)     { p.y = 1;     p.vy = -p.vy * 0.55f; }
      uint8_t bright = (uint8_t)(p.life > 40 ? 255 : p.life * 6);
      plot(fb, (int)p.x, (int)p.y, wheel565(p.hue, bright));
    } else {
      // Orbit: spring-ish attraction to the attractor + damping = swirl.
      float dx = atX - p.x, dy = atY - p.y;
      float d2 = dx * dx + dy * dy + 40.f;
      float inv = 1.0f / sqrtf(d2);
      float f = 90.0f / d2;                  // inverse-square pull
      p.vx += dx * inv * f + gx * 0.05f;
      p.vy += dy * inv * f + gy * 0.05f;
      p.vx *= 0.992f; p.vy *= 0.992f;
      p.x += p.vx; p.y += p.vy;
      // Recycle escapees.
      if (p.x < -20 || p.x > W + 20 || p.y < -20 || p.y > H + 20) { p.life = 0; continue; }
      // Hue drifts along the particle's lifetime for a shimmering swarm.
      plot(fb, (int)p.x, (int)p.y, wheel565((uint8_t)(p.hue + (frameCount >> 1)), 255));
    }
  }

  // ---- mode label (transient) ----
  if (now < modeLabelUntil) {
    static const char *kNames[3] = { "FIREWORKS", "FOUNTAIN", "ORBIT" };
    const char *nm = kNames[mode];
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(uiCenterX(nm, 2), 12);
    canvas->print(nm);
    canvas->setTextSize(1);
    canvas->setTextColor(DARKGREY);
    const char *hint = "swipe = mode   btn = exit";
    canvas->setCursor(uiCenterX(hint, 1), 34);
    canvas->print(hint);
  }

  canvas->flush();
}
