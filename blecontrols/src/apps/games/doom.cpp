// DOOM — raycasting FPS engine. See doom.h for the public contract.
//
// Rendering: a single 240x280 RGB565 canvas lives in PSRAM (frameCanvas()).
// We write the 3D view straight into its framebuffer for speed, draw the gun
// and HUD on top with the Canvas GFX primitives, then flush() the whole frame
// to the ST7789 once per loop.
//
// Controls (single-touch hardware, so movement and firing use separate inputs):
//   * Drag on screen  — virtual joystick. Up/down = walk, left/right = turn.
//   * Side button tap — FIRE.
//   * Side button hold (2 s) — exit to the launcher.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "pins.h"
#include "display.h"
#include "haptic.h"
#include "doom.h"

// ---------------------------------------------------------------------------
// Geometry / colour helpers
// ---------------------------------------------------------------------------
#define SCREEN_W 240
#define SCREEN_H 280
#define VIEW_H   200            // height of the 3D viewport; HUD fills the rest
#define HORIZON  (VIEW_H / 2)

#define BLACK565 0x0000
#define SPR_KEY  0xF81F         // magenta = transparent in sprite textures

static Arduino_Canvas *cv = nullptr;
static uint16_t       *fb = nullptr;   // cv->getFramebuffer(), 240*280 RGB565

// Scale each RGB565 channel by f/256 — cheap per-pixel light/fog shading.
static inline uint16_t shade(uint16_t c, uint16_t f) {
  uint16_t r = ((c >> 11) & 0x1F) * f >> 8;
  uint16_t g = ((c >> 5) & 0x3F) * f >> 8;
  uint16_t b = (c & 0x1F) * f >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void cvText(int x, int y, uint8_t size, uint16_t color, const char *fmt, ...) {
  char buf[48];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  cv->setTextSize(size);
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(buf);
}

// ---------------------------------------------------------------------------
// Level — a 24x24 grid. 0 = empty, 1..4 = wall texture id. Built once.
// ---------------------------------------------------------------------------
#define MAP_W 24
#define MAP_H 24
static uint8_t grid[MAP_H][MAP_W];

static inline uint8_t cell(int x, int y) {
  if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return 1;
  return grid[y][x];
}

struct Box { int x, y, w, h, tex; };
static const Box kBoxes[] = {
  {5, 5, 2, 2, 1}, {17, 5, 2, 2, 2}, {5, 17, 2, 2, 3}, {17, 17, 2, 2, 4},
  {11, 11, 2, 2, 2},                     // centre pillar
  {11, 4, 2, 1, 1}, {11, 19, 2, 1, 1},   // N/S accents
  {4, 11, 1, 2, 3}, {19, 11, 1, 2, 3},   // E/W accents
};

static const float kSpawn[][2] = {
  {8.5f, 3.5f}, {15.5f, 3.5f}, {3.5f, 12.5f},
  {20.5f, 12.5f}, {8.5f, 20.5f}, {15.5f, 20.5f},
};
#define ENEMY_MAX ((int)(sizeof(kSpawn) / sizeof(kSpawn[0])))

static void buildLevel() {
  memset(grid, 0, sizeof grid);
  for (int x = 0; x < MAP_W; x++) { grid[0][x] = 1; grid[MAP_H - 1][x] = 1; }
  for (int y = 0; y < MAP_H; y++) { grid[y][0] = 1; grid[y][MAP_W - 1] = 1; }
  for (const Box &b : kBoxes)
    for (int dy = 0; dy < b.h; dy++)
      for (int dx = 0; dx < b.w; dx++) {
        int x = b.x + dx, y = b.y + dy;
        if (x >= 0 && x < MAP_W && y >= 0 && y < MAP_H) grid[y][x] = b.tex;
      }
}

// ---------------------------------------------------------------------------
// Textures (procedural, generated once at boot)
// ---------------------------------------------------------------------------
#define TEX 32
static uint16_t wallTex[4][TEX * TEX];
static uint16_t enemyTex[TEX * TEX];

static inline uint32_t hash2(int x, int y, int s) {
  uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)s * 83492791u);
  h ^= h >> 13; h *= 0x5bd1e995; h ^= h >> 15;
  return h;
}

