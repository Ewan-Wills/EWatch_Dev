#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <limits.h>

#include "tunnel.h"
#include "display.h"     // gfx, frameCanvas
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model + ModelLock (accel for tilt)
#include "event.h"       // eventQueue
#include "view.h"        // switchTo, tappedBack

// =====================================================================
// Geometry / gameplay tunables. Grouped so the "feel" can be dialled in from
// one place — frame rate and motion are the product here, so everything below
// is chosen to keep per-frame work bounded and the motion smooth.
// =====================================================================
namespace {

constexpr int16_t W = 240, H = 280;
constexpr float   CX = 120.0f, CY = 140.0f;   // screen centre (vanishing point)

constexpr int   NSEG = 14;        // cross-section sides (polygon → tube)
constexpr int   NR   = 16;        // rings drawn from camera to vanishing point
constexpr float RING_R = 1.0f;    // tube radius in world units
constexpr float SP   = 0.62f;     // world-space spacing between rings
constexpr float F    = 86.0f;     // focal length (projection scale)
constexpr float Z_CLIP = 0.20f;   // rings nearer than this have passed us
constexpr float Z_HIT  = 0.72f;   // collision / player plane depth
constexpr float Z_PLAYER = 0.72f; // marker rides at this depth
constexpr float Z_FADE = 6.5f;    // depth over which colour fades to dark

// Gentle tunnel bend (hypnotic curve) — camera follows the same path.
constexpr float BENDX = 0.55f, BFX = 0.55f;
constexpr float BENDY = 0.42f, BFY = 0.41f;
// Slow global roll/bank applied to the whole scene (player + obstacles move
// together, so it is purely cosmetic and never affects collision).
constexpr float ROLL_AMP = 0.18f, ROLL_FREQ = 0.55f;

// Speed ramp — the core tension. Units are world-distance per second.
constexpr float BASE_SPEED = 2.3f;
constexpr float RAMP       = 0.018f;   // speed += dist * RAMP
constexpr float MAX_SPEED  = 7.5f;
constexpr float ATTRACT_SPEED = 1.6f;  // slow fly-through on title / game-over

// Obstacles: a ring is an obstacle when (g % OBS_EVERY == 0) and g >= firstObs.
constexpr long  OBS_EVERY = 6;
constexpr long  FIRST_OBS = 12;        // grace rings before the first hazard
constexpr long  NO_OBS    = 1000000000L;
constexpr float GAP_EASY = 0.66f;      // half-width of the safe gap (radians)
constexpr float GAP_HARD = 0.30f;
constexpr float DIFF_DIST = 140.0f;    // distance over which difficulty maxes

constexpr float TILT_DEAD = 700.0f;    // accel magnitude below this = "flat"
constexpr float TILT_RESP = 7.5f;      // steering responsiveness (banking)
constexpr bool  TILT_INV_X = true;     // flip if steering feels mirrored
constexpr bool  TILT_INV_Y = true;

constexpr float SCORE_K = 9.0f;        // score = dist * SCORE_K (+ bonuses)
constexpr uint32_t HAPTIC_STEP = 220;  // rising-pulse cadence (score units)

constexpr uint32_t CRASH_MS = 1200;    // shatter animation length
constexpr int   MAX_SHARDS = 60;

// ---- palette (RGB565) ----
inline uint16_t rgb565(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
inline void split565(uint16_t c, int &r, int &g, int &b) {
  r = ((c >> 11) & 0x1F) << 3; g = ((c >> 5) & 0x3F) << 2; b = (c & 0x1F) << 3;
}
inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  int ar, ag, ab, br, bg, bb; split565(a, ar, ag, ab); split565(b, br, bg, bb);
  return rgb565((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t),
                (int)(ab + (bb - ab) * t));
}
inline uint16_t scale565(uint16_t c, float f) {
  int r, g, b; split565(c, r, g, b);
  return rgb565((int)(r * f), (int)(g * f), (int)(b * f));
}

const uint16_t ORANGE_HOT  = rgb565(232, 107, 43);   // #E86B2B — the brand neon
const uint16_t FAR_DARK    = rgb565(26, 12, 48);     // deep indigo near vanish
const uint16_t DANGER      = rgb565(255, 48, 32);
const uint16_t GAPCOL      = rgb565(48, 255, 130);
const uint16_t PLAYER_COL  = rgb565(150, 245, 255);
const uint16_t PLAYER_GLOW = rgb565(30, 110, 160);
const uint16_t HUD_COL     = rgb565(255, 180, 120);

// =====================================================================
// Fast clipped primitives writing straight into the canvas framebuffer. We
// bypass the GFX virtual draw path for the hundreds of tunnel line segments;
// text + the player triangle still go through the canvas object.
// =====================================================================
inline void plot(uint16_t *fb, int x, int y, uint16_t c) {
  if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) fb[y * W + x] = c;
}
void lineFb(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    plot(fb, x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void lineThickFb(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c, int th) {
  lineFb(fb, x0, y0, x1, y1, c);
  if (th > 1) { lineFb(fb, x0 + 1, y0, x1 + 1, y1, c);
                lineFb(fb, x0, y0 + 1, x1, y1 + 1, c); }
}
void fillRectFb(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x; if (y + h > H) h = H - y;
  for (int yy = y; yy < y + h; yy++) {
    uint16_t *row = fb + yy * W + x;
    for (int xx = 0; xx < w; xx++) row[xx] = c;
  }
}

// =====================================================================
// Game state (single instance, so file-static keeps the header tiny).
// =====================================================================
enum class Phase { Title, Playing, Crashing, GameOver };

Phase    phase = Phase::Title;
float    dist = 0.0f;             // total world distance travelled
float    speed = ATTRACT_SPEED;
float    playerAngle = 0.0f;      // current marker orbit angle (radians)
float    targetAngle = 0.0f;      // tilt-derived target
uint32_t score = 0, best = 0, bonusScore = 0;
bool     newBest = false;
long     firstObsG = NO_OBS;
long     nextObsG  = NO_OBS;      // next obstacle global index to resolve
uint32_t lastHapticBucket = 0;
uint32_t crashStartMs = 0;
uint32_t lastFrameMs = 0;

float baseCos[NSEG], baseSin[NSEG], baseAng[NSEG], midAng[NSEG];

struct Shard { float x, y, vx, vy, hx, hy, ang, spin; };
Shard shards[MAX_SHARDS];
int   shardCount = 0;

Preferences prefs;
bool prefsOpen = false;

uint32_t rngState = 0x1234abcdu;
inline uint32_t xr() { rngState ^= rngState << 13; rngState ^= rngState >> 17;
                       rngState ^= rngState << 5; return rngState; }
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * ((xr() & 0xFFFF) / 65535.0f);
}

constexpr float TWO_PI_F = 6.28318530718f;
constexpr float PI_F     = 3.14159265359f;
inline float wrapPi(float a) {
  while (a >  PI_F) a -= TWO_PI_F;
  while (a < -PI_F) a += TWO_PI_F;
  return a;
}

// Deterministic gap centre for a given obstacle ring — no per-ring storage, so
// obstacles recycle for free as global indices scroll past.
float gapCenter(long g) {
  uint32_t h = (uint32_t)g * 2654435761u;
  h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
  return (float)(h & 1023) / 1024.0f * TWO_PI_F;
}
float gapHalf() {
  float d = dist / DIFF_DIST; if (d < 0) d = 0; if (d > 1) d = 1;
  return GAP_EASY + (GAP_HARD - GAP_EASY) * d;
}

// Path the tunnel centre follows; the camera tracks it so near rings stay
// centred while far ones drift → the curving-tube illusion.
inline float pathX(float s) { return BENDX * sinf(s * BFX); }
inline float pathY(float s) { return BENDY * sinf(s * BFY + 1.3f); }

void openPrefs() {
  if (prefsOpen) return;
  prefs.begin("tracer", /*readOnly=*/false);
  prefsOpen = true;
  best = prefs.getUInt("best", 0);
}

void resetTrig() {
  for (int k = 0; k < NSEG; k++) {
    float a = (float)k / NSEG * TWO_PI_F;
    baseAng[k] = a; baseCos[k] = cosf(a); baseSin[k] = sinf(a);
    midAng[k]  = a + PI_F / NSEG;          // midpoint angle of edge k→k+1
  }
}

void startGame() {
  dist = 0.0f; speed = BASE_SPEED;
  playerAngle = 0.0f; targetAngle = 0.0f;
  score = 0; bonusScore = 0; newBest = false;
  firstObsG = FIRST_OBS; nextObsG = FIRST_OBS;   // FIRST_OBS is a multiple of 6
  lastHapticBucket = 0;
  shardCount = 0;
  phase = Phase::Playing;
  lastFrameMs = millis();
  hapticBuzz(70, 45);
}

// ---- steering ----
void updateTilt(float dt) {
  int16_t ax, ay;
  { ModelLock lk; ax = model.ax; ay = model.ay; }
  float mag = sqrtf((float)ax * ax + (float)ay * ay);
  if (mag > TILT_DEAD) {
    float fx = TILT_INV_X ? -(float)ax : (float)ax;
    float fy = TILT_INV_Y ? -(float)ay : (float)ay;
    targetAngle = atan2f(fy, fx);
  }
  // Frame-rate-independent exponential approach → smooth banking, no snap.
  float k = 1.0f - expf(-TILT_RESP * dt);
  playerAngle = wrapPi(playerAngle + wrapPi(targetAngle - playerAngle) * k);
}

void triggerCrash();

void resolveObstacle(long g) {
  float gc = gapCenter(g), gh = gapHalf();
  float diff = fabsf(wrapPi(playerAngle - gc));
  if (diff <= gh) {
    if (diff < gh * 0.28f) { bonusScore += 25; hapticBuzz(90, 22); }  // clean pass
    else if (diff > gh * 0.62f) hapticBuzz(45, 12);                   // scrape
  } else {
    triggerCrash();
  }
}

void update(float dt) {
  switch (phase) {
    case Phase::Title:
      speed = ATTRACT_SPEED; dist += speed * dt; updateTilt(dt);
      break;
    case Phase::Playing: {
      speed = BASE_SPEED + dist * RAMP; if (speed > MAX_SPEED) speed = MAX_SPEED;
      dist += speed * dt;
      updateTilt(dt);
      score = (uint32_t)(dist * SCORE_K) + bonusScore;   // current, before any crash
      while (phase == Phase::Playing) {
        float zc = (float)nextObsG * SP - dist;
        if (zc > Z_HIT) break;
        resolveObstacle(nextObsG);                       // may trigger a crash
        nextObsG += OBS_EVERY;
      }
      if (phase == Phase::Playing) {
        uint32_t bucket = score / HAPTIC_STEP;
        if (bucket > lastHapticBucket) {
          lastHapticBucket = bucket;
          float f = (speed - BASE_SPEED) / (MAX_SPEED - BASE_SPEED);
          if (f < 0) f = 0; if (f > 1) f = 1;
          hapticBuzz((uint8_t)(70 + 130 * f), 28);   // rising pulse with speed
        }
      }
      break;
    }
    case Phase::Crashing: {
      for (int i = 0; i < shardCount; i++) {
        Shard &s = shards[i];
        s.x += s.vx * dt; s.y += s.vy * dt;
        s.vx *= (1.0f - 1.4f * dt); s.vy *= (1.0f - 1.4f * dt);
        s.vy += 60.0f * dt;                 // a little gravity for the tumble
        s.ang += s.spin * dt;
      }
      if (millis() - crashStartMs > CRASH_MS) phase = Phase::GameOver;
      break;
    }
    case Phase::GameOver:
      speed = ATTRACT_SPEED * 0.55f; dist += speed * dt; updateTilt(dt);
      break;
  }
}

// =====================================================================
// Projection helpers + drawing.
// =====================================================================
struct Frame { float roll, cR, sR, camX, camY, phaseSP; long gNear; };

Frame computeFrame() {
  Frame f;
  f.gNear = (long)floorf(dist / SP);
  f.phaseSP = dist - (float)f.gNear * SP;       // [0, SP)
  f.roll = ROLL_AMP * sinf(dist * ROLL_FREQ);
  f.cR = cosf(f.roll); f.sR = sinf(f.roll);
  f.camX = pathX(dist); f.camY = pathY(dist);
  return f;
}

void drawTunnel(uint16_t *fb, float dim) {
  Frame f = computeFrame();
  bool prevValid = false;
  int psx[NSEG], psy[NSEG];

  for (int i = 1; i <= NR; i++) {
    long  g = f.gNear + i;
    float z = (float)i * SP - f.phaseSP;
    if (z < Z_CLIP) { prevValid = false; continue; }
    float scale = F / z;
    float s   = (float)g * SP;
    float rcx = pathX(s) - f.camX;
    float rcy = pathY(s) - f.camY;

    int sx[NSEG], sy[NSEG];
    for (int k = 0; k < NSEG; k++) {
      float rx = baseCos[k] * f.cR - baseSin[k] * f.sR;
      float ry = baseCos[k] * f.sR + baseSin[k] * f.cR;
      sx[k] = (int)(CX + (rcx + RING_R * rx) * scale);
      sy[k] = (int)(CY + (rcy + RING_R * ry) * scale);
    }

    float t = (z - Z_HIT) / Z_FADE; if (t < 0) t = 0; if (t > 1) t = 1;
    float band = 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)g * 0.55f - dist * 1.8f));
    uint16_t base = scale565(lerp565(ORANGE_HOT, FAR_DARK, t), band * dim);

    bool obstacle = (g >= firstObsG) && (g % OBS_EVERY == 0);
    if (obstacle) {
      float gc = gapCenter(g), gh = gapHalf();
      for (int k = 0; k < NSEG; k++) {
        int k2 = (k + 1) % NSEG;
        bool gap = fabsf(wrapPi(midAng[k] - gc)) <= gh;
        if (gap) lineThickFb(fb, sx[k], sy[k], sx[k2], sy[k2], scale565(GAPCOL, dim), 1);
        else     lineThickFb(fb, sx[k], sy[k], sx[k2], sy[k2],
                             scale565(DANGER, dim * (0.65f + 0.35f * band)), 2);
      }
    } else {
      for (int k = 0; k < NSEG; k++) {
        int k2 = (k + 1) % NSEG;
        lineFb(fb, sx[k], sy[k], sx[k2], sy[k2], base);
      }
    }

    if (prevValid) {
      uint16_t rail = scale565(base, 0.75f);
      for (int k = 0; k < NSEG; k++) lineFb(fb, psx[k], psy[k], sx[k], sy[k], rail);
    }
    for (int k = 0; k < NSEG; k++) { psx[k] = sx[k]; psy[k] = sy[k]; }
    prevValid = true;
  }
}

