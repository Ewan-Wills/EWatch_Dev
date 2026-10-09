// StarFox-lite — on-rails wireframe space shooter. See starfox_shooter.h.
//
// Frame model: taskRender calls render() whenever the model is dirty, and
// taskIO bumps model.revision every 20 ms, so render() runs at ~20 Hz. We treat
// each render() call as one game tick and advance the simulation by a real
// dt (millis delta), so motion stays smooth regardless of the exact frame rate.
// All drawing goes into the shared PSRAM frame canvas and is flushed once per
// frame, so the fast vector motion is flicker-free.
#include "starfox_shooter.h"
#include "display.h"      // gfx, frameCanvas
#include "haptic.h"       // hapticBuzz
#include "model.h"        // model + ModelLock (tilt input)
#include "event.h"
#include "view.h"         // switchTo, Screen
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

// ----------------------------------------------------------------------------
// Tunables — everything that affects feel lives here.
// ----------------------------------------------------------------------------
static const int   W = 240, H = 280;
static const float CXf = 120.0f, CYf = 140.0f;
static const float F      = 300.0f;    // focal length (FOV); bigger = narrower
static const float NEARP  = 70.0f;     // near plane: objects past this hit/pass
static const float SPAWNZ = 1000.0f;   // spawn depth

// Tilt → reticle. Accel is raw 14-bit (~±4096 per g). We work relative to a
// baseline captured when play starts, so "however you hold it" becomes centre.
static const float TILT_SENS   = 0.085f;   // px of reticle travel per accel LSB
static const bool  TILT_INV_X  = true;     // flip if left/right feels reversed
static const bool  TILT_INV_Y  = true;     // flip if up/down feels reversed
static const float RETICLE_LERP = 0.45f;   // 0..1 smoothing toward tilt target

static const int   HP_MAX      = 100;      // health — lost when enemies get by you
static const int   SH_MAX      = 50;       // shield — absorbs direct (central) rams
static const float SH_REGEN    = 7.0f;     // shield pts/sec, regained after a lull
static const int   RING_HEAL   = 15;       // HP restored by flying through a ring
static const float WAVE_SECS    = 22.0f;   // seconds per wave
static const uint32_t ROLL_INVULN_MS = 700;
static const uint32_t ROLL_CD_MS     = 1100;
static const float AIM_TOL     = 13.0f;    // extra aim forgiveness (px)
static const float DANGER_W    = 64.0f;    // half-width of the cockpit hit-box
static const float DANGER_H    = 74.0f;

enum { T_FIGHTER = 0, T_ASTEROID = 1, T_RING = 2, T_BOSS = 3 };
enum { PH_TITLE = 0, PH_PLAYING = 1, PH_OVER = 2 };

static const int NSTAR  = 96;
static const int NOBJ   = 14;
static const int NPART  = 30;
static const int NLASER = 4;

// ----------------------------------------------------------------------------
// Colours (RGB565). Fixed neon-on-black palette — the game ignores the UI theme
// on purpose; that vector look is the whole point.
// ----------------------------------------------------------------------------
static constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static const uint16_t C_BG       = c565(0,   0,   6);
static const uint16_t C_ORANGE   = c565(232, 107, 43);   // brand — reticle/player
static const uint16_t C_LASER    = c565(255, 176, 64);
static const uint16_t C_FIGHTER  = c565(255, 64,  84);    // red threat
static const uint16_t C_ASTER    = c565(150, 200, 255);   // icy
static const uint16_t C_RING     = c565(80,  255, 150);   // green bonus
static const uint16_t C_BOSS     = c565(255, 72,  200);   // magenta
static const uint16_t C_STAR_F   = c565(60,  60,  92);
static const uint16_t C_STAR_M   = c565(140, 140, 185);
static const uint16_t C_STAR_N   = c565(235, 235, 255);
static const uint16_t C_HUD      = c565(180, 220, 255);
static const uint16_t C_HI       = c565(80,  230, 120);
static const uint16_t C_MD       = c565(240, 200, 60);
static const uint16_t C_LO       = c565(240, 70,  60);