static void genWallTextures() {
  for (int t = 0; t < 4; t++) {
    for (int y = 0; y < TEX; y++) {
      for (int x = 0; x < TEX; x++) {
        int n = (int)(hash2(x, y, t) & 0x1F) - 16;   // -16..+15 noise
        uint16_t c;
        switch (t) {
          case 0: {  // brick
            int row = y / 8;
            int bx = (row & 1) ? (x + 8) % 16 : x % 16;
            bool mortar = (y % 8 == 0) || (bx == 0);
            c = mortar ? RGB565(70, 65, 60)
                       : RGB565(150 + n, 60 + n / 2, 45 + n / 2);
          } break;
          case 1: {  // stone block
            bool seam = (x % 16 == 0) || (y % 16 == 0);
            int g = 110 + n;
            c = seam ? RGB565(60, 60, 65) : RGB565(g, g, g + 6);
          } break;
          case 2: {  // metal panel
            bool edge = (x < 2 || x > 29 || y < 2 || y > 29);
            bool rivet = ((x % 14 == 4 || x % 14 == 9) && (y % 14 == 4 || y % 14 == 9));
            if (rivet) c = RGB565(170, 175, 190);
            else if (edge) c = RGB565(40, 44, 56);
            else c = RGB565(74 + n, 80 + n, 96 + n);
          } break;
          default: { // tech-green (exit-looking)
            bool stripe = (x % 8 < 2);
            int g = 120 + n;
            c = stripe ? RGB565(20, 70, 45) : RGB565(36, g, 70);
          } break;
        }
        wallTex[t][y * TEX + x] = c;
      }
    }
  }
}

// A little imp/demon billboard on a transparent (magenta) background.
static void genEnemySprite() {
  const uint16_t BODY   = RGB565(150, 45, 35);
  const uint16_t BODYDK = RGB565(95, 26, 20);
  const uint16_t EYE    = RGB565(255, 235, 60);
  const uint16_t HORN   = RGB565(225, 215, 205);
  const uint16_t MOUTH  = RGB565(35, 8, 8);

  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++)
      enemyTex[y * TEX + x] = SPR_KEY;

  auto put = [&](int x, int y, uint16_t c) {
    if (x >= 0 && x < TEX && y >= 0 && y < TEX) enemyTex[y * TEX + x] = c;
  };
  auto ellipse = [&](int cx, int cy, int rx, int ry, uint16_t c, uint16_t edge) {
    for (int y = 0; y < TEX; y++)
      for (int x = 0; x < TEX; x++) {
        float dx = (float)(x - cx) / rx, dy = (float)(y - cy) / ry;
        float d = dx * dx + dy * dy;
        if (d <= 1.0f) put(x, y, d > 0.74f ? edge : c);
      }
  };

  ellipse(16, 20, 9, 9, BODY, BODYDK);    // torso
  ellipse(8, 19, 3, 6, BODY, BODYDK);     // arms
  ellipse(24, 19, 3, 6, BODY, BODYDK);
  ellipse(16, 9, 6, 6, BODY, BODYDK);     // head
  for (int y = 27; y < 31; y++) { put(13, y, BODYDK); put(14, y, BODY); put(18, y, BODY); put(19, y, BODYDK); }  // legs
  // horns
  put(10, 3, HORN); put(11, 4, HORN); put(22, 3, HORN); put(21, 4, HORN);
  // eyes + brow + mouth
  for (int y = 7; y < 10; y++) { put(12, y, EYE); put(13, y, EYE); put(19, y, EYE); put(20, y, EYE); }
  put(12, 6, BODYDK); put(13, 6, BODYDK); put(19, 6, BODYDK); put(20, 6, BODYDK);
  for (int x = 12; x <= 20; x++) put(x, 13, MOUTH);
  put(14, 14, MOUTH); put(16, 14, MOUTH); put(18, 14, MOUTH);
}