void drawPlayer(Arduino_Canvas *cv, float dim) {
  Frame f = computeFrame();
  float scale = F / Z_PLAYER;
  float s   = dist + Z_PLAYER;
  float rcx = pathX(s) - f.camX, rcy = pathY(s) - f.camY;
  float a   = playerAngle + f.roll;
  float px  = CX + (rcx + RING_R * cosf(a)) * scale;
  float py  = CY + (rcy + RING_R * sinf(a)) * scale;
  float ccx = CX + rcx * scale, ccy = CY + rcy * scale;   // tunnel centre

  float inx = ccx - px, iny = ccy - py;
  float L = sqrtf(inx * inx + iny * iny); if (L < 1) L = 1;
  inx /= L; iny /= L;
  float tnx = -iny, tny = inx;                            // tangent

  const float SZ = 22, BASE = 12;
  int tipx = (int)(px + inx * SZ),   tipy = (int)(py + iny * SZ);
  int b1x  = (int)(px + tnx * BASE), b1y  = (int)(py + tny * BASE);
  int b2x  = (int)(px - tnx * BASE), b2y  = (int)(py - tny * BASE);
  // Glow then bright core.
  cv->fillTriangle((int)(px + inx * (SZ + 6)), (int)(py + iny * (SZ + 6)),
                   (int)(px + tnx * (BASE + 4)), (int)(py + tny * (BASE + 4)),
                   (int)(px - tnx * (BASE + 4)), (int)(py - tny * (BASE + 4)),
                   scale565(PLAYER_GLOW, dim));
  cv->fillTriangle(b1x, b1y, b2x, b2y, tipx, tipy, scale565(PLAYER_COL, dim));
}