// ----------------------------------------------------------------------------
// 3D wireframe models — local vertices (world units) + edge index pairs. Drawn
// with real per-vertex perspective so they genuinely foreshorten and tumble.
// ----------------------------------------------------------------------------
static const float FIGHTER_V[5][3] = {
  {  0,  0, -34 },   // 0 nose
  {  0, -9,  -4 },   // 1 canopy
  {-34,  7,  18 },   // 2 left wing
  { 34,  7,  18 },   // 3 right wing
  {  0,  0,  22 },   // 4 tail
};
static const uint8_t FIGHTER_E[][2] = {
  {0,1},{1,4},{0,2},{0,3},{2,4},{3,4},{2,3},{1,2},{1,3}
};
// Octahedron — used for asteroids and (scaled up) the boss.
static const float OCTA_V[6][3] = {
  { 1, 0, 0 }, {-1, 0, 0 }, { 0, 0, 1 }, { 0, 0,-1 }, { 0,-1, 0 }, { 0, 1, 0 }
};
static const uint8_t OCTA_E[][2] = {
  {0,2},{2,1},{1,3},{3,0},  {4,0},{4,2},{4,1},{4,3},  {5,0},{5,2},{5,1},{5,3}
};

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
struct Star { float x, y, z; };
struct Obj {
  bool    alive;
  uint8_t type;
  float   x, y, z, prevZ;
  float   yaw, pitch, yawSpd, pitchSpd;
  int16_t hp;
};
struct Particle { bool alive; float x, y, vx, vy, life; uint16_t col; };
struct Laser    { bool alive; float tx, ty; uint32_t born; };

struct Game {
  bool     inited = false;
  uint8_t  phase  = PH_TITLE;
  uint32_t lastMs = 0;

  float    reticleX = CXf, reticleY = CYf;
  int16_t  ax0 = 0, ay0 = 0;
  bool     calibrated = false;

  float    hp = HP_MAX, shield = SH_MAX;
  uint32_t lastHitMs = 0;
  uint32_t score  = 0;
  uint32_t high   = 0;
  bool     highLoaded = false;
  bool     newHigh = false;

  int      wave = 1;
  float    waveTimer = 0;
  float    spawnTimer = 0;
  bool     bossActive = false;

  uint32_t invulnUntil = 0;
  uint32_t rollStart = 0;
  int8_t   rollDir = 1;
  uint32_t rollCdUntil = 0;

  uint32_t waveBannerUntil = 0;
  uint32_t hitFlashUntil = 0;

  // Deferred second pulse → a real (gapped) double-buzz on a kill.
  uint32_t buzz2At = 0; uint8_t buzz2I = 0; uint16_t buzz2D = 0;

  Star     stars[NSTAR];
  Obj      objs[NOBJ];
  Particle parts[NPART];
  Laser    lasers[NLASER];
};
static Game G;

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------
static inline float frand(float lo, float hi) {
  return lo + (random(0, 10001) / 10000.0f) * (hi - lo);
}
static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
// Clamp coordinates before handing them to drawLine so a vertex projected just
// in front of the camera (huge |x|) can't overflow int16 inside the GFX lib.
static inline int16_t cl(float v) { return (int16_t)clampi((int)v, -2000, 2000); }

static void line(Arduino_GFX *g, float x0, float y0, float x1, float y1,
                 uint16_t col, bool glow) {
  g->drawLine(cl(x0), cl(y0), cl(x1), cl(y1), col);
  if (glow) {  // a 1px-offset second pass fattens close-up edges into neon
    g->drawLine(cl(x0 + 1), cl(y0), cl(x1 + 1), cl(y1), col);
    g->drawLine(cl(x0), cl(y0 + 1), cl(x1), cl(y1 + 1), col);
  }
}

static void textAt(Arduino_GFX *g, int x, int y, uint8_t size, uint16_t col,
                   const char *s) {
  g->setTextSize(size); g->setTextColor(col); g->setCursor(x, y); g->print(s);
}
static void textCenter(Arduino_GFX *g, int y, uint8_t size, uint16_t col,
                       const char *s) {
  int w = (int)strlen(s) * 6 * size;
  textAt(g, (W - w) / 2, y, size, col, s);
}

static uint16_t starColor(float z) {
  if (z < 280)  return C_STAR_N;
  if (z < 620)  return C_STAR_M;
  return C_STAR_F;
}