// ---------------------------------------------------------------------------
// Floor / ceiling gradient (precomputed per scanline)
// ---------------------------------------------------------------------------
static uint16_t ceilGrad[VIEW_H];
static uint16_t floorGrad[VIEW_H];
static void genGradients() {
  for (int y = 0; y < VIEW_H; y++) {
    if (y < HORIZON) {
      float t = (float)y / HORIZON;            // 0 top .. 1 horizon
      uint16_t f = (uint16_t)((1.0f - 0.70f * t) * 256);
      ceilGrad[y] = shade(RGB565(58, 64, 92), f);
    } else {
      float u = (float)(y - HORIZON) / (VIEW_H - HORIZON);  // 0 horizon .. 1 bottom
      uint16_t f = (uint16_t)((0.30f + 0.70f * u) * 256);
      floorGrad[y] = shade(RGB565(96, 76, 56), f);
    }
  }
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
static const float FOV       = 0.66f;   // tan(half-fov); ~66 deg
static const float MOVE_SPD  = 2.6f;    // cells / sec at full stick
static const float TURN_SPD  = 2.7f;    // rad / sec at full stick
static const float ENEMY_SPD = 1.2f;
static const float AGGRO     = 11.0f;
static const float MELEE     = 1.1f;
static const float FIRE_RANGE = 16.0f;
static const int   ENEMY_HP   = 50;
static const int   SHOT_DMG   = 25;

struct Enemy {
  float x, y;
  int   health;
  bool  alive;
  float hurt;     // hit-flash seconds remaining
  float atkCd;    // melee cooldown
};
static Enemy enemy[ENEMY_MAX];

static float posX, posY, ang;
static float dirX, dirY, planeX, planeY;
static int   health, ammo, kills;
static float zbuf[SCREEN_W];

enum State { TITLE, PLAY, DEAD, WIN };
static State state;

static float gunRecoil, muzzle, bob, bobPhase, hurtFlash;
static uint32_t lastMs;

// input edge tracking
static bool  joyActive, prevFinger, prevBtn;
static float anchorX, anchorY;
static uint32_t btnDownMs;
static volatile bool sDoomExit = false;   // set by 2 s button hold; adapter polls
bool doomExitRequested() { bool e = sDoomExit; sDoomExit = false; return e; }

static void setDir() {
  dirX = cosf(ang); dirY = sinf(ang);
  planeX = -dirY * FOV; planeY = dirX * FOV;
}

static void resetLevel() {
  posX = 12.5f; posY = 21.5f; ang = -(float)M_PI / 2;   // bottom centre, facing N
  setDir();
  health = 100; ammo = 50; kills = 0;
  for (int i = 0; i < ENEMY_MAX; i++)
    enemy[i] = {kSpawn[i][0], kSpawn[i][1], ENEMY_HP, true, 0, 0};
  joyActive = false;
  gunRecoil = muzzle = bob = bobPhase = hurtFlash = 0;
}

// ---------------------------------------------------------------------------
// Touch — read the CST816S finger point straight off the I2C bus.
// ---------------------------------------------------------------------------
static bool readFinger(int &x, int &y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 6) != 6) return false;
  Wire.read();                 // 0x01 gesture (unused)
  uint8_t pts = Wire.read();   // 0x02 finger count
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  if (pts == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}

// ---------------------------------------------------------------------------
// Movement + AI helpers
// ---------------------------------------------------------------------------
static void tryMove(float &px, float &py, float dx, float dy) {
  const float m = 0.20f;
  float tx = px + dx + (dx > 0 ? m : (dx < 0 ? -m : 0));
  if (cell((int)tx, (int)py) == 0) px += dx;
  float ty = py + dy + (dy > 0 ? m : (dy < 0 ? -m : 0));
  if (cell((int)px, (int)ty) == 0) py += dy;
}

static bool lineOfSight(float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay;
  float dist = sqrtf(dx * dx + dy * dy);
  int steps = (int)(dist * 4) + 1;
  for (int i = 1; i < steps; i++) {
    float t = (float)i / steps;
    if (cell((int)(ax + dx * t), (int)(ay + dy * t))) return false;
  }
  return true;
}