void drawSpeedLines(uint16_t *fb) {
  if (speed < 3.6f) return;
  float sp = (speed - 3.6f) / (MAX_SPEED - 3.6f); if (sp > 1) sp = 1;
  int n = (int)(4 + sp * 9);
  for (int i = 0; i < n; i++) {
    float a  = (float)i * 2.39996f + dist * 3.0f;        // golden-angle spread
    float r1 = 66 + (i % 3) * 9;
    float r2 = r1 + 18 + 46 * sp;
    int x1 = (int)(CX + cosf(a) * r1), y1 = (int)(CY + sinf(a) * r1);
    int x2 = (int)(CX + cosf(a) * r2), y2 = (int)(CY + sinf(a) * r2);
    lineFb(fb, x1, y1, x2, y2, scale565(ORANGE_HOT, 0.35f + 0.45f * sp));
  }
}

void textCentered(Arduino_Canvas *cv, const char *s, int y, uint8_t size, uint16_t col) {
  int w = (int)strlen(s) * 6 * size;
  cv->setTextSize(size);
  cv->setTextColor(col);                 // transparent (bg == fg path)
  cv->setCursor((W - w) / 2, y);
  cv->print(s);
}

void drawHud(Arduino_Canvas *cv) {
  char buf[16]; snprintf(buf, sizeof(buf), "%lu", (unsigned long)score);
  textCentered(cv, buf, 6, 2, HUD_COL);
}