// ----------------------------------------------------------------------------
// 3D model drawing
// ----------------------------------------------------------------------------
static void drawModel(Arduino_GFX *g, const float (*v)[3], int nv,
                      const uint8_t (*e)[2], int ne,
                      float ox, float oy, float oz, float sizeMul,
                      float yaw, float pitch, uint16_t col) {
  float px[8], py[8]; bool ok[8];
  const float cy = cosf(yaw),   sy = sinf(yaw);
  const float cp = cosf(pitch), sp = sinf(pitch);
  for (int i = 0; i < nv; i++) {
    float lx = v[i][0] * sizeMul, ly = v[i][1] * sizeMul, lz = v[i][2] * sizeMul;
    // yaw about Y, then pitch about X
    float x1 =  lx * cy + lz * sy;
    float z1 = -lx * sy + lz * cy;
    float y1 =  ly;
    float y2 = y1 * cp - z1 * sp;
    float z2 = y1 * sp + z1 * cp;
    float wx = ox + x1, wy = oy + y2, wz = oz + z2;
    ok[i] = (wz > 5.0f);
    if (!ok[i]) wz = 5.0f;
    px[i] = CXf + wx * F / wz;
    py[i] = CYf + wy * F / wz;
  }
  bool glow = (oz < 320.0f);
  for (int i = 0; i < ne; i++) {
    int a = e[i][0], b = e[i][1];
    if (ok[a] && ok[b]) line(g, px[a], py[a], px[b], py[b], col, glow);
  }
}

// Screen-space centre + scale of an object (for hit-tests / flat shapes).
static void projectCenter(const Obj &o, float &sx, float &sy, float &scale) {
  float z = o.z < 6.0f ? 6.0f : o.z;
  scale = F / z;
  sx = CXf + o.x * scale;
  sy = CYf + o.y * scale;
}

static float objRadius(uint8_t type) {
  switch (type) {
    case T_FIGHTER:  return 34;
    case T_ASTEROID: return 28;
    case T_RING:     return 44;
    case T_BOSS:     return 86;
  }
  return 30;
}

// ----------------------------------------------------------------------------
// Spawning / waves
// ----------------------------------------------------------------------------
static Obj *freeObj() {
  for (int i = 0; i < NOBJ; i++) if (!G.objs[i].alive) return &G.objs[i];
  return nullptr;
}

static void spawnNormal() {
  Obj *o = freeObj();
  if (!o) return;
  // Triangular distribution clusters spawns toward the centre, so most enemies
  // are actually threatening (drift through the cockpit) rather than flying off
  // the edges.
  float x = (frand(-170, 170) + frand(-170, 170)) * 0.5f;
  float y = (frand(-150, 150) + frand(-150, 150)) * 0.5f;
  // Type weights drift with the wave: more rocks/rings appear later on.
  int roll = (int)frand(0, 100);
  uint8_t type = T_FIGHTER;
  int astChance  = clampi(18 + G.wave * 3, 18, 45);
  int ringChance = clampi(6  + G.wave,     6,  18);
  if (roll < ringChance)                 type = T_RING;
  else if (roll < ringChance + astChance) type = T_ASTEROID;

  o->alive = true; o->type = type;
  o->x = x; o->y = y; o->z = SPAWNZ + frand(-80, 120); o->prevZ = o->z;
  o->yaw = frand(0, 6.28f); o->pitch = frand(0, 6.28f);
  o->yawSpd   = frand(-1.6f, 1.6f);
  o->pitchSpd = (type == T_ASTEROID) ? frand(-1.8f, 1.8f) : 0.0f;
  o->hp = (type == T_ASTEROID) ? 2 : 1;
}

static void spawnBoss() {
  Obj *o = freeObj();
  if (!o) { G.bossActive = false; return; }
  o->alive = true; o->type = T_BOSS;
  o->x = 0; o->y = 0; o->z = 900; o->prevZ = o->z;
  o->yaw = 0; o->pitch = 0; o->yawSpd = 0.7f; o->pitchSpd = 0.25f;
  o->hp = 14 + G.wave;                 // ~ that many clean hits
  G.bossActive = true;
}

