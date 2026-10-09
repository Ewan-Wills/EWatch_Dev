// Flock / starfield demo — see boids.h. Both scenes render into the shared
// PSRAM frame canvas: fade the previous frame for motion trails, draw, flush.
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <esp_random.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "boids.h"

static const int16_t W = 240, H = 280;

static inline float frnd() { return (float)(esp_random() & 0xFFFF) / 65535.0f; }

// ---------- boids ----------
struct Boid { float x, y, vx, vy; uint8_t hue; };
static const int kNBoids = 44;
static Boid boids[kNBoids];

// ---------- stars ----------
struct Star { float x, y, z; };     // x,y in [-1,1] view units, z in (0,1]
static const int kNStars = 220;
static Star stars[kNStars];
static float warp = 0.f;            // 0 = cruise, ramps to 1 while held

static uint16_t wheel565(uint8_t h, uint8_t v) {
  uint8_t r, g, b;
  uint8_t seg = h / 43;
  uint8_t rem = (h - seg * 43) * 6;
  switch (seg) {
    default:
    case 0: r = 255;       g = rem;         b = 0;         break;
    case 1: r = 255 - rem; g = 255;         b = 0;         break;
    case 2: r = 0;         g = 255;         b = rem;       break;
    case 3: r = 0;         g = 255 - rem;   b = 255;       break;
    case 4: r = rem;       g = 0;           b = 255;       break;
    case 5: r = 255;       g = 0;           b = 255 - rem; break;
  }
  r = (uint16_t)r * v / 255; g = (uint16_t)g * v / 255; b = (uint16_t)b * v / 255;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void fadeFrame(uint16_t *fb, int shift) {
  const uint32_t mask = (shift == 1) ? 0x7BEF7BEF : 0x39E739E7;
  uint32_t *p = (uint32_t *)fb;
  int n = (W * H) / 2;
  for (int i = 0; i < n; i++) p[i] = (p[i] >> shift) & mask;
}

static void tiltVector(float &tx, float &ty) {
  int16_t ax, ay;
  { ModelLock lk; ax = model.ax; ay = model.ay; }
  tx = -(float)ax / 4096.0f;
  ty = -(float)ay / 4096.0f;
}

void BoidsView::resetScene() {
  for (int i = 0; i < kNBoids; i++) {
    float ang = frnd() * 6.2832f;
    boids[i] = { frnd() * W, frnd() * H,
                 cosf(ang) * 1.5f, sinf(ang) * 1.5f,
                 (uint8_t)esp_random() };
  }
  for (int i = 0; i < kNStars; i++) {
    stars[i] = { (frnd() - 0.5f) * 2.f, (frnd() - 0.5f) * 2.f,
                 0.05f + frnd() * 0.95f };
  }
  warp = 0.f;
}

void BoidsView::onEnter() {
  resetScene();
  touchDown = false;
  pressMoved = false;
  sceneLabelUntil = millis() + 1500;
  Arduino_Canvas *canvas = frameCanvas();
  if (canvas) { canvas->fillScreen(BLACK); canvas->flush(); }
}

void BoidsView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch) {
    touchDown = true;
    touchX = (int16_t)e.x; touchY = (int16_t)e.y;
    pressX = e.x; pressY = e.y; pressMs = millis(); pressMoved = false;
    return;
  }
  if (e.type == EventType::TouchHold) {
    touchDown = true;
    touchX = (int16_t)e.x; touchY = (int16_t)e.y;
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > 16 * 16) pressMoved = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    touchDown = false;
    // Quick clean tap toggles the scene.
    if (!pressMoved && millis() - pressMs < 350) {
      scene = (uint8_t)(1 - scene);
      sceneLabelUntil = millis() + 1500;
      hapticBuzz(50, 50);
    }
    return;
  }
}

void BoidsView::render() {
  Arduino_Canvas *canvas = frameCanvas();
  if (!canvas || !canvas->getFramebuffer()) return;
  if (scene == 0) stepBoids();
  else            stepStars();

  if (millis() < sceneLabelUntil) {
    const char *nm   = scene == 0 ? "FLOCK" : "STARFIELD";
    const char *hint = scene == 0 ? "tap = scene  hold = scatter"
                                  : "tap = scene  hold = warp";
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(uiCenterX(nm, 2), 12);
    canvas->print(nm);
    canvas->setTextSize(1);
    canvas->setTextColor(DARKGREY);
    canvas->setCursor(uiCenterX(hint, 1), 34);
    canvas->print(hint);
  }
  canvas->flush();
}