void drawBackHint(Arduino_Canvas *cv) {
  cv->setTextSize(2); cv->setTextColor(scale565(HUD_COL, 0.8f));
  cv->setCursor(8, 8); cv->print("<");
}

void drawTitle(Arduino_Canvas *cv) {
  textCentered(cv, "TUNNEL", 66, 3, ORANGE_HOT);
  textCentered(cv, "RACER",  98, 4, rgb565(255, 200, 150));
  textCentered(cv, "TILT TO STEER", 152, 1, HUD_COL);
  char b[24]; snprintf(b, sizeof(b), "BEST %lu", (unsigned long)best);
  textCentered(cv, b, 170, 1, scale565(HUD_COL, 0.85f));
  if ((millis() / 500) & 1) textCentered(cv, "TAP TO START", 212, 2, GAPCOL);
}

void drawGameOver(uint16_t *fb, Arduino_Canvas *cv) {
  fillRectFb(fb, 0, 70, W, 150, rgb565(8, 3, 14));   // dark readability band
  textCentered(cv, "CRASHED", 82, 3, DANGER);
  char b[24];
  snprintf(b, sizeof(b), "SCORE %lu", (unsigned long)score);
  textCentered(cv, b, 124, 2, rgb565(255, 220, 180));
  if (newBest) {
    if ((millis() / 350) & 1) textCentered(cv, "NEW BEST!", 152, 2, GAPCOL);
  } else {
    snprintf(b, sizeof(b), "BEST %lu", (unsigned long)best);
    textCentered(cv, b, 152, 2, scale565(HUD_COL, 0.85f));
  }
  if ((millis() / 500) & 1) textCentered(cv, "TAP TO RETRY", 198, 2, HUD_COL);
}