// ----------------------------------------------------------------------------
// Particles / lasers
// ----------------------------------------------------------------------------
static void burst(float sx, float sy, uint16_t col, int n, float spd) {
  for (int k = 0, made = 0; k < NPART && made < n; k++) {
    if (G.parts[k].alive) continue;
    float a = frand(0, 6.28f), v = frand(spd * 0.3f, spd);
    G.parts[k] = { true, sx, sy, cosf(a) * v, sinf(a) * v, 1.0f, col };
    made++;
  }
}

static void addLaser(float tx, float ty) {
  for (int i = 0; i < NLASER; i++) if (!G.lasers[i].alive) {
    G.lasers[i] = { true, tx, ty, millis() }; return;
  }
  G.lasers[0] = { true, tx, ty, millis() };   // overwrite oldest slot
}

// ----------------------------------------------------------------------------
// Game flow
// ----------------------------------------------------------------------------
static void loadHigh() {
  if (G.highLoaded) return;
  Preferences p;
  if (p.begin("starfox", true)) { G.high = p.getUInt("hi", 0); p.end(); }
  G.highLoaded = true;
}
static void saveHigh() {
  Preferences p;
  if (p.begin("starfox", false)) { p.putUInt("hi", G.high); p.end(); }
}

static void resetPlay() {
  G.phase = PH_PLAYING;
  G.score = 0; G.wave = 1;
  G.waveTimer = 0; G.spawnTimer = 0.6f; G.bossActive = false;
  G.hp = HP_MAX; G.shield = SH_MAX; G.lastHitMs = 0;
  G.calibrated = false; G.newHigh = false;
  G.invulnUntil = 0; G.rollCdUntil = 0; G.hitFlashUntil = 0;
  G.reticleX = CXf; G.reticleY = CYf;
  for (int i = 0; i < NOBJ; i++)   G.objs[i].alive  = false;
  for (int i = 0; i < NPART; i++)  G.parts[i].alive = false;
  for (int i = 0; i < NLASER; i++) G.lasers[i].alive = false;
  G.waveBannerUntil = millis() + 1600;
}

static void gameOver() {
  G.phase = PH_OVER;
  if (G.score > G.high) { G.high = G.score; G.newHigh = true; saveHigh(); }
  hapticBuzz(255, 320);
}

static void killScore(uint8_t type, float sx, float sy) {
  switch (type) {
    case T_FIGHTER:  G.score += 100; burst(sx, sy, C_FIGHTER, 10, 150); break;
    case T_ASTEROID: G.score += 150; burst(sx, sy, C_ASTER,   12, 170); break;
    case T_BOSS:     G.score += 2000; burst(sx, sy, C_BOSS,   24, 240); break;
  }
  // Real double-buzz: one now, a second scheduled ~90 ms later.
  hapticBuzz(170, 30);
  G.buzz2At = millis() + 90; G.buzz2I = 170; G.buzz2D = 30;
}

// A centred enemy rams the cockpit: the shield soaks it first, any overflow
// bleeds into health. The shield then pauses regen for a moment.
static void collidePlayer(uint8_t type) {
  int dmg = (type == T_BOSS) ? 50 : (type == T_ASTEROID) ? 24 : 18;
  G.lastHitMs = millis();
  if (G.shield >= dmg) {
    G.shield -= dmg;
  } else {
    float over = dmg - G.shield;
    G.shield = 0;
    G.hp -= over;
  }
  G.hitFlashUntil = millis() + 220;
  hapticBuzz(220, 110);
  if (G.hp <= 0) { G.hp = 0; gameOver(); }
}

// An enemy slips past without being shot — it "got by" you and chips health
// directly (the shield doesn't help against the ones you simply let through).
static void leakPlayer(uint8_t type) {
  int dmg = (type == T_ASTEROID) ? 10 : 7;
  G.hp -= dmg;
  G.hitFlashUntil = millis() + 160;
  hapticBuzz(150, 60);
  if (G.hp <= 0) { G.hp = 0; gameOver(); }
}