void BoidsView::stepBoids() {
  Arduino_Canvas *canvas = frameCanvas();
  uint16_t *fb = canvas->getFramebuffer();
  fadeFrame(fb, 2);      // short trails

  float tx, ty;
  tiltVector(tx, ty);

  // A hold longer than the tap threshold acts as a predator at the finger.
  bool predator = touchDown && pressMoved;
  bool predatorHold = touchDown && (millis() - pressMs > 350);

  const float kSepR2   = 14.f * 14.f;
  const float kNearR2  = 44.f * 44.f;
  const float kMaxSpd  = 3.0f;

  for (int i = 0; i < kNBoids; i++) {
    Boid &b = boids[i];
    float sepX = 0, sepY = 0;
    float aliX = 0, aliY = 0;
    float cohX = 0, cohY = 0;
    int   near = 0;
    for (int j = 0; j < kNBoids; j++) {
      if (j == i) continue;
      float dx = boids[j].x - b.x, dy = boids[j].y - b.y;
      float d2 = dx * dx + dy * dy;
      if (d2 > kNearR2) continue;
      near++;
      aliX += boids[j].vx; aliY += boids[j].vy;
      cohX += boids[j].x;  cohY += boids[j].y;
      if (d2 < kSepR2 && d2 > 0.01f) {
        sepX -= dx / d2 * 12.f;
        sepY -= dy / d2 * 12.f;
      }
    }
    if (near > 0) {
      b.vx += (aliX / near - b.vx) * 0.045f + (cohX / near - b.x) * 0.0035f + sepX;
      b.vy += (aliY / near - b.vy) * 0.045f + (cohY / near - b.y) * 0.0035f + sepY;
    }
    // Tilt = wind.
    b.vx += tx * 0.12f;
    b.vy += ty * 0.12f;
    // Predator flee.
    if (predator || predatorHold) {
      float dx = b.x - touchX, dy = b.y - touchY;
      float d2 = dx * dx + dy * dy + 1.f;
      if (d2 < 90.f * 90.f) {
        float inv = 1.0f / sqrtf(d2);
        b.vx += dx * inv * (2200.f / d2);
        b.vy += dy * inv * (2200.f / d2);
      }
    }
    // Soft screen-edge steer (wrap looks jumpy with trails).
    if (b.x < 20)      b.vx += 0.25f;
    if (b.x > W - 20)  b.vx -= 0.25f;
    if (b.y < 20)      b.vy += 0.25f;
    if (b.y > H - 20)  b.vy -= 0.25f;

    float spd = sqrtf(b.vx * b.vx + b.vy * b.vy);
    if (spd > kMaxSpd) { b.vx *= kMaxSpd / spd; b.vy *= kMaxSpd / spd; }
    if (spd < 0.8f && spd > 0.001f) { b.vx *= 1.06f; b.vy *= 1.06f; }

    b.x += b.vx; b.y += b.vy;
    if (b.x < 0) b.x = 0; if (b.x > W - 1) b.x = W - 1;
    if (b.y < 0) b.y = 0; if (b.y > H - 1) b.y = H - 1;

    // Draw as a small heading triangle: nose + two tail corners.
    float inv = (spd > 0.001f) ? 1.0f / spd : 0.f;
    float hx = b.vx * inv, hy = b.vy * inv;         // heading unit
    float px = -hy, py = hx;                        // perpendicular
    int16_t nx = (int16_t)(b.x + hx * 6);
    int16_t ny = (int16_t)(b.y + hy * 6);
    int16_t lx = (int16_t)(b.x - hx * 4 + px * 3);
    int16_t ly = (int16_t)(b.y - hy * 4 + py * 3);
    int16_t rx = (int16_t)(b.x - hx * 4 - px * 3);
    int16_t ry = (int16_t)(b.y - hy * 4 - py * 3);
    canvas->fillTriangle(nx, ny, lx, ly, rx, ry, wheel565(b.hue, 255));
  }
}

void BoidsView::stepStars() {
  Arduino_Canvas *canvas = frameCanvas();
  uint16_t *fb = canvas->getFramebuffer();
  // Warp leaves streaks; cruise fades faster for crisp points.
  warp += ((touchDown ? 1.f : 0.f) - warp) * 0.06f;
  fadeFrame(fb, warp > 0.4f ? 1 : 2);

  float tx, ty;
  tiltVector(tx, ty);

  float speed = 0.012f + warp * 0.06f;

  for (int i = 0; i < kNStars; i++) {
    Star &s = stars[i];
    s.z -= speed;
    // Tilt steers: drift the whole field opposite the tilt so it reads as
    // the camera turning into the tilt.
    s.x -= tx * 0.004f;
    s.y -= ty * 0.004f;
    if (s.z <= 0.03f || fabsf(s.x) > 1.4f || fabsf(s.y) > 1.4f) {
      s = { (frnd() - 0.5f) * 2.f, (frnd() - 0.5f) * 2.f, 1.f };
      continue;
    }
    float invZ = 1.0f / s.z;
    int x = (int)(W / 2 + s.x * invZ * 110.f);
    int y = (int)(H / 2 + s.y * invZ * 110.f);
    if (x < 0 || x >= W || y < 0 || y >= H) continue;
    // Nearer stars are brighter and slightly bluer-white.
    uint8_t v = (uint8_t)(255.f * (1.f - s.z) * (1.f - s.z));
    if (v < 40) v = 40;
    uint16_t c = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
    fb[y * W + x] = c;
    if (s.z < 0.35f) {          // near stars get a 2×2 glow
      if (x + 1 < W)              fb[y * W + x + 1]       = c;
      if (y + 1 < H)              fb[(y + 1) * W + x]     = c;
      if (x + 1 < W && y + 1 < H) fb[(y + 1) * W + x + 1] = c;
    }
  }
}