void drawShards(uint16_t *fb) {
  float prog = (float)(millis() - crashStartMs) / (float)CRASH_MS;
  if (prog > 1) prog = 1;
  uint16_t col = scale565(lerp565(ORANGE_HOT, FAR_DARK, prog), 1.0f - prog * 0.7f);
  for (int i = 0; i < shardCount; i++) {
    Shard &s = shards[i];
    float c = cosf(s.ang), sn = sinf(s.ang);
    float rx = s.hx * c - s.hy * sn, ry = s.hx * sn + s.hy * c;
    lineFb(fb, (int)(s.x - rx), (int)(s.y - ry),
               (int)(s.x + rx), (int)(s.y + ry), col);
  }
}

void triggerCrash() {
  phase = Phase::Crashing;
  crashStartMs = millis();
  newBest = false;
  if (score > best) { best = score; newBest = true;
                      if (prefsOpen) prefs.putUInt("best", best); }
  // Hard crash buzz — but kept inside the firmware's proven-safe motor envelope
  // (≤180 PWM / ~200 ms elsewhere). A full 255/280 ms pulse spikes motor current
  // hard enough to brown out the soft-latch rail on a low battery → reset on death.
  hapticBuzz(190, 170);

  // Seed shards from the nearest few rings' edges, blown outward from centre.
  rngState ^= (uint32_t)millis() * 2654435761u | 1u;
  Frame f = computeFrame();
  shardCount = 0;
  for (int i = 1; i <= 6 && shardCount < MAX_SHARDS; i++) {
    float z = (float)i * SP - f.phaseSP;
    if (z < Z_CLIP) continue;
    float scale = F / z, s = (float)(f.gNear + i) * SP;
    float rcx = pathX(s) - f.camX, rcy = pathY(s) - f.camY;
    int sx[NSEG], sy[NSEG];
    for (int k = 0; k < NSEG; k++) {
      float rx = baseCos[k] * f.cR - baseSin[k] * f.sR;
      float ry = baseCos[k] * f.sR + baseSin[k] * f.cR;
      sx[k] = (int)(CX + (rcx + RING_R * rx) * scale);
      sy[k] = (int)(CY + (rcy + RING_R * ry) * scale);
    }
    for (int k = 0; k < NSEG && shardCount < MAX_SHARDS; k++) {
      int k2 = (k + 1) % NSEG;
      Shard &sh = shards[shardCount++];
      sh.x = (sx[k] + sx[k2]) * 0.5f; sh.y = (sy[k] + sy[k2]) * 0.5f;
      sh.hx = (sx[k2] - sx[k]) * 0.5f; sh.hy = (sy[k2] - sy[k]) * 0.5f;
      float dx = sh.x - CX, dy = sh.y - CY;
      float L = sqrtf(dx * dx + dy * dy); if (L < 1) L = 1;
      float spd = frand(90, 260);
      sh.vx = dx / L * spd + frand(-40, 40);
      sh.vy = dy / L * spd + frand(-40, 40);
      sh.ang = 0; sh.spin = frand(-7, 7);
    }
  }
}