static void fireShot() {
  if (G.phase != PH_PLAYING) return;
  hapticBuzz(80, 12);
  addLaser(G.reticleX, G.reticleY);

  // Nearest shootable object whose projected disc covers the reticle.
  int best = -1; float bestD = 1e9f;
  for (int i = 0; i < NOBJ; i++) {
    Obj &o = G.objs[i];
    if (!o.alive || o.type == T_RING) continue;
    float sx, sy, sc; projectCenter(o, sx, sy, sc);
    float hitR = objRadius(o.type) * sc + AIM_TOL;
    float dx = sx - G.reticleX, dy = sy - G.reticleY;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < hitR && d < bestD) { bestD = d; best = i; }
  }
  if (best < 0) return;

  Obj &o = G.objs[best];
  o.hp--;
  float sx, sy, sc; projectCenter(o, sx, sy, sc);
  if (o.hp <= 0) {
    o.alive = false;
    killScore(o.type, sx, sy);
    if (o.type == T_BOSS) {
      G.bossActive = false;
      G.wave++; G.waveTimer = 0; G.waveBannerUntil = millis() + 1600;
    }
  } else {
    burst(sx, sy, C_ASTER, 5, 110);   // chipped (asteroid first hit / boss)
    G.score += 10;
    hapticBuzz(110, 16);
  }
}

static void startRoll(int8_t dir) {
  uint32_t now = millis();
  if (now < G.rollCdUntil) return;
  G.invulnUntil = now + ROLL_INVULN_MS;
  G.rollStart   = now;
  G.rollDir     = dir;
  G.rollCdUntil = now + ROLL_CD_MS;
  hapticBuzz(140, 40);
}

// ----------------------------------------------------------------------------
// Simulation
// ----------------------------------------------------------------------------
static void updateStars(float dt, float speed) {
  for (int i = 0; i < NSTAR; i++) {
    G.stars[i].z -= speed * dt;
    if (G.stars[i].z < 4) {
      G.stars[i].z = frand(400, 1000);
      G.stars[i].x = frand(-220, 220);
      G.stars[i].y = frand(-220, 220);
    }
  }
}

static void updatePlay(float dt) {
  uint32_t now = millis();

  // --- Tilt → reticle (relative to baseline captured on the first frame) ---
  int16_t ax, ay; { ModelLock lk; ax = model.ax; ay = model.ay; }
  if (!G.calibrated) { G.ax0 = ax; G.ay0 = ay; G.calibrated = true; }
  float tdx = (ax - G.ax0) * TILT_SENS * (TILT_INV_X ? -1 : 1);
  float tdy = (ay - G.ay0) * TILT_SENS * (TILT_INV_Y ? -1 : 1);
  float tgtX = CXf + tdx, tgtY = CYf + tdy;
  G.reticleX += (tgtX - G.reticleX) * RETICLE_LERP;
  G.reticleY += (tgtY - G.reticleY) * RETICLE_LERP;
  G.reticleX = (float)clampi((int)G.reticleX, 14, W - 14);
  G.reticleY = (float)clampi((int)G.reticleY, 54, H - 32);

  // --- Waves ---
  G.waveTimer += dt;
  if (!G.bossActive && G.waveTimer >= WAVE_SECS) {
    G.waveTimer = 0; G.wave++;
    G.waveBannerUntil = now + 1600;
    if (G.wave % 5 == 0) spawnBoss();
  }

  // --- Spawning ---
  if (!G.bossActive) {
    G.spawnTimer -= dt;
    if (G.spawnTimer <= 0) {
      spawnNormal();
      float base = 1.5f - G.wave * 0.11f;
      if (base < 0.42f) base = 0.42f;
      G.spawnTimer = frand(base * 0.7f, base * 1.3f);
    }
  }

  // --- Objects ---
  bool invuln = now < G.invulnUntil;
  float baseSpeed = 215 + G.wave * 28;
  for (int i = 0; i < NOBJ; i++) {
    Obj &o = G.objs[i];
    if (!o.alive) continue;
    o.prevZ = o.z;
    float spd = (o.type == T_BOSS) ? (46 + G.wave * 2) : baseSpeed;
    o.z -= spd * dt;
    o.yaw   += o.yawSpd * dt;
    o.pitch += o.pitchSpd * dt;
    if (o.z >= NEARP) continue;

    // Reached the camera plane — resolve and retire.
    float sx, sy, sc; projectCenter(o, sx, sy, sc);
    if (o.type == T_RING) {
      float r = objRadius(T_RING) * sc;
      float dx = sx - G.reticleX, dy = sy - G.reticleY;
      if (sqrtf(dx * dx + dy * dy) < r * 0.7f) {   // flew through the hoop
        G.score += 250;
        G.hp += RING_HEAL; if (G.hp > HP_MAX) G.hp = HP_MAX;
        burst(sx, sy, C_RING, 14, 150);
        hapticBuzz(150, 40);
      }
    } else {
      bool central = fabsf(sx - CXf) < DANGER_W && fabsf(sy - CYf) < DANGER_H;
      if (invuln)       burst(sx, sy, C_ORANGE, 8, 160);   // deflected on a roll
      else if (central) collidePlayer(o.type);             // rammed the cockpit
      else              leakPlayer(o.type);                 // slipped past → -HP
    }
    o.alive = false;
    if (o.type == T_BOSS) {
      // The boss rammed (or was dodged) past us without dying — clear the
      // boss state and move on, or normal spawns would stay suppressed forever
      // and a phantom HP bar would linger.
      G.bossActive = false;
      G.wave++; G.waveTimer = 0; G.waveBannerUntil = now + 1600;
    }
  }

  // --- Shield regen ---
  // The shield slowly rebuilds, but only after a short lull with no direct hit,
  // so taking rams is still punishing in the moment.
  if (now - G.lastHitMs > 1500 && G.shield < SH_MAX) {
    G.shield += SH_REGEN * dt;
    if (G.shield > SH_MAX) G.shield = SH_MAX;
  }

  // --- Particles ---
  for (int i = 0; i < NPART; i++) {
    Particle &p = G.parts[i];
    if (!p.alive) continue;
    p.x += p.vx * dt * 60; p.y += p.vy * dt * 60;
    p.life -= dt * 1.6f;
    if (p.life <= 0) p.alive = false;
  }

  // --- Deferred kill double-buzz ---
  if (G.buzz2At && now >= G.buzz2At) {
    hapticBuzz(G.buzz2I, G.buzz2D);
    G.buzz2At = 0;
  }
}

