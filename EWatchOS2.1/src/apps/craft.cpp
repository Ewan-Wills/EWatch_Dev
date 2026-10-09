// Craft — see craft.h.
// The raycaster is the hot path: build this file for speed, not size.
#pragma GCC optimize ("O3", "fast-math")
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>

#include "craft.h"
#include "xbox_pad.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "ewsim.h"
#include "safe_canvas.h"

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
static const int WX = 64, WZ = 64, WY = 32, SEA = 9;
enum : uint8_t { AIR, GRASS, DIRT, STONE, COBBLE, PLANKS, LOG, LEAVES, SAND,
                 WATER, BRICK, BEDROCK, GLASS, COAL, SNOW, NBLOCK };
static uint8_t *gWorld = nullptr;            // nibbles, internal RAM
static bool gWorldMade = false;

static int gTopY = WY;                       // 1 + highest layer holding any block
// Coarse occupancy: 4x4x4-block cells holding anything at all. Rays jump
// across empty cells in one step (conservative: cleared blocks keep their
// cell marked, which only costs a little speed).
static uint8_t gOcc[WY / 4][WZ / 4][WX / 4];
static inline bool occ(int x, int y, int z) { return gOcc[y >> 2][z >> 2][x >> 2]; }
static inline uint8_t blk(int x, int y, int z) {
  if ((unsigned)x >= (unsigned)WX || (unsigned)z >= (unsigned)WZ || (unsigned)y >= (unsigned)WY) return AIR;
  uint32_t i = ((uint32_t)y << 12) | ((uint32_t)z << 6) | (uint32_t)x;   // WX=WZ=64
  uint8_t v = gWorld[i >> 1];
  return (i & 1) ? v >> 4 : v & 15;
}
static inline void setBlk(int x, int y, int z, uint8_t b) {
  if ((unsigned)x >= (unsigned)WX || (unsigned)z >= (unsigned)WZ || (unsigned)y >= (unsigned)WY) return;
  if (b != AIR) { if (y + 1 > gTopY) gTopY = y + 1; gOcc[y >> 2][z >> 2][x >> 2] = 1; }
  uint32_t i = ((uint32_t)y << 12) | ((uint32_t)z << 6) | (uint32_t)x;
  uint8_t &v = gWorld[i >> 1];
  v = (i & 1) ? (uint8_t)((v & 0x0F) | (b << 4)) : (uint8_t)((v & 0xF0) | b);
}
static inline bool solid(uint8_t b) { return b != AIR && b != WATER; }