// transformY (depth in camera space) and transformX (lateral) for a point.
static void worldToCamera(float wx, float wy, float &tx, float &ty) {
  float sx = wx - posX, sy = wy - posY;
  float inv = 1.0f / (planeX * dirY - dirX * planeY);
  tx = inv * (dirY * sx - dirX * sy);
  ty = inv * (-planeY * sx + planeX * sy);
}

static void fireShot() {
  if (ammo <= 0) return;
  ammo--;
  muzzle = 0.07f; gunRecoil = 1.0f;
  hapticBuzz(160, 45);

  int best = -1; float bestDepth = 1e9f;
  for (int i = 0; i < ENEMY_MAX; i++) {
    if (!enemy[i].alive) continue;
    float tx, ty; worldToCamera(enemy[i].x, enemy[i].y, tx, ty);
    if (ty <= 0.3f) continue;                          // behind camera
    float ex = enemy[i].x - posX, ey = enemy[i].y - posY;
    float dist = sqrtf(ex * ex + ey * ey);
    if (dist > FIRE_RANGE) continue;
    if (fabsf(tx / ty) > 0.16f) continue;              // not under crosshair
    if (!lineOfSight(posX, posY, enemy[i].x, enemy[i].y)) continue;
    if (ty < bestDepth) { bestDepth = ty; best = i; }
  }
  if (best < 0) return;
  enemy[best].health -= SHOT_DMG;
  enemy[best].hurt = 0.12f;
  if (enemy[best].health <= 0) {
    enemy[best].alive = false;
    kills++;
    hapticBuzz(220, 90);
  }
}

static void updateEnemies(float dt) {
  for (int i = 0; i < ENEMY_MAX; i++) {
    Enemy &e = enemy[i];
    if (!e.alive) continue;
    if (e.hurt > 0) e.hurt -= dt;
    if (e.atkCd > 0) e.atkCd -= dt;
    float dx = posX - e.x, dy = posY - e.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > AGGRO || !lineOfSight(e.x, e.y, posX, posY)) continue;
    if (dist > MELEE) {
      float s = ENEMY_SPD * dt / dist;
      tryMove(e.x, e.y, dx * s, dy * s);
    } else if (e.atkCd <= 0) {
      e.atkCd = 1.1f;
      health -= 9;
      hurtFlash = 0.30f;
      hapticBuzz(200, 70);
      if (health <= 0) { health = 0; state = DEAD; }
    }
  }
}

// ---------------------------------------------------------------------------
// 3D rendering
// ---------------------------------------------------------------------------
static void renderWalls() {
  for (int x = 0; x < SCREEN_W; x++) {
    float cameraX = 2.0f * x / SCREEN_W - 1.0f;
    float rdx = dirX + planeX * cameraX;
    float rdy = dirY + planeY * cameraX;

    int mapX = (int)posX, mapY = (int)posY;
    float ddx = (rdx == 0) ? 1e30f : fabsf(1.0f / rdx);
    float ddy = (rdy == 0) ? 1e30f : fabsf(1.0f / rdy);

    int stepX, stepY; float sideX, sideY;
    if (rdx < 0) { stepX = -1; sideX = (posX - mapX) * ddx; }
    else         { stepX = 1;  sideX = (mapX + 1.0f - posX) * ddx; }
    if (rdy < 0) { stepY = -1; sideY = (posY - mapY) * ddy; }
    else         { stepY = 1;  sideY = (mapY + 1.0f - posY) * ddy; }

    int side = 0, hit = 0;
    for (int guard = 0; guard < 64 && !hit; guard++) {
      if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
      else               { sideY += ddy; mapY += stepY; side = 1; }
      if (cell(mapX, mapY)) hit = 1;
    }

    float perp = (side == 0) ? (sideX - ddx) : (sideY - ddy);
    if (perp < 0.05f) perp = 0.05f;
    zbuf[x] = perp;

    int lineH = (int)(VIEW_H / perp);
    int drawStart = HORIZON - lineH / 2;
    int drawEnd   = HORIZON + lineH / 2;
    int cs = drawStart < 0 ? 0 : drawStart;
    int ce = drawEnd > VIEW_H ? VIEW_H : drawEnd;

    // wall texture column
    int t = cell(mapX, mapY) - 1; if (t < 0) t = 0; if (t > 3) t = 3;
    float wallX = (side == 0) ? (posY + perp * rdy) : (posX + perp * rdx);
    wallX -= floorf(wallX);
    int texX = (int)(wallX * TEX);
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) texX = TEX - texX - 1;
    if (texX < 0) texX = 0; if (texX >= TEX) texX = TEX - 1;

    float sh = 1.0f - perp * 0.045f;
    if (sh < 0.22f) sh = 0.22f; if (sh > 1.0f) sh = 1.0f;
    if (side == 1) sh *= 0.68f;
    uint16_t sf = (uint16_t)(sh * 256);

    float texStep = (float)TEX / lineH;
    float texPos = (cs - HORIZON + lineH / 2) * texStep;

    uint16_t *col = &fb[x];
    const uint16_t *tex = &wallTex[t][texX];
    for (int y = 0; y < cs; y++)          col[y * SCREEN_W] = ceilGrad[y];
    for (int y = cs; y < ce; y++) {
      int ty = (int)texPos & (TEX - 1);
      texPos += texStep;
      col[y * SCREEN_W] = shade(tex[ty * TEX], sf);
    }
    for (int y = ce; y < VIEW_H; y++)     col[y * SCREEN_W] = floorGrad[y];
  }
}