// ----------------------------------------------------------------------------
// Drawing
// ----------------------------------------------------------------------------
static void drawStars(Arduino_GFX *g, float shiftX) {
  for (int i = 0; i < NSTAR; i++) {
    Star &s = G.stars[i];
    float sc  = F / (s.z + 1.0f);
    float scP = F / (s.z + 26.0f);          // a step "behind" → streak length
    float x0 = CXf + s.x * scP + shiftX, y0 = CYf + s.y * scP;
    float x1 = CXf + s.x * sc   + shiftX, y1 = CYf + s.y * sc;
    g->drawLine(cl(x0), cl(y0), cl(x1), cl(y1), starColor(s.z));
  }
}

static void drawObj(Arduino_GFX *g, const Obj &o) {
  float sx, sy, sc; projectCenter(o, sx, sy, sc);
  switch (o.type) {
    case T_FIGHTER:
      drawModel(g, FIGHTER_V, 5, FIGHTER_E,
                sizeof(FIGHTER_E) / 2, o.x, o.y, o.z, 1.0f,
                o.yaw, o.pitch, C_FIGHTER);
      break;
    case T_ASTEROID:
      drawModel(g, OCTA_V, 6, OCTA_E,
                sizeof(OCTA_E) / 2, o.x, o.y, o.z, 26.0f,
                o.yaw, o.pitch, C_ASTER);
      break;
    case T_RING: {
      int r = (int)(objRadius(T_RING) * sc);
      if (r < 2) break;
      g->drawCircle(cl(sx), cl(sy), clampi(r, 2, 600), C_RING);
      if (r > 6) g->drawCircle(cl(sx), cl(sy), clampi(r - 4, 1, 600), C_RING);
      break;
    }
    case T_BOSS:
      drawModel(g, OCTA_V, 6, OCTA_E,
                sizeof(OCTA_E) / 2, o.x, o.y, o.z, 78.0f,
                o.yaw, o.pitch, C_BOSS);
      // Pulsing weak-point core.
      {
        int cr = clampi((int)(10 * sc), 2, 80);
        uint16_t cc = ((millis() / 120) & 1) ? C_LASER : C_BOSS;
        g->fillCircle(cl(sx), cl(sy), cr, cc);
      }
      break;
  }
}