static uint32_t hash3(int x, int y, int s) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
static float vnoise(float x, float z, int s) {          // smooth value noise 0..1
  int ix = (int)floorf(x), iz = (int)floorf(z);
  float fx = x - ix, fz = z - iz;
  fx = fx * fx * (3 - 2 * fx); fz = fz * fz * (3 - 2 * fz);
  float a = (hash3(ix, iz, s) & 0xFFFF) / 65535.0f, b = (hash3(ix + 1, iz, s) & 0xFFFF) / 65535.0f;
  float c = (hash3(ix, iz + 1, s) & 0xFFFF) / 65535.0f, d = (hash3(ix + 1, iz + 1, s) & 0xFFFF) / 65535.0f;
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

static int heightAt(int x, int z) {
  // Island: noise hills, falling into the sea towards the edges.
  float dx = (x - WX / 2) / (float)(WX / 2), dz = (z - WZ / 2) / (float)(WZ / 2);
  float edge = 1.0f - (dx * dx + dz * dz) * 0.9f;
  float h = 6 + vnoise(x / 14.0f, z / 14.0f, 1) * 10 + vnoise(x / 6.0f, z / 6.0f, 2) * 4;
  h = h * (0.35f + 0.65f * (edge < 0 ? 0 : edge)) + 3;
  int hi = (int)h;
  return hi < 2 ? 2 : hi > WY - 8 ? WY - 8 : hi;
}

static void makeWorld() {
  memset(gWorld, 0, WX * WY * WZ / 2);
  memset(gOcc, 0, sizeof(gOcc));
  gTopY = 0;
  for (int x = 0; x < WX; x++)
    for (int z = 0; z < WZ; z++) {
      int h = heightAt(x, z);
      for (int y = 0; y < h; y++) {
        uint8_t b = STONE;
        if (y == 0) b = BEDROCK;
        else if (y >= h - 1) b = h - 1 <= SEA + 1 ? SAND : (h > 20 ? SNOW : GRASS);
        else if (y >= h - 4) b = h - 1 <= SEA + 1 ? SAND : DIRT;
        else if ((hash3(x * 7 + y, z, 9) & 63) == 0) b = COAL;
        setBlk(x, y, z, b);
      }
      for (int y = h; y < SEA; y++) setBlk(x, y, z, WATER);
    }
  // Trees on grass, kept apart.
  for (int n = 0; n < 70; n++) {
    int x = 3 + hash3(n, 1, 5) % (WX - 6), z = 3 + hash3(n, 2, 5) % (WZ - 6);
    int h = heightAt(x, z);
    if (blk(x, h - 1, z) != GRASS || blk(x, h, z) != AIR) continue;
    int th = 4 + hash3(n, 3, 5) % 2;
    for (int y = h + th - 2; y <= h + th + 1; y++)
      for (int lx = -2; lx <= 2; lx++)
        for (int lz = -2; lz <= 2; lz++) {
          int r = abs(lx) + abs(lz) + (y > h + th ? 1 : 0);
          if (r <= 3 && blk(x + lx, y, z + lz) == AIR) setBlk(x + lx, y, z + lz, LEAVES);
        }
    for (int y = h; y < h + th; y++) setBlk(x, y, z, LOG);
  }
  // A little brick hut with glass windows near spawn: shows off the blocks.
  int cx = WX / 2 + 4, cz = WZ / 2 + 4, base = heightAt(cx, cz);
  for (int x = 0; x < 5; x++) for (int z = 0; z < 5; z++) for (int y = 0; y < 4; y++) {
    bool wall = x == 0 || x == 4 || z == 0 || z == 4;
    uint8_t b = y == 3 ? PLANKS : wall ? ((y == 1 && (x == 2 || z == 2)) ? GLASS : BRICK) : AIR;
    if (wall && x == 2 && z == 0 && y < 2) b = AIR;     // door
    setBlk(cx + x, base + y, cz + z, b);
  }
  gWorldMade = true;
}

// ---------------------------------------------------------------------------
// Textures: 16x16 RGB565, side / top / bottom per block. 0x0001 = clear.
// ---------------------------------------------------------------------------
static uint16_t (*gTex)[3][256] = nullptr;
static const uint16_t CLEAR = 0x0001;

static inline uint16_t rgb(int r, int g, int b) {
  r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline float tn(int x, int y, int s) { return (hash3(x, y, s) & 255) / 255.0f; }

static void makeTextures() {
  for (int b = 0; b < NBLOCK; b++)
    for (int f = 0; f < 3; f++)
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          float n = tn(x, y, b * 3 + f), m = 0.82f + 0.36f * n;
          uint16_t c = 0;
          switch (b) {
            case GRASS:
              if (f == 1) c = rgb(95 * m, 160 * m, 55 * m);
              else if (f == 2) c = rgb(134 * m, 96 * m, 67 * m);
              else c = (y < 3 + (int)(tn(x, 0, 77) * 2.5f)) ? rgb(95 * m, 160 * m, 55 * m) : rgb(134 * m, 96 * m, 67 * m);
              break;
            case DIRT:    c = rgb(134 * m, 96 * m, 67 * m); break;
            case STONE:   c = rgb(125 * m, 125 * m, 125 * m); break;
            case COBBLE: {
              bool crack = (hash3(x / 4 + (y / 4) * 7, y / 4, 31) & 3) == 0 || x % 4 == 0 || y % 4 == 0;
              c = crack ? rgb(80 * m, 80 * m, 80 * m) : rgb(140 * m, 140 * m, 140 * m); break;
            }
            case PLANKS: {
              float g = (y % 4 == 3) ? 0.62f : 0.9f + 0.15f * tn(x / 3, y, 41);
              c = rgb(170 * g * m * 0.95f, 135 * g * m * 0.95f, 82 * g * m * 0.95f); break;
            }
            case LOG:
              if (f != 0) { int r = (int)sqrtf((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
                            c = (r % 3 == 0) ? rgb(120, 90, 55) : rgb(170 * m, 135 * m, 85 * m); }
              else c = rgb(95 * (x % 3 == 0 ? 0.7f : 1.0f) * m, 74 * m, 45 * m);
              break;
            case LEAVES:  c = n < 0.18f ? rgb(30, 70, 25) : rgb(55 * m, 135 * m, 40 * m); break;
            case SAND:    c = rgb(220 * m, 208 * m, 160 * m); break;
            case WATER:   c = rgb(45 * m, 85 * m, 200 * m); break;
            case BRICK: {
              bool mortar = (y % 4 == 3) || ((x + ((y / 4) & 1) * 4) % 8 == 7);
              c = mortar ? rgb(190 * m, 185 * m, 175 * m) : rgb(155 * m, 65 * m, 50 * m); break;
            }
            case BEDROCK: c = rgb(70 * (0.5f + n), 70 * (0.5f + n), 70 * (0.5f + n)); break;
            case GLASS: {
              bool frame = x == 0 || y == 0 || x == 15 || y == 15;
              bool streak = (x == y && x > 3 && x < 8) || (x == y + 1 && x > 4 && x < 9);
              c = frame ? rgb(200, 225, 235) : streak ? rgb(235, 245, 250) : CLEAR; break;
            }
            case COAL:    c = (n > 0.8f) ? rgb(30, 30, 30) : rgb(125 * m, 125 * m, 125 * m); break;
            case SNOW:    c = f == 0 && y > 3 ? rgb(134 * m, 96 * m, 67 * m) : rgb(240 * (0.95f + 0.05f * n), 245, 250); break;
            default:      c = rgb(255, 0, 255); break;
          }
          if (c == CLEAR && b != GLASS) c = 0x0020;
          gTex[b][f][y * 16 + x] = c;
        }
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
static float pX, pY, pZ, pVy = 0, yaw = 0.6f, pitch = -0.15f;
static bool  onGround = false, flying = false, showFps = false;
static const uint8_t kHotbar[9] = { GRASS, DIRT, STONE, COBBLE, PLANKS, LOG, LEAVES, BRICK, GLASS };
static int   gSel = 0;
static const float EYE = 1.62f, HALF = 0.3f, TALL = 1.8f;

static bool hitsBox(float x, float y, float z) {
  for (int bx = (int)floorf(x - HALF); bx <= (int)floorf(x + HALF); bx++)
    for (int by = (int)floorf(y); by <= (int)floorf(y + TALL - 0.01f); by++)
      for (int bz = (int)floorf(z - HALF); bz <= (int)floorf(z + HALF); bz++)
        if (solid(blk(bx, by, bz))) return true;
  return false;
}

static void spawn() {
  pX = WX / 2 + 0.5f; pZ = WZ / 2 + 0.5f;
  int y = WY - 1;
  while (y > 0 && !solid(blk((int)pX, y - 1, (int)pZ))) y--;
  pY = y + 0.01f; pVy = 0; flying = false;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
static const int RW = 120, RH = 140;
static const float FOGD = 20.0f;
static float gSx[RW], gSy[RH];
struct Frame {
  float ox, oy, oz;
  float fx, fy, fz, rx, ry, rz, ux, uy, uz;
  int   tx, ty, tz; bool target;
  uint16_t *fb;                           // 240x280 canvas framebuffer
};
static Frame gF;
static SemaphoreHandle_t gGo = nullptr, gDone = nullptr;
static TaskHandle_t gHelper = nullptr;
static volatile bool gHelperRun = false;

static inline uint16_t shade565(uint16_t c, int k) {     // k: 0..256
  int r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t mix565(uint16_t a, uint16_t b, int k) {   // k: 0 = a, 256 = b
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + ((br - ar) * k >> 8)) << 11) | ((ag + ((bg - ag) * k >> 8)) << 5) | (ab + ((bb - ab) * k >> 8)));
}
static inline uint16_t skyColor(float dy) {
  float t = dy < 0 ? 0 : dy > 1 ? 1 : dy;
  return rgb(150 - 90 * t, 190 - 80 * t, 255 - 30 * t);
}
static const int kFaceShade[4] = { 205, 256, 140, 230 };  // X, top, bottom, Z

static inline float rsqrtFast(float x) {              // 1/sqrt, one Newton step (~0.2%)
  union { float f; uint32_t i; } u = { x };
  u.i = 0x5f375a86 - (u.i >> 1);
  return u.f * (1.5f - 0.5f * x * u.f * u.f);
}
// floorf() and float division are library calls here (~70 cycles each).
static inline int ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
static inline float recipAbs(float x) {               // |1/x| via rsqrt(x^2), 2 Newton steps
  float x2 = x * x;
  union { float f; uint32_t i; } u = { x2 };
  u.i = 0x5f375a86 - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - 0.5f * x2 * y * y);
  y = y * (1.5f - 0.5f * x2 * y * y);
  return y;
}
static uint16_t gSkyLut[65];                          // sky colour by ray dy (0..1 in 64 steps)
static uint16_t gFogCol;
static void initSky() {
  for (int i = 0; i <= 64; i++) { float t = i / 64.0f; gSkyLut[i] = rgb(150 - 90 * t, 190 - 80 * t, 255 - 30 * t); }
  gFogCol = gSkyLut[0];
}
static inline uint16_t skyFast(float dy) { int i = (int)(dy * 64); return gSkyLut[i < 0 ? 0 : i > 64 ? 64 : i]; }

static uint16_t castRay(const Frame &F, float dx, float dy, float dz) {
  float inv = rsqrtFast(dx * dx + dy * dy + dz * dz);
  dx *= inv; dy *= inv; dz *= inv;
  int ix = ifloor(F.ox), iy = ifloor(F.oy), iz = ifloor(F.oz);
  int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
  float tdx = fabsf(dx) > 1e-6f ? recipAbs(dx) : 1e9f, tdy = fabsf(dy) > 1e-6f ? recipAbs(dy) : 1e9f, tdz = fabsf(dz) > 1e-6f ? recipAbs(dz) : 1e9f;
  float tmx = (dx > 0 ? (ix + 1 - F.ox) : (F.ox - ix)) * tdx;
  float tmy = (dy > 0 ? (iy + 1 - F.oy) : (F.oy - iy)) * tdy;
  float tmz = (dz > 0 ? (iz + 1 - F.oz) : (F.oz - iz)) * tdz;
  float t = 0;
  int face = 0;
  uint16_t water = 0; float waterT = -1;
  uint8_t inside = blk(ix, iy, iz);
  bool jumped = false;
  for (int n = 0; n < 72; n++) {
    if (!jumped) {
      if (tmx < tmy) {
        if (tmx < tmz) { ix += sx; t = tmx; tmx += tdx; face = 0; }
        else           { iz += sz; t = tmz; tmz += tdz; face = 3; }
      } else {
        if (tmy < tmz) { iy += sy; t = tmy; tmy += tdy; face = 1; }
        else           { iz += sz; t = tmz; tmz += tdz; face = 3; }
      }
    }
    jumped = false;
    if (t > FOGD) break;
    if ((unsigned)ix >= (unsigned)WX || (unsigned)iz >= (unsigned)WZ) {
      // Off the island: endless sea below SEA, sky above.
      if (dy < 0) {
        float ts = (SEA - F.oy) / dy;
        if (ts > 0 && ts < FOGD) { int k = (int)(ts / FOGD * 256); uint16_t sea = rgb(40, 80, 190);
          return mix565(sea, gFogCol, k); }
      }
      break;
    }
    if (iy < 0) { return rgb(20, 20, 20); }
    if (iy >= gTopY) { if (sy > 0) break; continue; }   // above all terrain: sky
    uint8_t b = blk(ix, iy, iz);
    if (b == AIR) {
      inside = AIR;
      if (iy >= 0 && !occ(ix, iy, iz)) {
        // Empty 4x4x4 cell: jump to where the ray leaves it.
        int cx = ix & ~3, cy = iy & ~3, cz = iz & ~3;
        float ex = (sx > 0 ? (cx + 4 - F.ox) : (F.ox - cx)) * tdx;
        float ey = (sy > 0 ? (cy + 4 - F.oy) : (F.oy - cy)) * tdy;
        float ez = (sz > 0 ? (cz + 4 - F.oz) : (F.oz - cz)) * tdz;
        float te = ex; face = 0;
        if (ey < te) { te = ey; face = 1; }
        if (ez < te) { te = ez; face = 3; }
        if (te > t) {
          t = te;
          float tp = te + 1e-3f;
          ix = ifloor(F.ox + dx * tp); iy = ifloor(F.oy + dy * tp); iz = ifloor(F.oz + dz * tp);
          tmx = (dx > 0 ? (ix + 1 - F.ox) : (F.ox - ix)) * tdx;
          tmy = (dy > 0 ? (iy + 1 - F.oy) : (F.oy - iy)) * tdy;
          tmz = (dz > 0 ? (iz + 1 - F.oz) : (F.oz - iz)) * tdz;
          jumped = true;
        }
      }
      continue;
    }
    if (b == WATER) {                    // see-through: remember the surface, keep going
      if (waterT < 0 && inside != WATER) { waterT = t; water = rgb(40, 85, 200); }
      inside = WATER;
      continue;
    }
    float hx = F.ox + dx * t, hy = F.oy + dy * t, hz = F.oz + dz * t;
    float u, v;
    int fi;                              // texture face: 0 side, 1 top, 2 bottom
    if (face == 0)      { u = hz; v = hy; fi = 0; }
    else if (face == 3) { u = hx; v = hy; fi = 0; }
    else                { u = hx; v = hz; fi = sy < 0 ? 1 : 2; }
    u -= (float)ifloor(u); v -= (float)ifloor(v);
    int tu = (int)(u * 16) & 15, tv = fi == 0 ? 15 - ((int)(v * 16) & 15) : ((int)(v * 16) & 15);
    uint16_t c = gTex[b][fi][tv * 16 + tu];
    if (c == CLEAR) continue;            // glass pane hole
    int sh = kFaceShade[face == 1 ? (sy < 0 ? 1 : 2) : face];
    c = shade565(c, sh);
    if (F.target && ix == F.tx && iy == F.ty && iz == F.tz &&
        (u < 0.06f || u > 0.94f || v < 0.06f || v > 0.94f)) c = rgb(20, 20, 20);
    int fog = (int)(t * t * (256.0f / (FOGD * FOGD)));
    c = mix565(c, gFogCol, fog);
    if (waterT >= 0) c = mix565(c, water, 150);
    return c;
  }
  uint16_t sky = skyFast(dy);
  return waterT >= 0 ? mix565(sky, water, 170) : sky;
}

static volatile int gNextRow = 0;               // rows handed out to both cores
static portMUX_TYPE gRowMux = portMUX_INITIALIZER_UNLOCKED;
static void renderRows(int r0, int r1);
static volatile uint32_t gRowsByCore[2] = {0, 0};
static void renderShared() {                     // pull 2-row chunks until the frame is done
  for (;;) {
    int r;
    portENTER_CRITICAL(&gRowMux);
    r = gNextRow; gNextRow = r + 2;
    portEXIT_CRITICAL(&gRowMux);
    if (r >= RH) return;
    renderRows(r, r + 2 > RH ? RH : r + 2);
    gRowsByCore[xPortGetCoreID()] += 2;
    taskYIELD();                                 // let the console (same prio, core 0) stream snaps
  }
}

static void renderRows(int r0, int r1) {
  const Frame &F = gF;
  for (int r = r0; r < r1; r++) {
    float vy = gSy[r];
    float bx = F.fx + F.ux * vy, by = F.fy + F.uy * vy, bz = F.fz + F.uz * vy;
    uint16_t *row0 = F.fb + (r * 2) * 240, *row1 = row0 + 240;
    for (int c = 0; c < RW; c++) {
      float hx = gSx[c];
      // Stored big-endian (panel wire order): the frame goes out with one
      // raw byte copy instead of a per-pixel swap from PSRAM.
      uint16_t px = __builtin_bswap16(castRay(F, bx + F.rx * hx, by + F.ry * hx, bz + F.rz * hx));
      row0[c * 2] = px; row0[c * 2 + 1] = px; row1[c * 2] = px; row1[c * 2 + 1] = px;
    }
  }
}

static void helperTask(void *) {
  while (gHelperRun) {
    if (xSemaphoreTake(gGo, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    renderShared();
    xSemaphoreGive(gDone);
  }
  gHelper = nullptr;
  vTaskDelete(nullptr);
}

// One-shot micro-benchmark (logged on entry): where does a ray's time go?
static void benchRays(uint16_t *fb) {
  const Frame &F = gF;
  const int N = 8400;
  volatile float sink = 0; volatile uint16_t sinkc = 0;
  uint32_t t0 = micros();
  for (int i = 0; i < N; i++) { int r = (i / 120) % 140, c = i % 120; uint16_t *p = fb + r * 2 * 240 + c * 2; p[0] = p[1] = p[240] = p[241] = (uint16_t)i; }
  uint32_t t1 = micros();
  for (int i = 0; i < N; i++) { float dx = F.fx + gSx[i % RW], dy = F.fy + gSy[(i / RW) % RH], dz = F.fz;
    float inv = rsqrtFast(dx * dx + dy * dy + dz * dz); dx *= inv; dy *= inv; dz *= inv;
    sink = sink + fabsf(1.0f / dx) + fabsf(1.0f / dy) + fabsf(1.0f / dz); }
  uint32_t t2 = micros();
  for (int i = 0; i < N; i++) { float v = F.ox + i * 0.001f; sink = sink + floorf(v) + floorf(v + 0.3f) + floorf(v + 0.6f); }
  uint32_t t3 = micros();
  for (int i = 0; i < N; i++) { float h = gSx[i % RW], v = gSy[(i / RW) % RH];
    sinkc = castRay(F, F.fx + F.ux * v + F.rx * h, F.fy + F.uy * v + F.ry * h, F.fz + F.uz * v + F.rz * h); }
  uint32_t t4 = micros();
  EWLOGI("CRAFT", "bench us/ray*100: fbwrite=%lu setup=%lu floor3=%lu castRay=%lu",
         (t1 - t0) * 100 / N, (t2 - t1) * 100 / N, (t3 - t2) * 100 / N, (t4 - t3) * 100 / N);
  (void)sink; (void)sinkc;
}

// Ray for mining/placing: first solid block within reach + the empty cell before it.
static bool pickBlock(int &bx, int &by, int &bz, int &px, int &py, int &pz) {
  float ex = pX, ey = pY + EYE, ez = pZ;
  float dx = sinf(yaw) * cosf(pitch), dy = sinf(pitch), dz = cosf(yaw) * cosf(pitch);
  int ix = (int)floorf(ex), iy = (int)floorf(ey), iz = (int)floorf(ez);
  int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
  float tdx = dx != 0 ? fabsf(1 / dx) : 1e9f, tdy = dy != 0 ? fabsf(1 / dy) : 1e9f, tdz = dz != 0 ? fabsf(1 / dz) : 1e9f;
  float tmx = (dx > 0 ? (ix + 1 - ex) : (ex - ix)) * tdx;
  float tmy = (dy > 0 ? (iy + 1 - ey) : (ey - iy)) * tdy;
  float tmz = (dz > 0 ? (iz + 1 - ez) : (ez - iz)) * tdz;
  px = ix; py = iy; pz = iz;
  for (int n = 0; n < 20; n++) {
    float t;
    if (tmx < tmy && tmx < tmz) { ix += sx; t = tmx; tmx += tdx; }
    else if (tmy < tmz)         { iy += sy; t = tmy; tmy += tdy; }
    else                        { iz += sz; t = tmz; tmz += tdz; }
    if (t > 5.0f) return false;
    uint8_t b = blk(ix, iy, iz);
    if (b != AIR && b != WATER) { bx = ix; by = iy; bz = iz; return true; }
    px = ix; py = iy; pz = iz;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Input (controller + touch) and game update
// ---------------------------------------------------------------------------
static uint32_t gLastMs = 0, gLastInputMs = 0, gFpsT0 = 0, gFpsFrames = 0;
static float    gFps = 0;
static uint16_t gPrevBtn = 0;
static bool     gPrevRT = false, gPrevLT = false;
static uint32_t gRepeatMs = 0;
// Touch
static bool  tDown = false, tMove = false;
static int16_t tX0, tY0, tLX, tLY;
static float tStickX = 0, tStickY = 0;
static volatile bool tReqMine = false, tReqPlace = false, tReqJump = false;
static volatile int  tReqSel = -1;
static volatile float tLookDX = 0, tLookDY = 0;
static volatile bool gQuit = false;

static void mine() {
  int bx, by, bz, px, py, pz;
  if (!pickBlock(bx, by, bz, px, py, pz) || by == 0) return;
  setBlk(bx, by, bz, AIR);
  hapticBuzz(40, 25); padRumble(25, 60);
}
static void place() {
  int bx, by, bz, px, py, pz;
  if (!pickBlock(bx, by, bz, px, py, pz)) return;
  setBlk(px, py, pz, kHotbar[gSel]);
  if (hitsBox(pX, pY, pZ)) { setBlk(px, py, pz, AIR); return; }   // not inside yourself
  hapticBuzz(30, 20);
}

static void update(float dt) {
  PadState p = padGet();
  float mx = 0, mz = 0, lookX = 0, lookY = 0;
  bool jump = false, down = false, sprint = false;
  if (p.connected) {
    mx = p.lx; mz = -p.ly; lookX = p.rx; lookY = -p.ry;
    jump = p.buttons & PAD_A; down = p.buttons & PAD_B; sprint = p.buttons & PAD_LS;
    uint16_t pressed = p.buttons & ~gPrevBtn;
    if (pressed & (PAD_RB | PAD_RIGHT)) gSel = (gSel + 1) % 9;
    if (pressed & (PAD_LB | PAD_LEFT))  gSel = (gSel + 8) % 9;
    if (pressed & PAD_Y) flying = !flying, pVy = 0;
    if (pressed & PAD_VIEW) showFps = !showFps;
    if (pressed & PAD_MENU) gQuit = true;
    gPrevBtn = p.buttons;
    bool rt = p.rt > 0.5f, lt = p.lt > 0.5f;
    uint32_t now = millis();
    if ((rt && !gPrevRT) || (rt && now - gRepeatMs > 250)) { if (rt) { mine(); gRepeatMs = now; } }
    if ((lt && !gPrevLT) || (lt && now - gRepeatMs > 250)) { if (lt) { place(); gRepeatMs = now; } }
    gPrevRT = rt; gPrevLT = lt;
    if (p.lx || p.ly || p.rx || p.ry || p.buttons || rt || lt) gLastInputMs = millis();
  }
  // Touch
  mx += tStickX; mz += -tStickY;
  yaw += tLookDX * 0.012f; pitch -= tLookDY * 0.012f; tLookDX = 0; tLookDY = 0;
  if (tReqMine) { tReqMine = false; mine(); }
  if (tReqPlace) { tReqPlace = false; place(); }
  if (tReqJump) { tReqJump = false; jump = true; }
  if (tReqSel >= 0) { gSel = tReqSel; tReqSel = -1; }
  // Attract mode: nobody playing.
  if (!p.connected && millis() - gLastInputMs > 6000) lookX = 0.18f;

  yaw += lookX * 2.6f * dt;
  pitch += lookY * 2.0f * dt;
  if (pitch > 1.5f) pitch = 1.5f; if (pitch < -1.5f) pitch = -1.5f;

  float speed = (sprint ? 6.5f : 4.3f) * (flying ? 1.6f : 1.0f);
  float sy = sinf(yaw), cy = cosf(yaw);
  float vx = (sy * mz + cy * mx) * speed, vz = (cy * mz - sy * mx) * speed;
  bool inWater = blk((int)floorf(pX), (int)floorf(pY + 0.5f), (int)floorf(pZ)) == WATER;
  if (inWater) { vx *= 0.6f; vz *= 0.6f; }
  if (flying) pVy = (jump ? 6.0f : 0) - (down ? 6.0f : 0);
  else if (inWater) { pVy -= 6.0f * dt; if (jump) pVy = 3.0f; if (pVy < -3) pVy = -3; }
  else { pVy -= 24.0f * dt; if (jump && onGround) pVy = 7.6f; if (pVy < -30) pVy = -30; }

  float nx = pX + vx * dt;
  if (nx < HALF) nx = HALF; if (nx > WX - HALF) nx = WX - HALF;
  if (!hitsBox(nx, pY, pZ)) pX = nx;
  float nz = pZ + vz * dt;
  if (nz < HALF) nz = HALF; if (nz > WZ - HALF) nz = WZ - HALF;
  if (!hitsBox(pX, pY, nz)) pZ = nz;
  float ny = pY + pVy * dt;
  onGround = false;
  if (ny < 0.01f) { ny = 0.01f; pVy = 0; onGround = true; }
  if (hitsBox(pX, ny, pZ)) {
    if (pVy < 0) { onGround = true; ny = floorf(ny) + 1.0f + 0.001f; if (hitsBox(pX, ny, pZ)) ny = pY; }
    else ny = pY;
    pVy = 0;
  }
  if (ny > WY + 4) ny = WY + 4;
  pY = ny;
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
static inline uint16_t be(uint16_t c) { return __builtin_bswap16(c); }   // HUD colours, wire order
static void drawHud(Arduino_Canvas *cv, bool padOn) {
  // Crosshair.
  cv->drawFastHLine(113, 140, 15, be(WHITE)); cv->drawFastVLine(120, 133, 15, be(WHITE));
  // Hotbar: 9 slots of 24 px.
  const int16_t y = 252, x0 = (240 - 9 * 24) / 2;
  cv->fillRect(x0 - 2, y - 2, 9 * 24 + 4, 28, be(rgb(20, 20, 20)));
  for (int i = 0; i < 9; i++) {
    int16_t x = x0 + i * 24;
    const uint16_t *t = gTex[kHotbar[i]][0];
    for (int ty = 0; ty < 16; ty++)
      for (int tx = 0; tx < 16; tx++) {
        uint16_t c = t[ty * 16 + tx];
        cv->drawPixel(x + 4 + tx, y + 4 + ty, be(c == CLEAR ? rgb(60, 60, 60) : c));
      }
    if (i == gSel) { cv->drawRect(x, y, 24, 24, be(WHITE)); cv->drawRect(x + 1, y + 1, 22, 22, be(WHITE)); }
  }
  cv->setTextSize(1);
  if (showFps) {
    char b[24]; snprintf(b, sizeof b, "%.1f fps", gFps);
    cv->setTextColor(be(WHITE)); cv->setCursor(4, 4); cv->print(b);
  }
  if (flying) { cv->setTextColor(be(YELLOW)); cv->setCursor(200, 4); cv->print("FLY"); }
  if (!padOn && millis() - gLastInputMs > 6000) {
    cv->setTextColor(be(WHITE));
    cv->setCursor(22, 20); cv->print("Connect an Xbox controller");
    cv->setCursor(40, 32); cv->print("in the Xbox Pad app");
  }
}

// ---------------------------------------------------------------------------
// Double-buffered output: core 1 pushes frame N to the panel (~35 ms of CPU-
// driven SPI) while core 0 is already ray-casting frame N+1; core 1 joins the
// row queue once the push is done. The HUD canvas is re-pointed at whichever
// buffer is being finished.
// ---------------------------------------------------------------------------
class HudCanvas : public SafeCanvas {
public:
  using SafeCanvas::SafeCanvas;
  void setFB(uint16_t *p) { _framebuffer = p; }
};
static HudCanvas *gHud = nullptr;
static uint16_t  *gBuf[2] = { nullptr, nullptr };
static int  gCur = 0;
static bool gHavePrev = false;

static void blitFrame(uint16_t *fb) {
  // The ewsim display tap (test snapshots) costs ~15 ms per full frame:
  // mirror every 4th frame only; the others are just counted (fps checks).
  static uint8_t tapN = 0;
  bool skipTap = (++tapN & 3) != 0;
  if (skipTap) ewsimTapPause(true);
  gfx->draw16bitBeRGBBitmap(0, 0, fb, 240, 280);   // big-endian: raw byte copy
  if (skipTap) ewsimTapPause(false);
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
void CraftView::onEnter() {
  if (!gWorld) {
    gWorld = (uint8_t *)heap_caps_malloc(WX * WY * WZ / 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gWorld) gWorld = (uint8_t *)ps_malloc(WX * WY * WZ / 2);
  }
  if (!gTex) {
    gTex = (uint16_t (*)[3][256])heap_caps_malloc(sizeof(uint16_t) * NBLOCK * 3 * 256, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gTex) gTex = (uint16_t (*)[3][256])ps_malloc(sizeof(uint16_t) * NBLOCK * 3 * 256);
  }
  for (int i = 0; i < 2; i++)
    if (!gBuf[i]) gBuf[i] = (uint16_t *)heap_caps_aligned_alloc(16, 240 * 280 * 2, MALLOC_CAP_SPIRAM);
  if (!gHud && gfx) { gHud = new HudCanvas(240, 280, gfx, 0, 0); gHud->setFB(gBuf[0]); gHud->begin(GFX_SKIP_OUTPUT_BEGIN); }
  gHavePrev = false;
  if (!gWorld || !gTex || !gBuf[0] || !gBuf[1] || !gHud) { EWLOGE("CRAFT", "out of memory"); return; }
  initSky();
  if (!gWorldMade) { uint32_t t0 = millis(); makeTextures(); makeWorld(); spawn();
                     EWLOGI("CRAFT", "world %dx%dx%d made in %lu ms", WX, WZ, WY, (unsigned long)(millis() - t0)); }
  const float hfov = 0.72f, vfov = hfov * RH / RW;
  for (int c = 0; c < RW; c++) gSx[c] = ((c + 0.5f) / RW * 2 - 1) * hfov;
  for (int r = 0; r < RH; r++) gSy[r] = (1 - (r + 0.5f) / RH * 2) * vfov;
  if (!gGo) { gGo = xSemaphoreCreateBinary(); gDone = xSemaphoreCreateBinary(); }
  gHelperRun = true;
  int other = xPortGetCoreID() == 0 ? 1 : 0;
  // Priority 1 = same as the ewlog console on core 0: the helper now renders
  // almost continuously (it overlaps the panel push), and at 2 it starved the
  // console (broken snapshots/log). Equal priority time-slices them.
  if (!gHelper) xTaskCreatePinnedToCore(helperTask, "craftR", 4096, nullptr, 1, &gHelper, other);
  gLastMs = millis(); gLastInputMs = millis(); gFpsT0 = millis(); gFpsFrames = 0;
  gQuit = false; gPrevBtn = 0xFFFF;               // ignore buttons still held from the menu
  EWLOGI("CRAFT", "enter heap=%u int_free=%u", (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void CraftView::onExit() {
  gHelperRun = false;
  for (int i = 0; i < 20 && gHelper; i++) delay(20);
}

void CraftView::render() {
  if (!gHud || !gBuf[0] || !gBuf[1] || !gWorld || !gTex) return;
  if (gQuit) { gQuit = false; switchTo(Screen::AppList); return; }
  if (ewsimStreaming()) { delay(20); gLastMs = millis(); return; }   // test snapshot: stay quiet
  powerPerfDemand(true);
  powerOnActivity();                                  // playing: keep the screen on
  uint32_t now = millis();
  float dt = (now - gLastMs) / 1000.0f; gLastMs = now;
  if (dt > 0.1f) dt = 0.1f;
  update(dt);

  Frame &F = gF;
  F.ox = pX; F.oy = pY + EYE; F.oz = pZ;
  float cp = cosf(pitch), sp = sinf(pitch), syw = sinf(yaw), cyw = cosf(yaw);
  F.fx = syw * cp; F.fy = sp; F.fz = cyw * cp;
  F.rx = cyw; F.ry = 0; F.rz = -syw;
  // u = f x r
  F.ux = F.fy * F.rz - F.fz * F.ry; F.uy = F.fz * F.rx - F.fx * F.rz; F.uz = F.fx * F.ry - F.fy * F.rx;
  int bx, by, bz, px, py, pz;
  F.target = pickBlock(bx, by, bz, px, py, pz);
  F.tx = bx; F.ty = by; F.tz = bz;
  F.fb = gBuf[gCur];
  static bool benched = false;
  if (!benched) { benched = true; benchRays(F.fb); }
  uint32_t t0 = micros();
  gNextRow = 0;
  xSemaphoreGive(gGo);                               // core 0 starts frame N+1 now
  if (gHavePrev) blitFrame(gBuf[gCur ^ 1]);          // core 1 pushes frame N meanwhile
  uint32_t t1 = micros();
  renderShared();                                    // then joins the row queue
  uint32_t t2 = micros();
  xSemaphoreTake(gDone, pdMS_TO_TICKS(500));
  uint32_t t3 = micros();
  gHud->setFB(gBuf[gCur]);
  drawHud(gHud, padGet().connected);
  uint32_t t4 = micros();
  gHavePrev = true;
  gCur ^= 1;
  static uint32_t aFlush = 0, aRender = 0, aWait = 0, aHud = 0, aN = 0;
  aFlush += t1 - t0; aRender += t2 - t1; aWait += t3 - t2; aHud += t4 - t3; aN++;
  gFpsFrames++;
  if (now - gFpsT0 >= 2000) {
    gFps = gFpsFrames * 1000.0f / (now - gFpsT0);
    gFpsFrames = 0; gFpsT0 = now;
    static uint32_t lastLog = 0;
    if (now - lastLog > 5000 && aN) {
      lastLog = now;
      EWLOGI("CRAFT", "fps=%.1f ms: push=%lu then_render=%lu helper_wait=%lu hud=%lu core=%d rows c0=%lu c1=%lu mhz=%lu", gFps,
             aFlush / aN / 1000, aRender / aN / 1000, aWait / aN / 1000, aHud / aN / 1000, xPortGetCoreID(),
             (unsigned long)(gRowsByCore[0] / aN), (unsigned long)(gRowsByCore[1] / aN),
             (unsigned long)getCpuFrequencyMhz());
      gRowsByCore[0] = gRowsByCore[1] = 0;
      aFlush = aRender = aWait = aHud = aN = 0;
    }
  }
}

void CraftView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { gQuit = true; return; }
  // Only a person counts as input (the 1 Hz Tick kept attract mode from starting).
  if (e.type == EventType::Touch || e.type == EventType::TouchHold ||
      e.type == EventType::TouchUp || e.type == EventType::Gesture) gLastInputMs = millis();
  switch (e.type) {
    case EventType::Touch:
      tDown = true; tMove = false; tX0 = tLX = e.x; tY0 = tLY = e.y;
      if (e.y >= 248) { int i = (e.x - (240 - 9 * 24) / 2) / 24; if (i >= 0 && i < 9) tReqSel = i; tDown = false; }
      break;
    case EventType::TouchHold:
      if (!tDown) break;
      if (abs(e.x - tX0) + abs(e.y - tY0) > 12) tMove = true;
      if (tX0 < 80) {                                  // left third: virtual stick
        tStickX = (e.x - tX0) / 40.0f; tStickY = (e.y - tY0) / 40.0f;
        if (tStickX > 1) tStickX = 1; if (tStickX < -1) tStickX = -1;
        if (tStickY > 1) tStickY = 1; if (tStickY < -1) tStickY = -1;
      } else { tLookDX += e.x - tLX; tLookDY += e.y - tLY; }
      tLX = e.x; tLY = e.y;
      break;
    case EventType::TouchUp:
      if (tDown && !tMove && tX0 >= 80) tReqMine = true;
      tDown = false; tStickX = tStickY = 0;
      break;
    case EventType::Gesture:
      if (e.gesture == Gesture::LongPress) tReqPlace = true;
      else if (e.gesture == Gesture::DoubleTap) tReqJump = true;
      break;
    default: break;
  }
}