static void renderEnemies() {
  // far-to-near order
  int order[ENEMY_MAX]; float depth[ENEMY_MAX]; int n = 0;
  for (int i = 0; i < ENEMY_MAX; i++) {
    if (!enemy[i].alive) continue;
    float dx = enemy[i].x - posX, dy = enemy[i].y - posY;
    depth[n] = dx * dx + dy * dy; order[n] = i; n++;
  }
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++)
      if (depth[j] < depth[j + 1]) {
        float td = depth[j]; depth[j] = depth[j + 1]; depth[j + 1] = td;
        int ti = order[j]; order[j] = order[j + 1]; order[j + 1] = ti;
      }

  for (int k = 0; k < n; k++) {
    Enemy &e = enemy[order[k]];
    float tx, ty; worldToCamera(e.x, e.y, tx, ty);
    if (ty <= 0.25f) continue;

    int screenX = (int)((SCREEN_W / 2) * (1.0f + tx / ty));
    int h = abs((int)(VIEW_H / ty));
    int w = h;
    int vOff = h / 6;                                   // nudge feet toward floor
    int startY = HORIZON - h / 2 + vOff;
    int endY   = HORIZON + h / 2 + vOff;
    int startX = screenX - w / 2;
    int endX   = screenX + w / 2;

    float fog = 1.0f - ty * 0.045f;
    if (fog < 0.30f) fog = 0.30f; if (fog > 1.0f) fog = 1.0f;
    bool flash = e.hurt > 0;

    for (int x = startX; x < endX; x++) {
      if (x < 0 || x >= SCREEN_W) continue;
      if (ty >= zbuf[x]) continue;                      // occluded by wall
      int texX = (x - startX) * TEX / w;
      if (texX < 0 || texX >= TEX) continue;
      for (int y = startY; y < endY; y++) {
        if (y < 0 || y >= VIEW_H) continue;
        int texY = (y - startY) * TEX / h;
        if (texY < 0 || texY >= TEX) continue;
        uint16_t c = enemyTex[texY * TEX + texX];
        if (c == SPR_KEY) continue;
        if (flash) c = RGB565(255, 230, 220);
        else c = shade(c, (uint16_t)(fog * 256));
        fb[y * SCREEN_W + x] = c;
      }
    }
  }
}

static void renderCrosshair() {
  uint16_t c = RGB565(230, 230, 230);
  for (int i = -4; i <= 4; i++) {
    if (i >= -1 && i <= 1) continue;
    fb[HORIZON * SCREEN_W + (SCREEN_W / 2 + i)] = c;
    fb[(HORIZON + i) * SCREEN_W + (SCREEN_W / 2)] = c;
  }
}