static void drawLasers(Arduino_GFX *g) {
  uint32_t now = millis();
  for (int i = 0; i < NLASER; i++) {
    Laser &l = G.lasers[i];
    if (!l.alive) continue;
    if (now - l.born > 110) { l.alive = false; continue; }
    // Twin tracers converging from the lower corners to the aim point.
    line(g, 6,     H - 2, l.tx, l.ty, C_LASER, true);
    line(g, W - 6, H - 2, l.tx, l.ty, C_LASER, true);
  }
}

static void drawParticles(Arduino_GFX *g) {
  for (int i = 0; i < NPART; i++) {
    Particle &p = G.parts[i];
    if (!p.alive) continue;
    g->fillRect(cl(p.x), cl(p.y), 2, 2, p.col);
  }
}

static void drawReticle(Arduino_GFX *g) {
  uint32_t now = millis();
  bool invuln = now < G.invulnUntil;
  uint16_t col = invuln ? C_RING : C_ORANGE;
  int x = (int)G.reticleX, y = (int)G.reticleY;
  g->drawCircle(x, y, 9, col);
  g->drawLine(x - 14, y, x - 5, y, col);
  g->drawLine(x + 5, y, x + 14, y, col);
  g->drawLine(x, y - 14, x, y - 5, col);
  g->drawLine(x, y + 5, x, y + 14, col);
  g->drawPixel(x, y, col);
  if (invuln) {   // barrel-roll shield flare
    float p = (now - G.rollStart) / 500.0f; if (p > 1) p = 1;
    g->drawCircle(x, y, 9 + (int)(p * 14), C_RING);
  }
}

static void drawHUD(Arduino_GFX *g) {
  char buf[24];
  // Score / wave nudged down and in from the corners so they don't clip.
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)G.score);
  textAt(g, 10, 12, 2, C_HUD, buf);
  snprintf(buf, sizeof(buf), "W%d", G.wave);
  int w = (int)strlen(buf) * 12;
  textAt(g, W - w - 10, 12, 2, C_HUD, buf);

  // Two stacked bars along the bottom: shield (absorbs rams) over health.
  int bx = 26, bw = W - 52;
  int hy = H - 11, sy = H - 21;
  g->drawRect(bx - 1, sy - 1, bw + 2, 5 + 2, C_HUD);
  int sw = (int)((float)bw * G.shield / SH_MAX);
  if (sw > 0) g->fillRect(bx, sy, sw, 5, C_ASTER);
  textAt(g, 4, sy - 1, 1, C_ASTER, "SH");
  g->drawRect(bx - 1, hy - 1, bw + 2, 7 + 2, C_HUD);
  int hw = (int)((float)bw * G.hp / HP_MAX);
  uint16_t hc = (G.hp > 55) ? C_HI : (G.hp > 28) ? C_MD : C_LO;
  if (hw > 0) g->fillRect(bx, hy, hw, 7, hc);
  textAt(g, 4, hy - 1, 1, C_HUD, "HP");

  if (G.bossActive) {
    for (int i = 0; i < NOBJ; i++) if (G.objs[i].alive && G.objs[i].type == T_BOSS) {
      int hpmax = 14 + G.wave;
      int bhw = (int)((float)(W - 80) * G.objs[i].hp / hpmax);
      if (bhw < 0) bhw = 0;
      g->drawRect(39, 46, W - 80 + 2, 7, C_BOSS);
      g->fillRect(40, 47, bhw, 5, C_BOSS);
      textAt(g, 40, 58, 1, C_BOSS, "BOSS");
      break;
    }
  }

  if (millis() < G.waveBannerUntil) {
    char wb[16]; snprintf(wb, sizeof(wb), "WAVE %d", G.wave);
    textCenter(g, H / 2 - 8, 3, C_ORANGE, wb);
  }
  if (millis() < G.hitFlashUntil) {   // red vignette on taking a hit
    g->drawRect(0, 0, W, H, C_LO);
    g->drawRect(1, 1, W - 2, H - 2, C_LO);
    g->drawRect(2, 2, W - 4, H - 4, C_LO);
  }
}