// =====================================================================
// Per-frame render dispatch.
// =====================================================================
void drawFrame(uint16_t *fb, Arduino_Canvas *cv) {
  memset(fb, 0, (size_t)W * H * 2);          // clear to pure black (fast)
  switch (phase) {
    case Phase::Title:
      drawTunnel(fb, 1.0f); drawPlayer(cv, 1.0f); drawTitle(cv); drawBackHint(cv);
      break;
    case Phase::Playing:
      drawTunnel(fb, 1.0f); drawPlayer(cv, 1.0f); drawSpeedLines(fb); drawHud(cv);
      break;
    case Phase::Crashing:
      drawShards(fb);
      break;
    case Phase::GameOver:
      drawTunnel(fb, 0.5f); drawGameOver(fb, cv); drawBackHint(cv);
      break;
  }
}

}  // namespace

// =====================================================================
// View interface.
// =====================================================================
void TunnelRacerView::onEnter() {
  openPrefs();
  resetTrig();
  phase = Phase::Title;
  dist = 0.0f; speed = ATTRACT_SPEED;
  playerAngle = targetAngle = 0.0f;
  firstObsG = NO_OBS; nextObsG = NO_OBS;     // no hazards in attract mode
  score = 0; bonusScore = 0; newBest = false;
  shardCount = 0;
  lastFrameMs = millis();
}

// Returns true if it switched away (caller must return from the loop).
static bool handleGameEvent(const Event &e) {
  if (e.type == EventType::ButtonShort || e.type == EventType::ButtonVeryLong) {
    switchTo(Screen::AppList); return true;
  }
  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return true; }
    if (phase == Phase::Title || phase == Phase::GameOver) { startGame(); }
    return false;
  }
  if (e.type == EventType::Gesture && e.gesture == Gesture::SingleTap) {
    if (phase == Phase::Title || phase == Phase::GameOver) startGame();
  }
  return false;
}

void TunnelRacerView::render() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) {
    // PSRAM exhausted — degrade gracefully to a static error screen. onEvent
    // (below) handles the back affordance in this fallback path.
    if (gfx) {
      ThemeColors t = theme();
      gfx->fillScreen(t.bg);
      gfx->setTextColor(RED, t.bg); gfx->setTextSize(2);
      gfx->setCursor(20, 120); gfx->print("no framebuffer");
      drawBackButton();
    }
    return;
  }
  uint16_t *fb = cv->getFramebuffer();
  lastFrameMs = millis();

  // The game's own frame loop. We stay here — feeding the watchdog and draining
  // input every frame — until the player leaves, so motion is never gated by
  // the model-dirty cadence.
  for (;;) {
    esp_task_wdt_reset();

    uint32_t now = millis();
    float dt = (now - lastFrameMs) / 1000.0f;
    lastFrameMs = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0f)  dt = 0.0f;

    Event e;
    while (eventQueue && xQueueReceive(eventQueue, &e, 0) == pdPASS) {
      if (handleGameEvent(e)) return;        // switched away — bail out
    }

    update(dt);
    drawFrame(fb, cv);
    cv->flush();

    vTaskDelay(pdMS_TO_TICKS(1));             // yield a tick for system health
  }
}

void TunnelRacerView::onEvent(const Event &e) {
  // Only reached in the no-canvas fallback (render() returns immediately).
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Touch && tappedBack(e.x, e.y))) {
    switchTo(Screen::AppList);
  }
}