static void renderGun() {
  int gx = SCREEN_W / 2;
  int ry = (int)(gunRecoil * 14) + (int)bob;
  int base = 150 + ry;
  cv->fillRect(gx - 26, base + 18, 52, 50, RGB565(34, 34, 40));        // receiver
  cv->fillRect(gx - 9, base - 8, 18, 70, RGB565(58, 58, 66));          // barrel
  cv->fillRect(gx - 9, base - 8, 4, 70, RGB565(110, 110, 122));        // highlight
  cv->fillRect(gx - 22, base + 30, 10, 26, RGB565(70, 50, 36));        // grip
  if (muzzle > 0) {
    cv->fillCircle(gx, base - 10, 13, RGB565(255, 220, 90));
    cv->fillCircle(gx, base - 10, 7, RGB565(255, 255, 235));
  }
}

// ---------------------------------------------------------------------------
// HUD + full-screen states
// ---------------------------------------------------------------------------
static void renderHUD() {
  cv->fillRect(0, VIEW_H, SCREEN_W, SCREEN_H - VIEW_H, RGB565(22, 18, 18));
  cv->fillRect(0, VIEW_H, SCREEN_W, 3, RGB565(130, 24, 18));

  cvText(8, VIEW_H + 12, 2, RGB565(255, 50, 36), "HP");
  uint16_t hpc = health > 40 ? RGB565(80, 220, 70) : RGB565(255, 60, 40);
  cvText(46, VIEW_H + 12, 2, hpc, "%d", health);
  cv->fillRect(8, VIEW_H + 34, 96, 8, RGB565(60, 20, 20));
  cv->fillRect(8, VIEW_H + 34, 96 * (health < 0 ? 0 : health) / 100, 8, hpc);

  cvText(8, VIEW_H + 50, 2, RGB565(240, 210, 60), "AMMO %d", ammo);

  cvText(150, VIEW_H + 12, 2, RGB565(255, 50, 36), "KILLS");
  cvText(168, VIEW_H + 38, 3, RGB565(240, 230, 230), "%d/%d", kills, ENEMY_MAX);
}