static void drawTitle(Arduino_GFX *g) {
  textCenter(g, 70, 4, C_ORANGE, "STAR");
  textCenter(g, 108, 4, C_HUD, "FOX");
  textCenter(g, 150, 1, C_HUD, "TILT TO AIM  -  TAP TO FIRE");
  textCenter(g, 164, 1, C_HUD, "SWIPE TO BARREL ROLL");
  textCenter(g, 178, 1, C_LO,  "DON'T LET THEM GET BY");
  char buf[24]; snprintf(buf, sizeof(buf), "HIGH  %lu", (unsigned long)G.high);
  textCenter(g, 196, 1, C_HI, buf);
  if ((millis() / 500) & 1) textCenter(g, 224, 2, C_LASER, "TAP TO START");
}

static void drawGameOver(Arduino_GFX *g) {
  textCenter(g, 74, 3, C_FIGHTER, "GAME OVER");
  char buf[24];
  snprintf(buf, sizeof(buf), "SCORE  %lu", (unsigned long)G.score);
  textCenter(g, 120, 2, C_HUD, buf);
  if (G.newHigh) {
    if ((millis() / 400) & 1) textCenter(g, 148, 2, C_HI, "NEW HIGH!");
  } else {
    snprintf(buf, sizeof(buf), "HIGH  %lu", (unsigned long)G.high);
    textCenter(g, 148, 2, C_HI, buf);
  }
  if ((millis() / 500) & 1) textCenter(g, 200, 2, C_LASER, "TAP TO RETRY");
  textCenter(g, 232, 1, C_HUD, "BUTTON TO EXIT");
}

// ----------------------------------------------------------------------------
// View
// ----------------------------------------------------------------------------
void StarfoxShooterView::onEnter() {
  randomSeed(micros());
  loadHigh();
  for (int i = 0; i < NSTAR; i++) {
    G.stars[i].x = frand(-220, 220);
    G.stars[i].y = frand(-220, 220);
    G.stars[i].z = frand(20, 1000);
  }
  G.phase  = PH_TITLE;
  G.lastMs = millis();
  G.reticleX = CXf; G.reticleY = CYf;
  G.buzz2At = 0;
  G.inited = true;
}

void StarfoxShooterView::onExit() {}

void StarfoxShooterView::render() {
  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *g = cv ? (Arduino_GFX *)cv : gfx;
  if (!g) return;

  uint32_t now = millis();
  float dt = (now - G.lastMs) / 1000.0f;
  if (dt > 0.10f) dt = 0.10f;          // clamp after a stall / first frame
  if (dt < 0)     dt = 0;
  G.lastMs = now;

  float starSpeed = (G.phase == PH_PLAYING) ? (360 + G.wave * 22) : 150;
  updateStars(dt, starSpeed);
  if (G.phase == PH_PLAYING) updatePlay(dt);

  // Barrel-roll sweeps the starfield sideways for a sense of spin.
  float shiftX = 0;
  if (now < G.invulnUntil) {
    float p = (now - G.rollStart) / (float)ROLL_INVULN_MS;
    shiftX = G.rollDir * sinf(p * 3.1416f) * 46.0f;
  }

  g->fillScreen(C_BG);
  drawStars(g, shiftX);

  if (G.phase == PH_PLAYING) {
    // Draw far → near so closer objects overlay distant ones.
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < NOBJ; i++) {
        if (!G.objs[i].alive) continue;
        bool far = G.objs[i].z > 400;
        if ((pass == 0) == far) drawObj(g, G.objs[i]);
      }
    }
    drawParticles(g);
    drawLasers(g);
    drawReticle(g);
    drawHUD(g);
  } else if (G.phase == PH_TITLE) {
    drawTitle(g);
  } else {
    drawGameOver(g);
  }

  if (cv) cv->flush();
}

void StarfoxShooterView::onEvent(const Event &e) {
  // Hardware button is always "back" — exit to the launcher. (Swipe-right is
  // repurposed to a barrel roll for this screen in controller.cpp, so it no
  // longer maps to back here.)
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  if (e.type == EventType::Gesture) {
    if (G.phase == PH_PLAYING) {
      int8_t dir = (e.gesture == Gesture::SwipeLeft) ? -1 : 1;
      startRoll(dir);
    }
    return;
  }

  if (e.type == EventType::Touch) {
    if (G.phase == PH_PLAYING)      fireShot();
    else if (G.phase == PH_TITLE)   resetPlay();
    else                            resetPlay();   // game-over → retry
    return;
  }
}