// Blend the 3D viewport toward colour c by f/256 (a coloured flash/tint).
static inline uint16_t blend(uint16_t a, uint16_t b, uint16_t f) {
  if (f > 256) f = 256;
  uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint16_t r = (ar * (256 - f) + br * f) >> 8;
  uint16_t g = (ag * (256 - f) + bg * f) >> 8;
  uint16_t bl = (ab * (256 - f) + bb * f) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
static void overlay(uint16_t c, uint16_t f) {
  for (int i = 0; i < SCREEN_W * VIEW_H; i++) fb[i] = blend(fb[i], c, f);
}

static void renderTitle() {
  cv->fillScreen(BLACK565);
  for (int y = 0; y < SCREEN_H; y++) {     // dim red vignette background
    uint16_t row = shade(RGB565(70, 8, 6), (uint16_t)(40 + 120 * y / SCREEN_H));
    cv->fillRect(0, y, SCREEN_W, 1, row);
  }
  cvText(40, 56, 7, RGB565(220, 30, 20), "DOOM");
  cvText(38, 58, 7, RGB565(255, 70, 40), "DOOM");   // faux bevel
  cvText(36, 124, 1, RGB565(255, 200, 60), "EWatch Edition");

  cvText(20, 168, 1, RGB565(220, 220, 220), "DRAG  - move / turn");
  cvText(20, 184, 1, RGB565(220, 220, 220), "SIDE BTN - fire");
  cvText(20, 200, 1, RGB565(160, 160, 160), "hold btn 2s - exit");
  cvText(36, 238, 2, RGB565(255, 230, 120), "TAP TO START");
}

static void renderEnd(bool won) {
  overlay(won ? RGB565(0, 90, 0) : RGB565(120, 0, 0), 150);
  if (won) {
    cvText(20, 70, 4, RGB565(120, 255, 120), "LEVEL");
    cvText(8, 110, 4, RGB565(120, 255, 120), "CLEARED");
  } else {
    cvText(18, 84, 5, RGB565(255, 60, 50), "YOU");
    cvText(8, 130, 5, RGB565(255, 60, 50), "DIED");
  }
  cvText(48, VIEW_H + 24, 2, RGB565(240, 230, 120), "TAP TO");
  cvText(36, VIEW_H + 48, 2, RGB565(240, 230, 120), "RESTART");
}

// ---------------------------------------------------------------------------
// Per-frame input + update
// ---------------------------------------------------------------------------
static void handlePlay(float dt, bool finger, int fx, int fy, bool fireEdge) {
  if (finger) {
    if (!joyActive) { joyActive = true; anchorX = fx; anchorY = fy; }
    float dx = fx - anchorX, dy = fy - anchorY;
    if (fabsf(dx) < 10) dx = 0;
    if (fabsf(dy) < 10) dy = 0;
    float turn = constrain(dx / 70.0f, -1.0f, 1.0f);
    float walk = constrain(-dy / 70.0f, -1.0f, 1.0f);
    if (turn != 0) { ang += turn * TURN_SPD * dt; setDir(); }
    if (walk != 0) {
      tryMove(posX, posY, dirX * walk * MOVE_SPD * dt, dirY * walk * MOVE_SPD * dt);
      bobPhase += dt * 9; bob = sinf(bobPhase) * 3 * fabsf(walk);
    }
  } else {
    joyActive = false; bob *= 0.8f;
  }

  if (fireEdge) fireShot();

  updateEnemies(dt);

  if (gunRecoil > 0) gunRecoil -= dt * 6;  if (gunRecoil < 0) gunRecoil = 0;
  if (muzzle > 0) muzzle -= dt;
  if (hurtFlash > 0) hurtFlash -= dt;

  if (kills >= ENEMY_MAX) state = WIN;
}

void doomInit() {
  cv = frameCanvas();
  if (!cv) {                         // PSRAM canvas alloc failed — bail loudly
    if (gfx) { gfx->fillScreen(0xF800); gfx->setCursor(20, 120);
               gfx->setTextColor(0xFFFF); gfx->print("DOOM: no PSRAM canvas"); }
    return;
  }
  fb = cv->getFramebuffer();
  pinMode(PIN_BTN, INPUT);

  genWallTextures();
  genEnemySprite();
  genGradients();
  buildLevel();
  resetLevel();

  state  = TITLE;
  lastMs = millis();
  renderTitle();
  cv->flush();
}

void doomFrame() {
  if (!cv) { delay(100); return; }

  uint32_t now = millis();
  float dt = (now - lastMs) / 1000.0f;
  lastMs = now;
  if (dt > 0.10f) dt = 0.10f;        // clamp after a stall so movement is sane

  // ----- input -----
  int fx = 0, fy = 0;
  bool finger = readFinger(fx, fy);
  bool tapEdge = finger && !prevFinger;
  prevFinger = finger;

  bool btn = (digitalRead(PIN_BTN) == HIGH);
  bool btnEdge = btn && !prevBtn;
  if (btnEdge) btnDownMs = now;
  if (btn && now - btnDownMs > 2000) { sDoomExit = true; btnDownMs = now; }
  prevBtn = btn;

  // ----- update + render per state -----
  switch (state) {
    case TITLE:
      renderTitle();
      if (tapEdge || btnEdge) { resetLevel(); state = PLAY; }
      break;

    case PLAY:
      handlePlay(dt, finger, fx, fy, btnEdge);
      renderWalls();
      renderEnemies();
      renderCrosshair();
      renderGun();
      if (hurtFlash > 0) overlay(RGB565(200, 0, 0), (uint16_t)(hurtFlash * 320));
      renderHUD();
      break;

    case DEAD:
      renderWalls(); renderEnemies(); renderGun();
      renderEnd(false);
      if (tapEdge || btnEdge) { resetLevel(); state = PLAY; }
      break;

    case WIN:
      renderWalls(); renderEnemies();
      renderEnd(true);
      if (tapEdge || btnEdge) { resetLevel(); state = PLAY; }
      break;
  }

  cv->flush();
}
