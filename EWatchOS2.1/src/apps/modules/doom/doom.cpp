// DOOM — raycasting FPS engine. See doom.h for the public contract.
//
// Rendering: LANDSCAPE 280x240 on the 240x280 panel. Two PSRAM canvases (the
// shared frameCanvas() + one of our own) are drawn alternately: while core 1
// renders frame N+1 into one, a flush task on core 0 streams frame N from the
// other to the panel (16-row strips through internal RAM, one writePixels
// each — the canvas's own flush measured ~49 ms, pixel by pixel). The canvas
// rotation turns the GFX primitives (gun, HUD, text) for free; the raycaster
// writes columns straight into the framebuffer through pix(). doomLeave()
// waits for the last flush, frees our canvas and puts rotation 0 back.
//
// Controls (the CST816S reports ONE finger, so no twin-stick / WASD pads):
//   * Tilt the watch left / right — LOOK. Measured against "level" (the
//     angle held as a level starts), dead zone + soft curve; holding a hard
//     tilt ramps the turn up. Level drifts slowly toward the resting angle
//     while near it; double-press the side button to re-level on the spot.
//   * Left half of the screen — a FIXED joystick (bottom-left). Up / down =
//     walk, left / right = strafe; push to the rim while walking = sprint.
//   * Right half — FIRE; keep the finger down for auto-fire (only while a
//     demon is under the crosshair).
//   * Side button press — FIRE (works while you steer with the stick).
//   * Side button hold  — after 0.5 s an EXIT bar fills; at 2 s, back to the
//     launcher (the OS power-off hold can't fire in here: the I/O task is
//     parked while Doom owns the button).
//   * Light aim assist pulls the view onto a demon near the crosshair.
//   * SWITCH button (top centre) — next weapon you carry (you keep every gun
//     you walk over; walking over one you have tops up its ammo).
//   HUD: health, ammo bar for the gun in hand (its ammo type), switch button,
//   coins, level, look indicator, boss bar, hit marker (green = effective,
//   grey = resisted, blue = blocked), red edges toward an attacker.
//
// Game: endless levels generated from the level number (16x16 growing to
// 32x32), one more demon per level, new types unlocking, a boss every 5th.
// Every weapon deals a damage type (ballistic / explosive / fire / energy /
// melee) and every enemy takes a multiplier per type (kEnemies[].mult); the
// Shield blocks ballistic/energy/melee from the front. Kills drop coins; a
// shop between levels sells health, ammo and weapons.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "pins.h"
#include "display.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "ewsim.h"     // host test hooks: simulated finger / side button / tilt
#include "ewlog.h"
#include "controller.h" // readAccel(): the I/O task is parked, the bus is ours
#include "doom.h"
#include "doom_lcd.h"     // GDMA pixel push (copy of hopper_lcd)
#include "power_mgr.h"    // powerOnActivity()
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Geometry / colour helpers
// ---------------------------------------------------------------------------
// Landscape: logical 280 x 240 on the 240 x 280 panel. DOOM_ROT 1 puts the
// logical top on the panel's right edge; 3 puts it on the left edge.
#define DOOM_ROT 1
#define PANEL_W  240            // physical framebuffer row length
#define PANEL_H  280
#define SCREEN_W 280            // logical (landscape) width
#define SCREEN_H 240
#define VIEW_H   SCREEN_H       // the 3D view fills the screen; HUD is overlaid
#define HORIZON  (VIEW_H / 2)

#define BLACK565 0x0000
#define SPR_KEY  0xF81F         // magenta = transparent in sprite textures

static Arduino_Canvas *cv = nullptr;    // canvas being drawn this frame
static uint16_t       *fb = nullptr;    // its framebuffer, 240*280 RGB565
static Arduino_Canvas *cvOs = nullptr;  // frameCanvas() (the OS's)
static Arduino_Canvas *cvOwn = nullptr; // our second buffer (null: single-buffered)

// Framebuffer index of logical (landscape) pixel x, y — the same mapping the
// canvas uses for its own drawing at this rotation.
static inline int pix(int x, int y) {
#if DOOM_ROT == 1
  return x * PANEL_W + (PANEL_W - 1 - y);
#else
  return (SCREEN_W - 1 - x) * PANEL_W + y;
#endif
}
// Panel touch point -> logical point.
static inline void touchToLogical(int px, int py, int &x, int &y) {
#if DOOM_ROT == 1
  x = py; y = PANEL_W - 1 - px;
#else
  x = SCREEN_W - 1 - py; y = px;
#endif
}

// Scale each RGB565 channel by f/256 (f <= 256) — cheap light/fog shading.
static inline uint16_t shade(uint16_t c, uint16_t f) {
  uint16_t r = ((c >> 11) & 0x1F) * f >> 8;
  uint16_t g = ((c >> 5) & 0x3F) * f >> 8;
  uint16_t b = (c & 0x1F) * f >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
// Blend a toward b by f/256.
static inline uint16_t blend(uint16_t a, uint16_t b, uint16_t f) {
  if (f > 256) f = 256;
  uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint16_t r = (ar * (256 - f) + br * f) >> 8;
  uint16_t g = (ag * (256 - f) + bg * f) >> 8;
  uint16_t bl = (ab * (256 - f) + bb * f) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------------------
// Present: strip flush on a core-0 task, overlapped with the next render.
// ---------------------------------------------------------------------------
#define FLUSH_ROWS 16
static uint16_t sStrip[PANEL_W * FLUSH_ROWS];   // internal RAM (7.5 KB), CPU path
static uint16_t *sDmaStrip[2];                  // DMA-capable, big-endian, ping-pong
static bool      sDmaOk = false;
static QueueHandle_t     sFlushQ = nullptr;     // framebuffer pointers to push
static SemaphoreHandle_t sFlushIdle = nullptr;  // given when the last push is done
static volatile uint32_t sFlushUsAcc = 0, sFlushN = 0;

static void flushBuffer(const uint16_t *src) {
  uint32_t t0 = micros();
  Arduino_DataBus *bus = displayBus();
  Arduino_TFT *tft = static_cast<Arduino_TFT *>(gfx);
  tft->startWrite();
  tft->writeAddrWindow(0, 0, PANEL_W, PANEL_H);
  if (sDmaOk) {
    // GDMA: byte-swap strip k+1 while DMA sends strip k (measured by the
    // Hopper module: 22 ms/frame vs 40-53 ms CPU-polled). The ewsim tap
    // can't see DMA, so hand it the native rows for `snap`.
    int k = 0;
    for (int y = 0; y < PANEL_H; y += FLUSH_ROWS, k ^= 1) {
      int rows = PANEL_H - y < FLUSH_ROWS ? PANEL_H - y : FLUSH_ROWS;
      const uint32_t *in = (const uint32_t *)&src[y * PANEL_W];
      uint32_t *out = (uint32_t *)sDmaStrip[k];
      for (int i = 0; i < rows * PANEL_W / 2; i++) {          // 2 px per word
        uint32_t v = in[i];
        out[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      }
      ewsimTapRows(y, rows, &src[y * PANEL_W]);
      doomLcdDmaWait();                                       // strip k-1 is out
      if (!doomLcdDmaSend(sDmaStrip[k], rows * PANEL_W * 2)) { sDmaOk = false; break; }
      // Sleep a tick every 6 strips (strip k is on the wire meanwhile): ~3 ms
      // per frame of core 0 left for the console. The push (28.6 ms) is
      // shorter than a frame (33 ms), so this costs no fps; without it ewt
      // replies / snaps timed out during Doom.
      if ((y / FLUSH_ROWS) % 6 == 5) vTaskDelay(1);
    }
    doomLcdDmaWait();
    tft->endWrite();
    if (sDmaOk) { sFlushUsAcc += micros() - t0; sFlushN++; return; }
    tft->startWrite();                                        // DMA failed: redo on CPU
    tft->writeAddrWindow(0, 0, PANEL_W, PANEL_H);
  }
  for (int y = 0; y < PANEL_H; y += FLUSH_ROWS) {
    int rows = PANEL_H - y < FLUSH_ROWS ? PANEL_H - y : FLUSH_ROWS;
    memcpy(sStrip, &src[y * PANEL_W], rows * PANEL_W * 2);
    bus->writePixels(sStrip, rows * PANEL_W);
  }
  tft->endWrite();
  sFlushUsAcc += micros() - t0; sFlushN++;
}

static void flushTask(void *) {
  for (;;) {
    const uint16_t *p;
    if (xQueueReceive(sFlushQ, &p, portMAX_DELAY) == pdTRUE) {
      flushBuffer(p);
      xSemaphoreGive(sFlushIdle);
    }
  }
}

static bool presentReady() { return sFlushQ && displayBus() && gfx; }

// Hand the finished frame to the flush task and switch to the other buffer.
// Single-buffered (no second canvas): wait for the push before returning.
static void present() {
  if (!presentReady()) { cv->flush(); return; }
  xSemaphoreTake(sFlushIdle, portMAX_DELAY);     // previous push finished
  const uint16_t *p = fb;
  xQueueSend(sFlushQ, &p, portMAX_DELAY);
  if (cvOwn) {
    cv = (cv == cvOs) ? cvOwn : cvOs;
    fb = cv->getFramebuffer();
  } else {
    xSemaphoreTake(sFlushIdle, portMAX_DELAY);   // must not draw into it mid-push
    xSemaphoreGive(sFlushIdle);
  }
}

static void waitFlushIdle() {
  if (!sFlushIdle) return;
  xSemaphoreTake(sFlushIdle, portMAX_DELAY);
  xSemaphoreGive(sFlushIdle);
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
// Centred text (built-in 6x8 font scaled by size).
static void cvTextC(int y, uint8_t size, uint16_t color, const char *s) {
  int w = (int)strlen(s) * 6 * size - size;
  cvText((SCREEN_W - w) / 2, y, size, color, "%s", s);
}

// ---------------------------------------------------------------------------
// Textures (procedural, generated once at boot)
// ---------------------------------------------------------------------------
#define TEX 32
static uint16_t wallTex[4][TEX * TEX];

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


// ---------------------------------------------------------------------------
// Sprites: generated once into one PSRAM block (internal RAM is scarce).
// ---------------------------------------------------------------------------
enum SprId {
  SP_IMP, SP_RUNT, SP_SPITTER, SP_BRUTE, SP_BOMBER, SP_SHADE, SP_BARON,
  SP_MANCUBUS, SP_LEAPER, SP_SHIELD_F, SP_SHIELD_B, SP_NECRO, SP_TURRET, SP_BAT,
  SP_FIREBALL, SP_ROCKET, SP_PLASMA, SP_GRENADE, SP_COIN,
  SP_G_PISTOL, SP_G_DUAL, SP_G_SMG, SP_G_SHOTGUN, SP_G_SNIPER, SP_G_RPG,
  SP_G_SAW, SP_G_FLAMER, SP_G_PLASMA, SP_G_RAIL, SP_G_GREN,
  SP_A_BULLET, SP_A_SHELL, SP_A_HEAVY, SP_A_ROCKET, SP_A_CELL, SP_A_FUEL, SP_A_GREN,
  SP_MEDKIT,
  SP_COUNT
};
static uint16_t *sprBlock = nullptr;
static inline uint16_t *spr(int id) { return sprBlock + id * TEX * TEX; }

static void sprClear(uint16_t *t) { for (int i = 0; i < TEX * TEX; i++) t[i] = SPR_KEY; }
static inline void sprPut(uint16_t *t, int x, int y, uint16_t c) {
  if (x >= 0 && x < TEX && y >= 0 && y < TEX) t[y * TEX + x] = c;
}
static void sprRect(uint16_t *t, int x0, int y0, int x1, int y1, uint16_t c) {
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) sprPut(t, x, y, c);
}
static void sprEllipse(uint16_t *t, int cx, int cy, int rx, int ry, uint16_t c, uint16_t edge) {
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      float dx = (float)(x - cx) / rx, dy = (float)(y - cy) / ry;
      float d = dx * dx + dy * dy;
      if (d <= 1.0f) sprPut(t, x, y, d > 0.74f ? edge : c);
    }
}

static void genDemon(uint16_t *t, uint16_t body, uint16_t dark, uint16_t eye, uint16_t horn,
                     bool bigHorns, bool openMouth) {
  sprClear(t);
  sprEllipse(t, 16, 20, 9, 9, body, dark);
  sprEllipse(t, 8, 19, 3, 6, body, dark);
  sprEllipse(t, 24, 19, 3, 6, body, dark);
  sprEllipse(t, 16, 9, 6, 6, body, dark);
  for (int y = 27; y < 31; y++) { sprPut(t, 13, y, dark); sprPut(t, 14, y, body); sprPut(t, 18, y, body); sprPut(t, 19, y, dark); }
  sprPut(t, 10, 3, horn); sprPut(t, 11, 4, horn); sprPut(t, 22, 3, horn); sprPut(t, 21, 4, horn);
  if (bigHorns) { sprPut(t, 9, 2, horn); sprPut(t, 8, 1, horn); sprPut(t, 23, 2, horn); sprPut(t, 24, 1, horn);
                  sprPut(t, 10, 2, horn); sprPut(t, 22, 2, horn); }
  for (int y = 7; y < 10; y++) { sprPut(t, 12, y, eye); sprPut(t, 13, y, eye); sprPut(t, 19, y, eye); sprPut(t, 20, y, eye); }
  sprPut(t, 12, 6, dark); sprPut(t, 13, 6, dark); sprPut(t, 19, 6, dark); sprPut(t, 20, 6, dark);
  const uint16_t MOUTH = RGB565(35, 8, 8);
  if (openMouth) { sprRect(t, 12, 12, 20, 15, MOUTH); sprPut(t, 13, 12, RGB565(240, 240, 220)); sprPut(t, 19, 12, RGB565(240, 240, 220)); }
  else { for (int x = 12; x <= 20; x++) sprPut(t, x, 13, MOUTH); sprPut(t, 14, 14, MOUTH); sprPut(t, 16, 14, MOUTH); sprPut(t, 18, 14, MOUTH); }
}

static void genBall(uint16_t *t, uint16_t core, uint16_t mid, uint16_t edge, float r) {
  sprClear(t);
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      float d = sqrtf((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f)) * 13.0f / r;
      if (d < 5) sprPut(t, x, y, core); else if (d < 10) sprPut(t, x, y, mid); else if (d < 13) sprPut(t, x, y, edge);
    }
}

static void genSprites() {
  if (!sprBlock) sprBlock = (uint16_t *)ps_malloc(SP_COUNT * TEX * TEX * 2);
  if (!sprBlock) return;
  const uint16_t EYE = RGB565(255, 235, 60), HORN = RGB565(225, 215, 205);
  genDemon(spr(SP_IMP),     RGB565(150, 45, 35),  RGB565(95, 26, 20),  EYE, HORN, false, false);
  genDemon(spr(SP_RUNT),    RGB565(70, 150, 50),  RGB565(35, 90, 25),  RGB565(255, 60, 40), HORN, false, false);
  genDemon(spr(SP_SPITTER), RGB565(130, 60, 160), RGB565(70, 30, 95),  RGB565(120, 255, 120), HORN, false, true);
  genDemon(spr(SP_BRUTE),   RGB565(80, 95, 120),  RGB565(40, 50, 70),  RGB565(255, 140, 40), RGB565(240, 240, 240), true, false);
  genDemon(spr(SP_SHADE),   RGB565(170, 180, 210),RGB565(110, 120, 150),RGB565(255, 255, 255), RGB565(200, 200, 230), false, true);
  genDemon(spr(SP_BARON),   RGB565(180, 20, 30),  RGB565(100, 10, 15), RGB565(255, 255, 255), RGB565(255, 210, 60), true, true);
  genDemon(spr(SP_LEAPER),  RGB565(210, 150, 40), RGB565(130, 80, 20), RGB565(255, 40, 40), RGB565(60, 30, 10), true, true);
  genDemon(spr(SP_SHIELD_B),RGB565(90, 110, 140), RGB565(50, 60, 85),  RGB565(255, 220, 60), HORN, false, false);
  genDemon(spr(SP_SHIELD_F),RGB565(90, 110, 140), RGB565(50, 60, 85),  RGB565(255, 220, 60), HORN, false, false);
  { uint16_t *t = spr(SP_SHIELD_F);                 // a riveted tower shield over the body
    sprRect(t, 7, 12, 25, 29, RGB565(150, 155, 165)); sprRect(t, 7, 12, 25, 12, RGB565(210, 215, 225));
    sprRect(t, 7, 12, 7, 29, RGB565(90, 95, 105)); sprRect(t, 25, 12, 25, 29, RGB565(90, 95, 105));
    sprRect(t, 15, 15, 17, 26, RGB565(200, 60, 40)); sprRect(t, 11, 19, 21, 21, RGB565(200, 60, 40)); }
  genDemon(spr(SP_NECRO),   RGB565(60, 30, 80),   RGB565(30, 10, 45),  RGB565(80, 255, 120), RGB565(40, 40, 40), false, false);
  { uint16_t *t = spr(SP_NECRO);                    // robe + glowing staff
    sprRect(t, 9, 22, 23, 30, RGB565(40, 15, 55)); sprRect(t, 27, 4, 28, 30, RGB565(120, 90, 60));
    sprEllipse(t, 27, 4, 2, 2, RGB565(120, 255, 150), RGB565(60, 200, 90)); }
  { uint16_t *t = spr(SP_MANCUBUS); sprClear(t);    // fat body, arm cannons
    sprEllipse(t, 16, 19, 13, 11, RGB565(190, 150, 110), RGB565(120, 90, 60));
    sprEllipse(t, 16, 8, 6, 5, RGB565(190, 150, 110), RGB565(120, 90, 60));
    sprRect(t, 0, 16, 4, 21, RGB565(70, 70, 80)); sprRect(t, 27, 16, 31, 21, RGB565(70, 70, 80));
    sprRect(t, 0, 17, 1, 20, RGB565(255, 140, 40)); sprRect(t, 30, 17, 31, 20, RGB565(255, 140, 40));
    sprRect(t, 13, 7, 14, 8, EYE); sprRect(t, 18, 7, 19, 8, EYE); sprRect(t, 12, 11, 20, 12, RGB565(60, 20, 10));
    sprRect(t, 10, 28, 13, 31, RGB565(120, 90, 60)); sprRect(t, 19, 28, 22, 31, RGB565(120, 90, 60)); }
  { uint16_t *t = spr(SP_TURRET); sprClear(t);      // tripod + armoured head + red eye
    sprRect(t, 9, 10, 23, 21, RGB565(90, 95, 105)); sprRect(t, 9, 10, 23, 11, RGB565(150, 155, 165));
    sprEllipse(t, 16, 15, 3, 3, RGB565(255, 40, 30), RGB565(150, 10, 10)); sprRect(t, 23, 14, 30, 16, RGB565(60, 60, 70));
    for (int k = 0; k < 9; k++) { sprPut(t, 15 - k, 22 + k, RGB565(60, 60, 70)); sprPut(t, 17 + k, 22 + k, RGB565(60, 60, 70)); sprPut(t, 16, 22 + k, RGB565(60, 60, 70)); } }
  { uint16_t *t = spr(SP_BAT); sprClear(t);         // wings in a V, body, red eyes
    for (int k = 0; k < 13; k++) { sprRect(t, 2 + k, 8 + k / 2, 2 + k, 14 + k / 3, RGB565(50, 30, 45)); sprRect(t, 29 - k, 8 + k / 2, 29 - k, 14 + k / 3, RGB565(50, 30, 45)); }
    sprEllipse(t, 16, 16, 4, 6, RGB565(80, 50, 70), RGB565(40, 20, 35)); sprPut(t, 14, 14, RGB565(255, 40, 40)); sprPut(t, 18, 14, RGB565(255, 40, 40)); }
  { uint16_t *t = spr(SP_BOMBER); sprClear(t);
    sprEllipse(t, 16, 20, 11, 11, RGB565(230, 120, 30), RGB565(150, 70, 15));
    sprRect(t, 6, 19, 26, 21, RGB565(70, 40, 20));
    sprRect(t, 11, 14, 13, 16, RGB565(255, 250, 200)); sprRect(t, 19, 14, 21, 16, RGB565(255, 250, 200));
    sprPut(t, 12, 15, BLACK565); sprPut(t, 20, 15, BLACK565);
    sprRect(t, 15, 5, 16, 9, RGB565(60, 50, 40)); sprRect(t, 14, 3, 17, 4, RGB565(255, 230, 80)); }
  genBall(spr(SP_FIREBALL), RGB565(255, 250, 200), RGB565(255, 150, 40), RGB565(200, 40, 20), 13);
  genBall(spr(SP_ROCKET),   RGB565(255, 255, 255), RGB565(255, 220, 90), RGB565(255, 120, 30), 13);
  genBall(spr(SP_PLASMA),   RGB565(230, 250, 255), RGB565(80, 180, 255), RGB565(30, 60, 200), 13);
  genBall(spr(SP_GRENADE),  RGB565(90, 110, 60),   RGB565(60, 80, 40),   RGB565(30, 40, 20), 8);
  { uint16_t *t = spr(SP_COIN); sprClear(t);
    sprEllipse(t, 16, 24, 7, 7, RGB565(255, 210, 50), RGB565(180, 130, 20));
    sprRect(t, 15, 20, 17, 28, RGB565(200, 150, 30)); }
  const uint16_t STEEL = RGB565(70, 72, 82), STEELL = RGB565(130, 132, 145), WOOD = RGB565(120, 75, 40);
  { uint16_t *t = spr(SP_G_PISTOL); sprClear(t);
    sprRect(t, 9, 21, 23, 24, STEEL); sprRect(t, 9, 21, 23, 21, STEELL); sprRect(t, 18, 25, 22, 30, WOOD); }
  { uint16_t *t = spr(SP_G_DUAL); sprClear(t);
    sprRect(t, 2, 22, 14, 24, STEEL); sprRect(t, 10, 25, 13, 30, WOOD);
    sprRect(t, 17, 20, 29, 22, STEELL); sprRect(t, 25, 23, 28, 28, WOOD); }
  { uint16_t *t = spr(SP_G_SMG); sprClear(t);
    sprRect(t, 6, 20, 25, 24, STEEL); sprRect(t, 1, 21, 6, 22, STEELL); sprRect(t, 14, 25, 16, 30, STEELL);
    sprRect(t, 21, 25, 24, 29, WOOD); sprRect(t, 26, 21, 30, 23, STEEL); }
  { uint16_t *t = spr(SP_G_SHOTGUN); sprClear(t);
    sprRect(t, 1, 21, 20, 22, STEEL); sprRect(t, 1, 23, 20, 24, STEELL); sprRect(t, 6, 25, 12, 26, WOOD);
    sprRect(t, 20, 21, 30, 25, WOOD); sprRect(t, 25, 26, 30, 28, WOOD); }
  { uint16_t *t = spr(SP_G_SNIPER); sprClear(t);
    sprRect(t, 0, 22, 22, 23, STEEL); sprRect(t, 8, 18, 18, 20, RGB565(30, 30, 35)); sprRect(t, 7, 18, 7, 20, STEELL);
    sprRect(t, 18, 22, 31, 26, RGB565(50, 60, 45)); sprRect(t, 20, 27, 22, 30, STEEL); }
  { uint16_t *t = spr(SP_G_RPG); sprClear(t);
    sprRect(t, 4, 19, 29, 24, RGB565(85, 100, 50)); sprRect(t, 0, 18, 4, 25, RGB565(160, 60, 40));
    sprRect(t, 29, 18, 31, 25, RGB565(40, 40, 40)); sprRect(t, 15, 25, 17, 30, STEEL); sprRect(t, 10, 17, 13, 18, STEEL); }
  { uint16_t *t = spr(SP_G_SAW); sprClear(t);
    sprRect(t, 16, 19, 30, 27, RGB565(220, 110, 30)); sprRect(t, 1, 21, 16, 25, STEELL);
    for (int x = 1; x < 16; x += 2) { sprPut(t, x, 20, STEEL); sprPut(t, x, 26, STEEL); } sprRect(t, 24, 16, 29, 18, STEEL); }
  { uint16_t *t = spr(SP_G_FLAMER); sprClear(t);
    sprRect(t, 4, 21, 22, 23, STEEL); sprRect(t, 12, 24, 22, 30, RGB565(190, 40, 30)); sprRect(t, 1, 20, 4, 24, RGB565(40, 40, 40));
    sprRect(t, 22, 21, 28, 25, WOOD); }
  { uint16_t *t = spr(SP_G_PLASMA); sprClear(t);
    sprRect(t, 4, 19, 26, 25, RGB565(60, 80, 110)); sprRect(t, 8, 21, 20, 23, RGB565(90, 200, 255)); sprRect(t, 0, 21, 4, 23, STEELL);
    sprRect(t, 18, 26, 21, 30, STEEL); }
  { uint16_t *t = spr(SP_G_RAIL); sprClear(t);
    sprRect(t, 0, 20, 26, 21, STEELL); sprRect(t, 0, 24, 26, 25, STEELL); sprRect(t, 2, 22, 24, 23, RGB565(60, 255, 220));
    sprRect(t, 22, 19, 31, 27, RGB565(40, 50, 60)); sprRect(t, 24, 28, 26, 30, STEEL); }
  { uint16_t *t = spr(SP_G_GREN); sprClear(t);
    sprRect(t, 2, 19, 18, 25, RGB565(70, 80, 50)); sprEllipse(t, 12, 22, 4, 4, RGB565(50, 55, 40), RGB565(30, 35, 25));
    sprRect(t, 18, 21, 29, 25, WOOD); sprRect(t, 15, 26, 17, 30, STEEL); }
  auto box = [&](uint16_t *t, uint16_t c, uint16_t edge) {
    sprClear(t);
    for (int y = 16; y < 32; y++) for (int x = 5; x < 27; x++)
      sprPut(t, x, y, (x == 5 || x == 26 || y == 16 || y == 31) ? edge : c);
  };
  { uint16_t *t = spr(SP_A_BULLET); box(t, RGB565(85, 100, 45), RGB565(40, 50, 25));
    for (int x = 8; x < 25; x += 4) sprRect(t, x, 18, x + 1, 23, RGB565(230, 190, 80)); }
  { uint16_t *t = spr(SP_A_SHELL); box(t, RGB565(150, 30, 25), RGB565(80, 15, 10));
    for (int x = 8; x < 25; x += 5) { sprRect(t, x, 18, x + 2, 25, RGB565(210, 40, 30)); sprRect(t, x, 24, x + 2, 25, RGB565(230, 190, 80)); } }
  { uint16_t *t = spr(SP_A_HEAVY); box(t, RGB565(45, 55, 50), RGB565(20, 25, 22));
    for (int x = 9; x < 24; x += 6) sprRect(t, x, 17, x + 2, 27, RGB565(240, 200, 90)); }
  { uint16_t *t = spr(SP_A_ROCKET); sprClear(t);
    for (int k = 0; k < 2; k++) { int y = 22 + k * 5;
      sprRect(t, 4, y, 24, y + 3, RGB565(90, 100, 60)); sprRect(t, 24, y, 29, y + 3, RGB565(200, 50, 40)); } }
  { uint16_t *t = spr(SP_A_CELL); box(t, RGB565(40, 60, 90), RGB565(20, 30, 50));
    sprRect(t, 9, 19, 22, 27, RGB565(80, 200, 255)); sprRect(t, 13, 17, 18, 18, STEELL); }
  { uint16_t *t = spr(SP_A_FUEL); sprClear(t);
    sprRect(t, 8, 14, 23, 31, RGB565(200, 40, 30)); sprRect(t, 8, 14, 23, 15, RGB565(240, 90, 70));
    sprRect(t, 12, 10, 15, 13, STEEL); sprRect(t, 11, 20, 20, 24, RGB565(250, 220, 60)); }
  { uint16_t *t = spr(SP_A_GREN); box(t, RGB565(70, 80, 50), RGB565(35, 40, 25));
    for (int x = 9; x < 24; x += 7) sprEllipse(t, x + 1, 23, 3, 4, RGB565(50, 60, 40), RGB565(30, 35, 25)); }
  { uint16_t *t = spr(SP_MEDKIT); sprClear(t);
    for (int y = 12; y < 32; y++) for (int x = 4; x < 28; x++) {
      bool edge = (x == 4 || x == 27 || y == 12 || y == 31);
      bool cross = (x >= 13 && x <= 18 && y >= 15 && y <= 29) || (y >= 19 && y <= 24 && x >= 8 && x <= 23);
      sprPut(t, x, y, edge ? RGB565(150, 150, 150) : cross ? RGB565(220, 30, 30) : RGB565(235, 235, 235));
    } }
}

// ---------------------------------------------------------------------------
// Floor / ceiling gradient
// ---------------------------------------------------------------------------
static uint16_t ceilGrad[VIEW_H];
static uint16_t floorGrad[VIEW_H];
static void genGradients() {
  for (int y = 0; y < VIEW_H; y++) {
    float t = (float)y / HORIZON; if (t > 1) t = 1;
    ceilGrad[y] = shade(RGB565(58, 64, 92), (uint16_t)((1.0f - 0.70f * t) * 256));
    float u = (float)(y - HORIZON) / (VIEW_H - HORIZON); if (u < 0) u = 0;
    floorGrad[y] = shade(RGB565(96, 76, 56), (uint16_t)((0.30f + 0.70f * u) * 256));
  }
}

// ---------------------------------------------------------------------------
// Damage types, ammo, guns, enemies
// ---------------------------------------------------------------------------
enum DType { DT_BAL, DT_EXP, DT_FIRE, DT_ENER, DT_MELEE, DT_N };

enum AmmoT { AM_BULLET, AM_SHELL, AM_HEAVY, AM_ROCKET, AM_CELL, AM_FUEL, AM_GREN, AM_N, AM_NONE = 255 };
static const char *const kAmmoName[AM_N] = { "9MM", "SHELL", "HEAVY", "ROCKET", "CELL", "FUEL", "GREN" };
static const int  kAmmoMax[AM_N]  = { 150, 30, 15, 8, 200, 200, 12 };
static const int  kAmmoBox[AM_N]  = { 18, 6, 4, 2, 30, 40, 3 };       // floor pickups (sparse)
static const int  kAmmoPack[AM_N] = { 30, 8, 5, 2, 50, 80, 4 };       // shop packs
static const int  kAmmoPrice[AM_N]= { 6, 8, 10, 12, 10, 8, 10 };
static const uint8_t kAmmoSpr[AM_N] = { SP_A_BULLET, SP_A_SHELL, SP_A_HEAVY, SP_A_ROCKET, SP_A_CELL, SP_A_FUEL, SP_A_GREN };

enum FireMode { M_HIT, M_ROCKET, M_PLASMA, M_GREN, M_FLAME, M_SAW, M_RAIL };
enum GunId { G_PISTOL, G_DUAL, G_SMG, G_SHOTGUN, G_SNIPER, G_RPG, G_SAW, G_FLAMER, G_PLASMA, G_RAIL, G_GREN, G_N };
struct GunDef {
  const char *name; uint8_t ammo, perShot; float cd; int dmg; uint8_t dtype;
  uint8_t pellets; float spread, window, range; bool ap; uint8_t pierce;
  uint8_t mode; bool autoFire; int pickupAmmo; uint8_t spr; int unlock, price;
};
static const GunDef kGuns[G_N] = {
  // name       ammo     per cd     dmg  dtype     pel spread win    range  ap   prc mode      auto  ammo spr           lvl  $
  {"PISTOL",   AM_BULLET, 1, 0.32f,  25, DT_BAL,   1, 0.00f, 0.20f, 16.f, false, 1, M_HIT,    false, 12, SP_G_PISTOL,  1,   0},
  {"DUAL",     AM_BULLET, 1, 0.15f,  20, DT_BAL,   1, 0.03f, 0.18f, 15.f, false, 1, M_HIT,    false, 24, SP_G_DUAL,    1,  25},
  {"SMG",      AM_BULLET, 1, 0.09f,  12, DT_BAL,   1, 0.05f, 0.16f, 14.f, false, 1, M_HIT,    true,  40, SP_G_SMG,     2,  30},
  {"SHOTGUN",  AM_SHELL,  1, 0.85f,  14, DT_BAL,   7, 0.16f, 0.08f,  9.f, false, 1, M_HIT,    false,  8, SP_G_SHOTGUN, 2,  40},
  {"SNIPER",   AM_HEAVY,  1, 1.30f, 160, DT_BAL,   1, 0.00f, 0.09f, 30.f, true,  3, M_HIT,    false,  5, SP_G_SNIPER,  3,  60},
  {"RPG",      AM_ROCKET, 1, 1.20f, 170, DT_EXP,   1, 0.00f, 0.00f,  0.f, true,  0, M_ROCKET, false,  3, SP_G_RPG,     4,  80},
  {"CHAINSAW", AM_NONE,   0, 0.08f,  10, DT_MELEE, 1, 0.00f, 0.35f, 1.4f, false, 1, M_SAW,    true,   0, SP_G_SAW,     1,  20},
  {"FLAMER",   AM_FUEL,   1, 0.08f,   4, DT_FIRE,  1, 0.35f, 0.00f, 4.5f, false, 9, M_FLAME,  true,  60, SP_G_FLAMER,  4,  45},
  {"PLASMA",   AM_CELL,   1, 0.11f,  20, DT_ENER,  1, 0.03f, 0.00f,  0.f, false, 1, M_PLASMA, true,  40, SP_G_PLASMA,  5,  60},
  {"RAILGUN",  AM_CELL,   5, 1.40f, 220, DT_ENER,  1, 0.00f, 0.06f, 40.f, true, 16, M_RAIL,   false, 25, SP_G_RAIL,    7, 100},
  {"GRENADE",  AM_GREN,   1, 0.90f, 130, DT_EXP,   1, 0.00f, 0.00f,  0.f, true,  0, M_GREN,   false,  3, SP_G_GREN,    3,  50},
};
static const float SHOTGUN_FALLOFF = 7.0f;
static const float RPG_RADIUS = 2.6f, RPG_SPEED = 9.0f, SELF_DMG = 0.4f;
static const float GREN_RADIUS = 2.2f, GREN_SPEED = 6.5f, GREN_FUSE = 1.4f;
static const float RAIL_CHARGE = 0.45f;
static const float BURN_S = 2.0f, BURN_DPS = 8.0f;

enum EKind { K_MELEE, K_RANGED, K_BOMBER, K_BOSS, K_MANC, K_LEAPER, K_SHIELD, K_NECRO, K_TURRET, K_BAT };
struct EnemyDef {
  const char *name; uint8_t spr; int hp; float spd; float scale; int dmg; float cd;
  float wake; uint8_t kind; uint8_t alpha; int unlock; int coins;
  float mult[DT_N];          // damage taken per type: ballistic, explosive, fire, energy, melee
};
enum EType { E_IMP, E_RUNT, E_SPITTER, E_BRUTE, E_BOMBER, E_SHADE, E_BARON,
             E_MANCUBUS, E_LEAPER, E_SHIELD, E_NECRO, E_TURRET, E_BAT, E_N };
static const EnemyDef kEnemies[E_N] = {
  // name       spr          hp   spd   scale dmg cd    wake kind      a   lvl $    BAL  EXP  FIRE ENER MELEE
  {"IMP",      SP_IMP,      50, 1.25f, 1.00f, 7, 1.4f, 0.7f, K_MELEE,   0,  1, 2, {1.0f,1.0f,1.0f,1.0f,1.2f}},
  {"RUNT",     SP_RUNT,     22, 2.70f, 0.62f, 4, 0.7f, 0.35f,K_MELEE,   0,  2, 2, {1.0f,1.2f,1.3f,1.0f,1.5f}},
  {"SPITTER",  SP_SPITTER,  40, 1.10f, 0.95f, 9, 2.2f, 0.5f, K_RANGED,  0,  3, 3, {1.0f,1.0f,0.6f,1.3f,1.0f}},
  {"BRUTE",    SP_BRUTE,   220, 0.75f, 1.35f,18, 1.8f, 1.0f, K_MELEE,   0,  6, 6, {0.45f,1.1f,0.8f,1.2f,0.5f}},
  {"BOMBER",   SP_BOMBER,   30, 2.10f, 0.80f,30, 0.0f, 0.4f, K_BOMBER,  0,  4, 2, {1.0f,1.0f,2.0f,1.0f,0.6f}},
  {"SHADE",    SP_SHADE,    60, 1.70f, 1.00f, 9, 1.2f, 0.5f, K_MELEE, 120,  7, 4, {0.5f,0.8f,1.2f,1.8f,0.7f}},
  {"BARON",    SP_BARON,   600, 0.90f, 1.70f,22, 1.6f, 0.3f, K_BOSS,    0,  5,25, {0.7f,1.0f,0.6f,1.3f,0.6f}},
  {"MANCUBUS", SP_MANCUBUS,350, 0.60f, 1.55f,12, 3.2f, 0.6f, K_MANC,    0, 11,10, {0.7f,1.3f,1.5f,1.0f,0.8f}},
  {"LEAPER",   SP_LEAPER,   45, 1.40f, 0.90f,14, 2.5f, 0.4f, K_LEAPER,  0,  6, 4, {1.2f,1.0f,1.0f,0.8f,1.6f}},
  {"SHIELD",   SP_SHIELD_F, 90, 1.10f, 1.05f,10, 1.4f, 0.6f, K_SHIELD,  0,  8, 5, {1.5f,1.0f,1.2f,1.5f,1.5f}},
  {"NECRO",    SP_NECRO,    80, 1.00f, 1.00f, 8, 6.0f, 0.5f, K_NECRO,   0,  9, 6, {1.0f,1.0f,1.6f,0.8f,1.2f}},
  {"TURRET",   SP_TURRET,  120, 0.00f, 0.80f, 5, 2.2f, 0.3f, K_TURRET,  0,  4, 4, {0.6f,1.6f,0.2f,1.4f,0.3f}},
  {"BAT",      SP_BAT,      12, 3.20f, 0.45f, 3, 0.6f, 0.2f, K_BAT,     0,  3, 1, {1.0f,1.5f,2.0f,0.8f,1.0f}},
};
static const float RUNT_SPD0 = 1.7f, RUNT_SPD_STEP = 0.1f, RUNT_SPD_MAX = 2.8f;   // cells/s
static const float CLEAR_S = 10.0f;     // after the last kill: time to collect coins
static const float SHIELD_FRONT = 0.1f;   // Shield: ballistic / energy / melee from the front
static const float SHIELD_ARC   = 1.2f;   // rad either side of its facing counted as "front"

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
static const float FOV        = 0.66f;
static const float MOVE_SPD   = 2.6f;
static const float SPRINT     = 1.4f;
static const float TURN_SPD   = 2.7f;
static const float TURN_BOOST = 0.6f;
static const float PLAYER_R   = 0.24f;
static const float AGGRO      = 10.0f;
static const float GRACE_S    = 2.0f;
static const float MELEE      = 1.1f;

#define MAP_MAX   32
#define ENEMY_MAX 24
#define ITEM_MAX  40
#define PROJ_MAX  24
#define FX_MAX    10

struct Enemy {
  uint8_t type;
  float x, y;
  int   health, maxHp, dmg;
  float spd;
  bool  alive, awake, boom, revived;
  float alert, hurt, atkCd, shootCd;
  float kbx, kby, dying, burn, face;
  uint8_t phase; float phaseT, lvx, lvy, wob;   // leaper / bat state
};
enum ItemKind { IT_MED, IT_AMMO, IT_GUN, IT_COIN };
struct Item { float x, y; uint8_t kind, sub; int amount; bool live; };
enum ProjKind { PK_FIRE, PK_BOLT, PK_ROCKET, PK_PLASMA, PK_GREN };
struct Proj { float x, y, vx, vy, ttl; int dmg; uint8_t kind; bool mine, live; };
struct Fx   { float x, y, t, dur, size; uint8_t spr; bool live; };

static uint8_t grid[MAP_MAX][MAP_MAX];
static int   mapN = 16;
static inline uint8_t cell(int x, int y) {
  if (x < 0 || y < 0 || x >= mapN || y >= mapN) return 1;
  return grid[y][x];
}

static Enemy enemy[ENEMY_MAX]; static int enemyN;
static Item  item[ITEM_MAX];   static int itemN;
static Proj  proj[PROJ_MAX];
static Fx    fx[FX_MAX];
static int   level = 1, bestLevel = 0;

static float posX, posY, ang;
static float dirX, dirY, planeX, planeY;
static int   health, kills, coins, levelCoins;
static int   ammo[AM_N];
static bool  owned[G_N];
static uint8_t gun = G_PISTOL;
static float zbuf[SCREEN_W];

enum State { TITLE, PLAY, DEAD, WIN, SHOP };
static State state;

static float gunRecoil, muzzle, bob, bobPhase, swayX, hurtFlash, shakeT, shakeAmp, boomFlash;
static float dmgLeft, dmgRight;
static float pickFlash; static uint16_t pickCol;
static float toastT; static char toastMsg[28] = "";
static float hitMarkT; static uint16_t hitMarkCol;
static float flameT, sawT, railCharge, railBeamT, dualSide;
static float clearT;                    // > 0: ROUND CLEARED countdown running
static int   hz = HORIZON;
static uint32_t lastMs, lastActivityMs;

static bool  joyActive, prevFinger, prevBtn;
static int   knobX, knobY;
static uint32_t btnDownMs, lastPressMs;
static float fireCd, clickCd;
static float walkVel, strafeVel, turnVel;
static float spin;
static float tiltLat, tiltZero;
static bool  tiltOk, tiltZeroed;
static int16_t rawAx, rawAy, rawAz;
static float ammoRegen;
static float exitHold;
static uint32_t playStartMs;

static const int   JOY_CX     = 62;
static const int   JOY_CY     = SCREEN_H - 62;
static const float JOY_DEAD   = 8.0f;
static const float JOY_RADIUS = 46.0f;
static const float JOY_ACCEL  = 7.0f;
static const float JOY_DECEL  = 12.0f;
static const float STRAFE_SPD = 0.85f;
static const float TILT_DEAD  = 0.07f;
static const float TILT_FULL  = 0.42f;
static const float TILT_LP    = 18.0f;
static const float TILT_DRIFT = 5.0f;
static const float TILT_SIGN  = 1.0f;
static const uint32_t DOUBLE_MS = 350;
static const int   FIRE_CX    = SCREEN_W - 48, FIRE_CY = SCREEN_H - 48, FIRE_R = 30;
// Weapon switch button: top centre.
static const int   SW_X0 = 128, SW_X1 = 196, SW_Y1 = 34;
static const uint32_t EXIT_SHOW_MS = 500, EXIT_MS = 2000;
static volatile bool sDoomExit = false;
bool doomExitRequested() { bool e = sDoomExit; sDoomExit = false; return e; }

static void setDir() {
  dirX = cosf(ang); dirY = sinf(ang);
  planeX = -dirY * FOV; planeY = dirX * FOV;
}

static void toast(const char *m, float s) {
  strncpy(toastMsg, m, sizeof toastMsg - 1); toastMsg[sizeof toastMsg - 1] = 0; toastT = s;
}

static uint32_t rngS = 1;
static inline uint32_t rnd() { rngS ^= rngS << 13; rngS ^= rngS >> 17; rngS ^= rngS << 5; return rngS; }
static inline int rndi(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }
static inline float rndf() { return (rnd() & 0xFFFF) / 65536.0f; }

// ---------------------------------------------------------------------------
// Level generation
// ---------------------------------------------------------------------------
static uint8_t reach[MAP_MAX][MAP_MAX];
static uint16_t bfsQ[MAP_MAX * MAP_MAX];
static int startCX, startCY;

static int floodFrom(int sx, int sy) {
  memset(reach, 0, sizeof reach);
  int h = 0, t = 0, n = 0;
  bfsQ[t++] = (uint16_t)(sy * MAP_MAX + sx); reach[sy][sx] = 1;
  while (h < t) {
    int c = bfsQ[h++], x = c % MAP_MAX, y = c / MAP_MAX; n++;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
      int nx = x + dx[k], ny = y + dy[k];
      if (cell(nx, ny) || reach[ny][nx]) continue;
      reach[ny][nx] = 1; bfsQ[t++] = (uint16_t)(ny * MAP_MAX + nx);
    }
  }
  return n;
}

static bool nearStart(int x, int y) { return abs(x - startCX) <= 2 && abs(y - startCY) <= 2; }

static void genMap(int n) {
  for (int attempt = 0; attempt < 10; attempt++) {
    rngS = 0x9E3779B9u * (uint32_t)(n * 16 + attempt + 1) ^ 0x5bd1e995u;
    if (!rngS) rngS = 1;
    mapN = 14 + 2 * n; if (mapN > MAP_MAX) mapN = MAP_MAX;
    memset(grid, 0, sizeof grid);
    for (int i = 0; i < mapN; i++) { grid[0][i] = grid[mapN - 1][i] = 1; grid[i][0] = grid[i][mapN - 1] = 1; }
    startCX = mapN / 2; startCY = mapN - 2;
    int walls = mapN / 5 + n / 3; if (walls > 10) walls = 10;
    for (int w = 0; w < walls; w++) {
      bool horiz = rnd() & 1;
      int len = 3 + rndi(mapN / 3);
      int x = 2 + rndi(mapN - 4), y = 2 + rndi(mapN - 4);
      int tex = 1 + rndi(4);
      for (int k = 0; k < len; k++) {
        int cx = horiz ? x + k : x, cy = horiz ? y : y + k;
        if (cx <= 0 || cy <= 0 || cx >= mapN - 1 || cy >= mapN - 1 || nearStart(cx, cy)) continue;
        if (k == len / 2) continue;
        grid[cy][cx] = tex;
      }
    }
    int pillars = mapN * mapN / 45;
    for (int p = 0; p < pillars; p++) {
      int w = 1 + rndi(2), h = 1 + rndi(2), x = 2 + rndi(mapN - 4), y = 2 + rndi(mapN - 4);
      int tex = 1 + rndi(4);
      for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) {
        int cx = x + dx, cy = y + dy;
        if (cx < mapN - 1 && cy < mapN - 1 && !nearStart(cx, cy)) grid[cy][cx] = tex;
      }
    }
    int got = floodFrom(startCX, startCY);
    int inner = (mapN - 2) * (mapN - 2);
    for (int y = 1; y < mapN - 1; y++) for (int x = 1; x < mapN - 1; x++)
      if (!grid[y][x] && !reach[y][x]) grid[y][x] = 1;
    if (got * 10 >= inner * 6) return;
  }
}

static bool pickCell(float minD, float &ox, float &oy, bool byWall = false) {
  for (int tries = 0; tries < 300; tries++) {
    int x = 1 + rndi(mapN - 2), y = 1 + rndi(mapN - 2);
    if (!reach[y][x] || grid[y][x]) continue;
    float dx = x - startCX, dy = y - startCY;
    if (dx * dx + dy * dy < minD * minD) continue;
    if (byWall && tries < 250 && !(cell(x + 1, y) || cell(x - 1, y) || cell(x, y + 1) || cell(x, y - 1))) continue;
    bool used = false;
    for (int i = 0; i < enemyN && !used; i++) used = (int)enemy[i].x == x && (int)enemy[i].y == y;
    for (int i = 0; i < itemN && !used; i++) used = item[i].live && (int)item[i].x == x && (int)item[i].y == y;
    if (used) continue;
    ox = x + 0.5f; oy = y + 0.5f;
    return true;
  }
  return false;
}

static void addItem(float x, float y, uint8_t kind, uint8_t sub, int amount) {
  for (int i = 0; i < itemN; i++) if (!item[i].live) { item[i] = {x, y, kind, sub, amount, true}; return; }
  if (itemN < ITEM_MAX) item[itemN++] = {x, y, kind, sub, amount, true};
}

static void addEnemy(uint8_t type, float x, float y, int n) {
  if (enemyN >= ENEMY_MAX) return;
  const EnemyDef &d = kEnemies[type];
  float hpS = 1.0f + 0.07f * (n - 1), spS = 1.0f + 0.02f * (n - 1), dmS = 1.0f + 0.04f * (n - 1);
  if (spS > 1.3f) spS = 1.3f;
  Enemy &e = enemy[enemyN++];
  memset(&e, 0, sizeof e);
  e.type = type; e.x = x; e.y = y;
  e.maxHp = e.health = (int)(d.hp * hpS);
  e.dmg = (int)(d.dmg * dmS + 0.5f);
  e.spd = d.spd * spS;
  // Runts: slow-ish when they first appear (level 2), faster every level.
  if (type == E_RUNT) { e.spd = RUNT_SPD0 + RUNT_SPD_STEP * (n - d.unlock); if (e.spd > RUNT_SPD_MAX) e.spd = RUNT_SPD_MAX; }
  e.alive = true;
  e.shootCd = 1.0f + rndf() * d.cd;
  e.face = rndf() * 6.283f;
  e.wob = rndf() * 6.283f;
}

static uint8_t pickEnemyType(int n) {
  static const uint8_t order[] = { E_IMP, E_RUNT, E_BAT, E_SPITTER, E_BOMBER, E_TURRET, E_BRUTE,
                                   E_LEAPER, E_SHADE, E_SHIELD, E_NECRO, E_MANCUBUS };
  static const uint8_t weight[] = { 4, 3, 2, 2, 2, 1, 1, 2, 2, 2, 1, 1 };
  const int K = sizeof order;
  int w[K], sum = 0;
  for (int i = 0; i < K; i++) {
    const EnemyDef &d = kEnemies[order[i]];
    int ww = n >= d.unlock ? weight[i] + (order[i] == E_BRUTE ? (n - d.unlock) / 3 : 0) : 0;
    w[i] = ww; sum += ww;
  }
  int r = rndi(sum);
  for (int i = 0; i < K; i++) { if (r < w[i]) return order[i]; r -= w[i]; }
  return E_IMP;
}

static uint8_t pickGunDrop(int n) {
  uint8_t pool[G_N]; int k = 0;
  for (int g = 1; g < G_N; g++) if (n >= kGuns[g].unlock && !owned[g]) pool[k++] = (uint8_t)g;
  if (!k) return 255;
  return pool[rndi(k)];
}

static void loadLevel(int n, bool fresh) {
  level = n;
  genMap(n);
  posX = startCX + 0.5f; posY = startCY + 0.5f; ang = -(float)M_PI / 2;
  setDir();
  if (fresh) {
    health = 100; gun = G_PISTOL; coins = 0;
    memset(owned, 0, sizeof owned); owned[G_PISTOL] = true;
    memset(ammo, 0, sizeof ammo); ammo[AM_BULLET] = 36;
  }
  kills = 0; enemyN = 0; itemN = 0; levelCoins = 0;
  memset(proj, 0, sizeof proj); memset(fx, 0, sizeof fx);
  bool boss = n % 5 == 0;
  int count = 2 + n - (boss ? 2 : 0);
  if (count > ENEMY_MAX - 4) count = ENEMY_MAX - 4;
  float minD = mapN * 0.35f; if (minD < 5) minD = 5;
  float x, y;
  if (boss && pickCell(mapN * 0.5f, x, y)) addEnemy(E_BARON, x, y, n);
  for (int i = 0; i < count && enemyN < ENEMY_MAX; i++) {
    uint8_t t = pickEnemyType(n);
    if (!pickCell(minD, x, y, t == E_TURRET)) continue;
    if (t == E_BAT) {                                // bats come in a swarm of three
      for (int k = 0; k < 3; k++) addEnemy(E_BAT, x + (k - 1) * 0.25f, y + (k & 1) * 0.2f, n);
      i += 1;
    } else {
      addEnemy(t, x, y, n);
    }
  }
  // sparse floor items: one medkit (two later), a 9mm box, a box for the gun
  // in hand, a gun you don't have, coins
  int meds = 1 + (n >= 4 ? 1 : 0) + (boss ? 1 : 0);
  for (int i = 0; i < meds; i++) if (pickCell(3, x, y)) addItem(x, y, IT_MED, 0, 25);
  if (pickCell(3, x, y)) addItem(x, y, IT_AMMO, AM_BULLET, kAmmoBox[AM_BULLET]);
  if (kGuns[gun].ammo != AM_NONE && kGuns[gun].ammo != AM_BULLET && pickCell(3, x, y))
    addItem(x, y, IT_AMMO, kGuns[gun].ammo, kAmmoBox[kGuns[gun].ammo]);
  uint8_t g = pickGunDrop(n);
  if (g != 255 && pickCell(4, x, y)) addItem(x, y, IT_GUN, g, kGuns[g].pickupAmmo);
  int piles = 3 + n / 2;
  for (int i = 0; i < piles; i++) if (pickCell(2, x, y)) addItem(x, y, IT_COIN, 0, 1 + rndi(3));

  joyActive = false;
  walkVel = strafeVel = turnVel = spin = 0; ammoRegen = 0; fireCd = clickCd = 0;
  railCharge = railBeamT = flameT = sawT = 0;
  clearT = 0;
  tiltZeroed = false;
  playStartMs = millis();
  gunRecoil = muzzle = bob = bobPhase = swayX = hurtFlash = shakeT = boomFlash = 0;
  dmgLeft = dmgRight = pickFlash = hitMarkT = 0;
  char mix[96] = ""; int ml = 0;
  for (int t = 0; t < E_N; t++) {
    int c = 0; for (int i = 0; i < enemyN; i++) c += enemy[i].type == t;
    if (c && ml < (int)sizeof mix - 12) ml += snprintf(mix + ml, sizeof mix - ml, "%s%s:%d", ml ? " " : "", kEnemies[t].name, c);
  }
  EWLOGI("DOOM", "mix %s", mix);
  char m[24]; snprintf(m, sizeof m, boss ? "LEVEL %d - BOSS" : "LEVEL %d", n);
  toast(m, 1.6f);
  EWLOGI("DOOM", "level %d fresh=%d map=%d enemies=%d items=%d hp=%d gun=%s $%d", n, (int)fresh, mapN, enemyN, itemN, health, kGuns[gun].name, coins);
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------
static bool readFinger(int &x, int &y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)I2C_ADDR_TOUCH, 6) != 6) {
    bool live = false; uint16_t ux = 0, uy = 0;
    ewsimTouch(live, ux, uy);
    touchToLogical(ux, uy, x, y);
    return live;
  }
  Wire.read();
  uint8_t pts = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  bool live = pts != 0;
  uint16_t ux = ((xh & 0x0F) << 8) | xl, uy = ((yh & 0x0F) << 8) | yl;
  ewsimTouch(live, ux, uy);
  touchToLogical(ux, uy, x, y);
  return live;
}

// ---------------------------------------------------------------------------
// Movement, collision, line of sight
// ---------------------------------------------------------------------------
static void tryMove(float &px, float &py, float dx, float dy) {
  const float m = 0.20f;
  float tx = px + dx + (dx > 0 ? m : (dx < 0 ? -m : 0));
  if (cell((int)tx, (int)py) == 0) px += dx;
  float ty = py + dy + (dy > 0 ? m : (dy < 0 ? -m : 0));
  if (cell((int)px, (int)ty) == 0) py += dy;
}

static void collideCircle(float &px, float &py, float r) {
  for (int it = 0; it < 2; it++) {
    int x0 = (int)floorf(px - r), x1 = (int)floorf(px + r);
    int y0 = (int)floorf(py - r), y1 = (int)floorf(py + r);
    for (int cy = y0; cy <= y1; cy++)
      for (int cx = x0; cx <= x1; cx++) {
        if (!cell(cx, cy)) continue;
        float nx = px < cx ? cx : (px > cx + 1 ? cx + 1 : px);
        float ny = py < cy ? cy : (py > cy + 1 ? cy + 1 : py);
        float dx = px - nx, dy = py - ny, d2 = dx * dx + dy * dy;
        if (d2 >= r * r) continue;
        float d = sqrtf(d2);
        if (d > 1e-4f) { px += dx / d * (r - d); py += dy / d * (r - d); }
        else {
          float l = px - cx, rr = cx + 1 - px, t = py - cy, b = cy + 1 - py;
          float m = fminf(fminf(l, rr), fminf(t, b));
          if (m == l) px = cx - r; else if (m == rr) px = cx + 1 + r;
          else if (m == t) py = cy - r; else py = cy + 1 + r;
        }
      }
  }
}
static void movePlayer(float dx, float dy) {
  float len = sqrtf(dx * dx + dy * dy);
  int n = (int)(len / 0.12f) + 1;
  for (int i = 0; i < n; i++) {
    posX += dx / n; posY += dy / n;
    collideCircle(posX, posY, PLAYER_R);
  }
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

static void worldToCamera(float wx, float wy, float &tx, float &ty) {
  float sx = wx - posX, sy = wy - posY;
  float inv = 1.0f / (planeX * dirY - dirX * planeY);
  tx = inv * (dirY * sx - dirX * sy);
  ty = inv * (-planeY * sx + planeX * sy);
}

static void shake(float s, float amp) { if (s > shakeT) shakeT = s; if (amp > shakeAmp || shakeT <= 0) shakeAmp = amp; }

static void addFx(float x, float y, float dur, float size, uint8_t sp = SP_FIREBALL) {
  for (int i = 0; i < FX_MAX; i++) if (!fx[i].live) { fx[i] = {x, y, 0, dur, size, sp, true}; return; }
}

static inline float angDiff(float a, float b) {
  float d = a - b;
  while (d > (float)M_PI) d -= 2 * (float)M_PI;
  while (d < -(float)M_PI) d += 2 * (float)M_PI;
  return d;
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------
static void hurtPlayerFrom(float sx, float sy, int dmg) {
  if (state != PLAY || dmg <= 0) return;
  if (millis() - playStartMs < (uint32_t)(GRACE_S * 1000)) return;
  health -= dmg;
  if (health < 0) health = 0;
  hurtFlash = 0.30f;
  shake(0.18f, 4);
  haptic(HapticRole::Damage);
  float tx, ty; worldToCamera(sx, sy, tx, ty);
  if (ty < 0.2f) { dmgLeft = dmgRight = 1; }
  else if (tx / ty < -0.25f) dmgLeft = 1;
  else if (tx / ty > 0.25f) dmgRight = 1;
  if (health <= 0) {
    state = DEAD;
    if (level > bestLevel) bestLevel = level;
    EWLOGI("DOOM", "dead level=%d best=%d $%d", level, bestLevel, coins);
  }
}

static void dropLoot(const Enemy &e) {
  const EnemyDef &d = kEnemies[e.type];
  addItem(e.x, e.y, IT_COIN, 0, d.coins);              // every kill pays
  uint32_t r = rndi(100);
  uint8_t at = kGuns[gun].ammo;
  if (e.type == E_BARON || e.type == E_MANCUBUS) {
    addItem(e.x + 0.3f, e.y, IT_MED, 0, 25);
    if (at != AM_NONE) addItem(e.x - 0.3f, e.y, IT_AMMO, at, kAmmoBox[at]);
    return;
  }
  if (r < 10 && at != AM_NONE) addItem(e.x + 0.25f, e.y, IT_AMMO, at, (kAmmoBox[at] + 1) / 2);
  else if (r < 15) addItem(e.x + 0.25f, e.y, IT_MED, 0, 15);
}

// Damage with type effectiveness (EnemyDef.mult), armour-piercing (ballistic
// AP ignores resistance), the Shield's front block, and a hit marker colour.
static void damageEnemy(int i, int dmg, uint8_t dtype, bool ap, float fromX, float fromY, float kb) {
  Enemy &e = enemy[i];
  if (!e.alive) return;
  const EnemyDef &d = kEnemies[e.type];
  float m = d.mult[dtype];
  if (ap && m < 1.0f && dtype != DT_FIRE) m = 1.0f;
  bool blocked = false;
  if (d.kind == K_SHIELD && (dtype == DT_BAL || dtype == DT_ENER || dtype == DT_MELEE)) {
    float toSrc = atan2f(fromY - e.y, fromX - e.x);
    if (fabsf(angDiff(toSrc, e.face)) < SHIELD_ARC) { m = SHIELD_FRONT; blocked = true; }
  }
  int hp = (int)(dmg * m + 0.5f);
  if (hp < 1 && m > 0.05f) hp = 1;
  e.health -= hp;
  e.hurt = 0.12f;
  e.awake = true;
  hitMarkT = 0.15f;
  hitMarkCol = blocked ? RGB565(120, 160, 255) : m >= 1.3f ? RGB565(80, 255, 80) : m <= 0.65f ? RGB565(140, 140, 140) : RGB565(255, 255, 255);
  if (blocked && toastT <= 0) toast("BLOCKED - FLANK IT", 0.7f);
  if (d.kind != K_TURRET) {
    float ex = e.x - fromX, ey = e.y - fromY, dd = sqrtf(ex * ex + ey * ey);
    float kbS = kb / (d.scale * d.scale);
    if (dd > 0.01f) { e.kbx += ex / dd * kbS; e.kby += ey / dd * kbS; }
  }
  if (e.health <= 0) {
    e.alive = false;
    e.dying = 0.45f;
    e.burn = 0;
    kills++;
    haptic(HapticRole::HitHeavy);
    if (d.kind == K_BOMBER) e.boom = true;
    dropLoot(e);
    EWLOGI("DOOM", "kill %s %d/%d", d.name, kills, enemyN);
  }
}

static void explode(float x, float y, float radius, int dmg, bool byPlayer) {
  addFx(x, y, 0.35f, radius * 0.9f);
  shake(0.3f, 6);
  boomFlash = 0.18f;
  haptic(HapticRole::Explosion);
  for (int i = 0; i < enemyN; i++) {
    if (!enemy[i].alive) continue;
    float dx = enemy[i].x - x, dy = enemy[i].y - y, d = sqrtf(dx * dx + dy * dy);
    if (d < radius && lineOfSight(x, y, enemy[i].x, enemy[i].y))
      damageEnemy(i, (int)(dmg * (1.0f - d / radius)), DT_EXP, false, x, y, 4.0f);
  }
  float dx = posX - x, dy = posY - y, d = sqrtf(dx * dx + dy * dy);
  if (d < radius && lineOfSight(x, y, posX, posY))
    hurtPlayerFrom(x, y, (int)(dmg * (1.0f - d / radius) * (byPlayer ? SELF_DMG : 1.0f) * (byPlayer ? 1.0f : 0.5f)));
}

// ---------------------------------------------------------------------------
// Hitscan: enemies along view angle + aoff, body within the window, nearest
// first, walls stop it.
// ---------------------------------------------------------------------------
static int hitscan(float aoff, float win, float range, int *out, int maxN) {
  float ca = cosf(aoff), sa = sinf(aoff);
  float rx = dirX * ca - dirY * sa, ry = dirX * sa + dirY * ca;
  int n = 0, all[ENEMY_MAX]; float dep[ENEMY_MAX];
  for (int i = 0; i < enemyN; i++) {
    if (!enemy[i].alive) continue;
    float ex = enemy[i].x - posX, ey = enemy[i].y - posY;
    float along = ex * rx + ey * ry;
    if (along <= 0.2f || along > range) continue;
    float lat = fabsf(ex * ry - ey * rx);
    if (lat > 0.28f * kEnemies[enemy[i].type].scale + win * along) continue;
    if (!lineOfSight(posX, posY, enemy[i].x, enemy[i].y)) continue;
    int k = n++;
    while (k > 0 && dep[k - 1] > along) { dep[k] = dep[k - 1]; all[k] = all[k - 1]; k--; }
    dep[k] = along; all[k] = i;
  }
  if (n > maxN) n = maxN;
  for (int k = 0; k < n; k++) out[k] = all[k];
  return n;
}
static int targetUnderCrosshair() {
  int o[1];
  float w = kGuns[gun].window > 0 ? kGuns[gun].window : 0.12f;
  float r = kGuns[gun].range > 0 ? kGuns[gun].range : 20.0f;
  return hitscan(0, w, r, o, 1) ? o[0] : -1;
}

static void spawnProj(float x, float y, float a, float spd, int dmg, uint8_t kind, bool mine, float ttl = 4.0f) {
  for (int i = 0; i < PROJ_MAX; i++) if (!proj[i].live) {
    proj[i] = {x, y, cosf(a) * spd, sinf(a) * spd, ttl, dmg, kind, mine, true};
    return;
  }
}

static void fireGun() {
  const GunDef &g = kGuns[gun];
  if (g.ammo != AM_NONE) ammo[g.ammo] -= g.perShot;
  int hits[ENEMY_MAX];
  switch (g.mode) {
    case M_SAW: {
      sawT = 0.15f;
      int n = hitscan(0, g.window, g.range, hits, 2);
      for (int k = 0; k < n; k++) damageEnemy(hits[k], g.dmg, g.dtype, false, posX, posY, 0.4f);
      if (n) { shake(0.05f, 2); haptic(HapticRole::HitLight); }
      return;
    }
    case M_FLAME: {
      flameT = 0.15f;
      // cone: every enemy within range and half-angle `spread`, sets it burning
      for (int i = 0; i < enemyN; i++) {
        if (!enemy[i].alive) continue;
        float ex = enemy[i].x - posX, ey = enemy[i].y - posY, d = sqrtf(ex * ex + ey * ey);
        if (d > g.range || d < 0.01f) continue;
        if (fabsf(angDiff(atan2f(ey, ex), ang)) > g.spread + 0.2f / d) continue;
        if (!lineOfSight(posX, posY, enemy[i].x, enemy[i].y)) continue;
        damageEnemy(i, g.dmg, g.dtype, false, posX, posY, 0.1f);
        if (kEnemies[enemy[i].type].mult[DT_FIRE] > 0.3f) enemy[i].burn = BURN_S;
      }
      if (rndi(3) == 0) haptic(HapticRole::HitLight);
      return;
    }
    default: break;
  }
  muzzle = 0.08f;
  bool heavy = gun == G_SNIPER || gun == G_SHOTGUN || g.mode == M_ROCKET || g.mode == M_RAIL || g.mode == M_GREN;
  gunRecoil = heavy ? 1.6f : 1.0f;
  shake(heavy ? 0.14f : 0.08f, gun == G_SHOTGUN || g.mode == M_ROCKET || g.mode == M_RAIL ? 4 : 2);
  haptic(gun == G_SMG || g.mode == M_PLASMA ? HapticRole::HitLight : HapticRole::Shoot);   // rapid fire stays light
  switch (g.mode) {
    case M_ROCKET: spawnProj(posX + dirX * 0.4f, posY + dirY * 0.4f, ang, RPG_SPEED, g.dmg, PK_ROCKET, true); return;
    case M_PLASMA: spawnProj(posX + dirX * 0.3f, posY + dirY * 0.3f, ang + (rndf() * 2 - 1) * g.spread, 12.0f, g.dmg, PK_PLASMA, true); return;
    case M_GREN:   spawnProj(posX + dirX * 0.3f, posY + dirY * 0.3f, ang, GREN_SPEED, g.dmg, PK_GREN, true, GREN_FUSE); return;
    case M_RAIL: {
      railBeamT = 0.14f;
      int n = hitscan(0, g.window, g.range, hits, g.pierce);
      for (int k = 0; k < n; k++) damageEnemy(hits[k], g.dmg, g.dtype, true, posX, posY, 5.0f);
      return;
    }
    default: break;
  }
  if (gun == G_DUAL) dualSide = dualSide > 0 ? -1 : 1;
  for (int p = 0; p < g.pellets; p++) {
    float aoff = g.pellets > 1 ? (p - (g.pellets - 1) / 2.0f) / ((g.pellets - 1) / 2.0f) * g.spread
                               : (g.spread > 0 ? (rndf() * 2 - 1) * g.spread : 0);
    int n = hitscan(aoff, g.window, g.range, hits, g.pierce);
    for (int k = 0; k < n; k++) {
      Enemy &e = enemy[hits[k]];
      float dx = e.x - posX, dy = e.y - posY, d = sqrtf(dx * dx + dy * dy);
      int dmg = g.dmg;
      if (gun == G_SHOTGUN) { float f = 1.15f - d / SHOTGUN_FALLOFF; if (f < 0.15f) f = 0.15f; if (f > 1) f = 1; dmg = (int)(dmg * f); }
      damageEnemy(hits[k], dmg, g.dtype, g.ap, posX, posY, gun == G_SHOTGUN ? 1.2f : gun == G_SNIPER ? 3.5f : 2.0f);
    }
  }
}

// Next owned weapon (the SWITCH button).
static void switchWeapon() {
  for (int k = 1; k <= G_N; k++) {
    uint8_t g = (uint8_t)((gun + k) % G_N);
    if (owned[g]) {
      if (g == gun) break;
      gun = g;
      railCharge = 0; fireCd = 0.15f;
      EWLOGI("DOOM", "switch %s", kGuns[g].name);
      toast(kGuns[g].name, 0.7f);
      haptic(HapticRole::ModeChange);
      return;
    }
  }
}

static void giveGun(uint8_t g, int amount) {
  bool had = owned[g];
  owned[g] = true;
  uint8_t at = kGuns[g].ammo;
  if (at != AM_NONE) { ammo[at] += amount; if (ammo[at] > kAmmoMax[at]) ammo[at] = kAmmoMax[at]; }
  if (!had) { gun = g; railCharge = 0; }                // switch to new toys
  char m[28]; snprintf(m, sizeof m, had ? "+%s AMMO" : "GOT %s", kGuns[g].name); toast(m, 1.2f);
}

static void tryFire() {
  if (fireCd > 0) return;
  const GunDef &g = kGuns[gun];
  if (g.ammo != AM_NONE && ammo[g.ammo] < g.perShot) {
    if (clickCd <= 0) { toast("NO AMMO - SWITCH", 0.8f); haptic(HapticRole::Refused); clickCd = 0.6f; }
    fireCd = 0.2f;
    return;
  }
  if (g.mode == M_RAIL) {                              // charge, then fire (see handlePlay)
    if (railCharge <= 0) { railCharge = 0.001f; haptic(HapticRole::PowerUp); }
    return;
  }
  fireGun();
  fireCd = g.cd;
}

// ---------------------------------------------------------------------------
// Enemies, projectiles, items
// ---------------------------------------------------------------------------
static int nearestCorpse(const Enemy &n, float maxD) {
  int best = -1; float bd = maxD * maxD;
  for (int i = 0; i < enemyN; i++) {
    const Enemy &c = enemy[i];
    if (c.alive || c.dying > 0 || c.revived) continue;
    uint8_t k = kEnemies[c.type].kind;
    if (k == K_BOSS || k == K_BOMBER || k == K_NECRO || k == K_TURRET) continue;
    float dx = c.x - n.x, dy = c.y - n.y, d2 = dx * dx + dy * dy;
    if (d2 < bd) { bd = d2; best = i; }
  }
  return best;
}

static void updateEnemies(float dt) {
  for (int i = 0; i < enemyN; i++) {
    Enemy &e = enemy[i];
    const EnemyDef &d = kEnemies[e.type];
    if (e.dying > 0) e.dying -= dt;
    if (e.boom) { e.boom = false; explode(e.x, e.y, 2.2f, 30 + level, false); if (state != PLAY) return; }
    if (e.kbx != 0 || e.kby != 0) {
      tryMove(e.x, e.y, e.kbx * dt, e.kby * dt);
      float k = expf(-9.0f * dt);
      e.kbx *= k; e.kby *= k;
      if (fabsf(e.kbx) + fabsf(e.kby) < 0.05f) e.kbx = e.kby = 0;
    }
    if (!e.alive) continue;
    if (e.hurt > 0) e.hurt -= dt;
    if (e.atkCd > 0) e.atkCd -= dt;
    if (e.shootCd > 0) e.shootCd -= dt;
    if (e.burn > 0) {                                  // burning: damage over time
      e.burn -= dt;
      static float acc[ENEMY_MAX];
      acc[i] += BURN_DPS * dt;
      if (acc[i] >= 1) { int b = (int)acc[i]; acc[i] -= b; damageEnemy(i, b, DT_FIRE, false, e.x, e.y, 0); if (!e.alive) continue; }
      if (rndi(8) == 0) addFx(e.x, e.y, 0.25f, 0.3f);
    }
    float dx = posX - e.x, dy = posY - e.y;
    float dist = sqrtf(dx * dx + dy * dy);
    float toP = atan2f(dy, dx);
    bool sees = dist <= AGGRO + (d.kind == K_BOSS || d.kind == K_MANC ? 6 : 0) && lineOfSight(e.x, e.y, posX, posY);
    if (!e.awake) {
      e.alert = sees ? e.alert + dt : (e.alert > dt ? e.alert - dt : 0);
      if (e.alert >= d.wake) e.awake = true;
      continue;
    }
    // facing turns toward the player at a limited rate (flank the Shield)
    float turnR = d.kind == K_SHIELD ? 1.6f : 6.0f;
    float df = angDiff(toP, e.face);
    e.face += constrain(df, -turnR * dt, turnR * dt);
    float meleeR = MELEE * (0.6f + 0.4f * d.scale);
    float move = 0, side = 0;
    switch (d.kind) {
      case K_MELEE: case K_SHIELD:
        if (!sees) break;
        if (dist > meleeR) move = 1;
        else if (e.atkCd <= 0) { e.atkCd = d.cd; hurtPlayerFrom(e.x, e.y, e.dmg); }
        break;
      case K_BAT:                                      // erratic, darting
        if (!sees) break;
        e.wob += dt * 7;
        side = sinf(e.wob) * 0.9f;
        if (dist > 0.6f) move = 1;
        else if (e.atkCd <= 0) { e.atkCd = d.cd; hurtPlayerFrom(e.x, e.y, e.dmg); }
        break;
      case K_BOMBER:
        if (!sees) break;
        if (dist > 0.85f) move = 1;
        else { e.alive = false; e.dying = 0.2f; kills++; addItem(e.x, e.y, IT_COIN, 0, d.coins);
               explode(e.x, e.y, 2.2f, e.dmg, false); EWLOGI("DOOM", "bomber blew %d/%d", kills, enemyN); }
        break;
      case K_RANGED:
        if (!sees) break;
        if (dist > 6.0f) move = 1; else if (dist < 3.5f) move = -1;
        if (e.shootCd <= 0) {
          spawnProj(e.x, e.y, toP, 5.0f, e.dmg, PK_FIRE, false);
          e.shootCd = d.cd * (0.8f + 0.4f * rndf());
        }
        break;
      case K_MANC:                                     // heavy: 5-ball spread from range
        if (!sees) break;
        if (dist > 9.0f) move = 1; else if (dist < 5.0f) move = -1;
        if (e.shootCd <= 0) {
          for (int k = -2; k <= 2; k++) spawnProj(e.x, e.y, toP + k * 0.2f, 4.0f, e.dmg, PK_FIRE, false);
          e.shootCd = d.cd;
        }
        break;
      case K_TURRET:                                   // fixed: 3-bolt bursts
        if (!sees) break;
        if (e.shootCd <= 0) {
          spawnProj(e.x, e.y, toP + (rndf() - 0.5f) * 0.08f, 7.0f, e.dmg, PK_BOLT, false);
          e.phase++;
          e.shootCd = e.phase % 3 ? 0.18f : d.cd;
        }
        break;
      case K_NECRO:                                    // hangs back, raises the dead
        if (sees) { if (dist > 7.0f) move = 1; else if (dist < 4.5f) move = -1; }
        if (e.shootCd <= 0) {
          int c = nearestCorpse(e, 7.0f);
          if (c >= 0) {
            Enemy &r = enemy[c];
            r.alive = true; r.revived = true; r.awake = true;
            r.health = r.maxHp / 2; r.dying = 0; r.hurt = 0.3f;
            kills--;
            addFx(r.x, r.y, 0.5f, 0.9f, SP_PLASMA);
            if (sees) toast("NECRO RAISED ONE", 0.8f);
            EWLOGI("DOOM", "necro raised %s", kEnemies[r.type].name);
          }
          e.shootCd = d.cd;
        }
        if (sees && e.atkCd <= 0 && dist < meleeR) { e.atkCd = 1.2f; hurtPlayerFrom(e.x, e.y, e.dmg); }
        break;
      case K_LEAPER:                                   // stalk, crouch, pounce
        if (e.phase == 0) {
          if (!sees) break;
          e.phaseT -= dt;
          if (dist < 5.0f && e.phaseT <= 0) { e.phase = 1; e.phaseT = 0.4f; }
          else if (dist > meleeR) move = 1;
          else if (e.atkCd <= 0) { e.atkCd = 1.0f; hurtPlayerFrom(e.x, e.y, e.dmg / 2); }
        } else if (e.phase == 1) {
          e.phaseT -= dt;
          if (e.phaseT <= 0) {
            e.phase = 2; e.phaseT = 0.45f;
            e.lvx = cosf(toP) * 8.0f; e.lvy = sinf(toP) * 8.0f;
          }
        } else {
          e.phaseT -= dt;
          tryMove(e.x, e.y, e.lvx * dt, e.lvy * dt);
          if (dist < 0.7f) { hurtPlayerFrom(e.x, e.y, e.dmg); e.phaseT = 0; }
          if (e.phaseT <= 0) { e.phase = 0; e.phaseT = d.cd; }
        }
        break;
      case K_BOSS:
        if (!sees) break;
        if (dist > meleeR) move = 1;
        else if (e.atkCd <= 0) { e.atkCd = d.cd; hurtPlayerFrom(e.x, e.y, e.dmg); }
        if (e.shootCd <= 0 && dist > 2.0f) {
          for (int k = -1; k <= 1; k++) spawnProj(e.x, e.y, toP + k * 0.22f, 4.5f, 10 + level / 2, PK_FIRE, false);
          e.shootCd = 2.6f;
        }
        break;
    }
    if ((move != 0 || side != 0) && dist > 0.01f && e.spd > 0) {
      float s = e.spd * dt / dist;
      tryMove(e.x, e.y, (dx * move - dy * side) * s, (dy * move + dx * side) * s);
    }
    if (state != PLAY) return;
  }
  for (int i = 0; i < enemyN; i++) {                  // no stacking
    if (!enemy[i].alive) continue;
    for (int j = i + 1; j < enemyN; j++) {
      if (!enemy[j].alive) continue;
      float dx = enemy[j].x - enemy[i].x, dy = enemy[j].y - enemy[i].y, d2 = dx * dx + dy * dy;
      float r = 0.25f * (kEnemies[enemy[i].type].scale + kEnemies[enemy[j].type].scale);
      if (d2 > 1e-6f && d2 < r * r) {
        float d = sqrtf(d2), p = (r - d) * 0.5f;
        if (kEnemies[enemy[i].type].kind != K_TURRET) tryMove(enemy[i].x, enemy[i].y, -dx / d * p, -dy / d * p);
        if (kEnemies[enemy[j].type].kind != K_TURRET) tryMove(enemy[j].x, enemy[j].y, dx / d * p, dy / d * p);
      }
    }
  }
}

static void updateProjectiles(float dt) {
  for (int i = 0; i < PROJ_MAX; i++) {
    Proj &p = proj[i];
    if (!p.live) continue;
    p.ttl -= dt;
    if (p.kind == PK_GREN && p.ttl <= 0) { p.live = false; explode(p.x, p.y, GREN_RADIUS, p.dmg, true); if (state != PLAY) return; continue; }
    float sx = p.vx * dt, sy = p.vy * dt;
    int steps = (int)(sqrtf(sx * sx + sy * sy) / 0.15f) + 1;
    for (int s = 0; s < steps && p.live; s++) {
      float ox = p.x, oy = p.y;
      p.x += sx / steps; p.y += sy / steps;
      if (cell((int)p.x, (int)p.y) || p.ttl <= 0) {
        if (p.kind == PK_GREN) {                       // bounce: reflect the axis that hit
          if (cell((int)p.x, (int)oy)) { p.vx = -p.vx * 0.6f; sx = -sx * 0.6f; }
          if (cell((int)ox, (int)p.y)) { p.vy = -p.vy * 0.6f; sy = -sy * 0.6f; }
          p.x = ox; p.y = oy;
          continue;
        }
        p.live = false;
        if (p.kind == PK_ROCKET) explode(ox, oy, RPG_RADIUS, p.dmg, p.mine);
        else addFx(ox, oy, 0.2f, 0.35f, p.kind == PK_PLASMA ? SP_PLASMA : SP_FIREBALL);
        break;
      }
      if (p.mine) {
        for (int k = 0; k < enemyN; k++) {
          if (!enemy[k].alive) continue;
          float dx = enemy[k].x - p.x, dy = enemy[k].y - p.y;
          float r = 0.35f * kEnemies[enemy[k].type].scale + 0.1f;
          if (dx * dx + dy * dy < r * r) {
            p.live = false;
            if (p.kind == PK_ROCKET) explode(p.x, p.y, RPG_RADIUS, p.dmg, true);
            else if (p.kind == PK_GREN) explode(p.x, p.y, GREN_RADIUS, p.dmg, true);
            else { damageEnemy(k, p.dmg, DT_ENER, false, p.x - p.vx * 0.05f, p.y - p.vy * 0.05f, 1.0f); addFx(p.x, p.y, 0.15f, 0.3f, SP_PLASMA); }
            break;
          }
        }
      } else {
        float dx = posX - p.x, dy = posY - p.y;
        if (dx * dx + dy * dy < 0.35f * 0.35f) {
          p.live = false; addFx(p.x, p.y, 0.2f, 0.4f);
          hurtPlayerFrom(p.x - p.vx * 0.1f, p.y - p.vy * 0.1f, p.dmg);
        }
      }
    }
    if (p.kind == PK_GREN) { p.vx *= expf(-1.2f * dt); p.vy *= expf(-1.2f * dt); }
    if (state != PLAY) return;
  }
  for (int i = 0; i < FX_MAX; i++) if (fx[i].live && (fx[i].t += dt) >= fx[i].dur) fx[i].live = false;
}

static void updateItems() {
  for (int i = 0; i < itemN; i++) {
    Item &it = item[i];
    if (!it.live) continue;
    float dx = it.x - posX, dy = it.y - posY, d2 = dx * dx + dy * dy;
    if (d2 > 0.55f * 0.55f) continue;
    switch (it.kind) {
      case IT_MED:
        if (health >= 100) continue;
        health = health + it.amount > 100 ? 100 : health + it.amount;
        pickCol = RGB565(60, 220, 80);
        break;
      case IT_AMMO:                                   // only for the gun in hand
        if (kGuns[gun].ammo != it.sub || ammo[it.sub] >= kAmmoMax[it.sub]) continue;
        ammo[it.sub] = ammo[it.sub] + it.amount > kAmmoMax[it.sub] ? kAmmoMax[it.sub] : ammo[it.sub] + it.amount;
        pickCol = RGB565(240, 200, 50);
        break;
      case IT_GUN:
        giveGun(it.sub, it.amount);
        pickCol = RGB565(120, 180, 255);
        break;
      case IT_COIN:
        coins += it.amount; levelCoins += it.amount;
        pickCol = RGB565(255, 210, 50);
        break;
    }
    it.live = false;
    pickFlash = it.kind == IT_COIN ? 0.12f : 0.25f;
    haptic(it.kind == IT_COIN ? HapticRole::Collect : HapticRole::PowerUp);
    if (it.kind != IT_COIN)
      EWLOGI("DOOM", "pickup %s hp=%d gun=%s ammo=%d", it.kind == IT_MED ? "health" : it.kind == IT_GUN ? kGuns[it.sub].name : kAmmoName[it.sub],
             health, kGuns[gun].name, kGuns[gun].ammo == AM_NONE ? 0 : ammo[kGuns[gun].ammo]);
  }
}

// ---------------------------------------------------------------------------
// 3D rendering
// ---------------------------------------------------------------------------
static void renderWalls() {
  float light = muzzle > 0 || flameT > 0 ? 1.45f : 1.0f;
  if (boomFlash > 0) light += boomFlash * 3;
  if (railBeamT > 0) light += 0.3f;
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
    for (int guard = 0; guard < 96 && !hit; guard++) {
      if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
      else               { sideY += ddy; mapY += stepY; side = 1; }
      if (cell(mapX, mapY)) hit = 1;
    }
    float perp = (side == 0) ? (sideX - ddx) : (sideY - ddy);
    if (perp < 0.05f) perp = 0.05f;
    zbuf[x] = perp;
    int lineH = (int)(VIEW_H / perp);
    int drawStart = hz - lineH / 2, drawEnd = hz + lineH / 2;
    int cs = drawStart < 0 ? 0 : drawStart;
    int ce = drawEnd > VIEW_H ? VIEW_H : drawEnd;
    int t = cell(mapX, mapY) - 1; if (t < 0) t = 0; if (t > 3) t = 3;
    float wallX = (side == 0) ? (posY + perp * rdy) : (posX + perp * rdx);
    wallX -= floorf(wallX);
    int texX = (int)(wallX * TEX);
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) texX = TEX - texX - 1;
    if (texX < 0) texX = 0; if (texX >= TEX) texX = TEX - 1;
    float sh = 1.0f - perp * 0.045f;
    if (sh < 0.22f) sh = 0.22f;
    sh *= light;
    if (sh > 1.0f) sh = 1.0f;
    if (side == 1) sh *= 0.68f;
    uint16_t sf = (uint16_t)(sh * 256);
    float texStep = (float)TEX / lineH;
    float texPos = (cs - hz + lineH / 2) * texStep;
    uint16_t *col = &fb[pix(x, 0)];
    const int st = pix(x, 1) - pix(x, 0);
    const uint16_t *tex = &wallTex[t][texX];
    for (int y = 0; y < cs; y++)          col[y * st] = ceilGrad[y];
    for (int y = cs; y < ce; y++) {
      int ty = (int)texPos & (TEX - 1);
      texPos += texStep;
      col[y * st] = shade(tex[ty * TEX], sf);
    }
    for (int y = ce; y < VIEW_H; y++)     col[y * st] = floorGrad[y];
  }
}

static void drawSprite(float wx, float wy, const uint16_t *tex, float scale, float squash,
                       float bottom, uint16_t tint, uint16_t tintF, uint16_t alpha) {
  float tx, ty; worldToCamera(wx, wy, tx, ty);
  if (ty <= 0.25f) return;
  int screenX = (int)((SCREEN_W / 2) * (1.0f + tx / ty));
  float full = VIEW_H / ty;
  int w = (int)(full * scale), h = (int)(full * scale * squash);
  if (w < 1 || h < 1) return;
  int endY = hz + (int)(full * bottom);
  int startY = endY - h;
  int startX = screenX - w / 2, endX = screenX + w / 2;
  float fog = 1.0f - ty * 0.045f;
  if (fog < 0.30f) fog = 0.30f;
  if (muzzle > 0 || boomFlash > 0 || flameT > 0) fog *= 1.3f;
  if (fog > 1.0f) fog = 1.0f;
  uint16_t ff = (uint16_t)(fog * 256);
  for (int x = startX < 0 ? 0 : startX; x < endX && x < SCREEN_W; x++) {
    if (ty >= zbuf[x]) continue;
    int texX = (x - startX) * TEX / w;
    if (texX < 0 || texX >= TEX) continue;
    uint16_t *col = &fb[pix(x, 0)];
    const int st = pix(x, 1) - pix(x, 0);
    for (int y = startY < 0 ? 0 : startY; y < endY && y < VIEW_H; y++) {
      int texY = (y - startY) * TEX / h;
      if (texY < 0 || texY >= TEX) continue;
      uint16_t c = tex[texY * TEX + texX];
      if (c == SPR_KEY) continue;
      c = shade(c, ff);
      if (tintF) c = blend(c, tint, tintF);
      col[y * st] = alpha ? blend(col[y * st], c, alpha) : c;
    }
  }
}

static void renderSprites() {
  if (!sprBlock) return;
  const int MAXS = ENEMY_MAX + ITEM_MAX + PROJ_MAX + FX_MAX;
  static int idx[MAXS]; static float depth[MAXS]; int n = 0;
  auto add = [&](float x, float y, int id) {
    float dx = x - posX, dy = y - posY; depth[n] = dx * dx + dy * dy; idx[n++] = id;
  };
  for (int i = 0; i < enemyN; i++) if (enemy[i].alive || enemy[i].dying > 0) add(enemy[i].x, enemy[i].y, i);
  for (int i = 0; i < itemN; i++)  if (item[i].live) add(item[i].x, item[i].y, 100 + i);
  for (int i = 0; i < PROJ_MAX; i++) if (proj[i].live) add(proj[i].x, proj[i].y, 200 + i);
  for (int i = 0; i < FX_MAX; i++) if (fx[i].live) add(fx[i].x, fx[i].y, 300 + i);
  for (int i = 1; i < n; i++) {
    float dd = depth[i]; int id = idx[i], j = i - 1;
    while (j >= 0 && depth[j] < dd) { depth[j + 1] = depth[j]; idx[j + 1] = idx[j]; j--; }
    depth[j + 1] = dd; idx[j + 1] = id;
  }
  uint32_t now = millis();
  for (int k = 0; k < n; k++) {
    int id = idx[k];
    if (id >= 300) {
      const Fx &f = fx[id - 300];
      float p = f.t / f.dur;
      float s = f.size * (0.5f + p);
      drawSprite(f.x, f.y, spr(f.spr), s, 1.0f, s * 0.5f + 0.1f, 0, 0, (uint16_t)(256 * (1 - p) + 1));
    } else if (id >= 200) {
      const Proj &p = proj[id - 200];
      int sp = p.kind == PK_ROCKET ? SP_ROCKET : p.kind == PK_PLASMA ? SP_PLASMA : p.kind == PK_GREN ? SP_GRENADE : SP_FIREBALL;
      float s = p.kind == PK_BOLT ? 0.14f : p.kind == PK_GREN ? 0.2f : p.kind == PK_ROCKET ? 0.22f : 0.3f;
      float bottom = p.kind == PK_GREN ? 0.5f : s * 0.5f + 0.05f;
      drawSprite(p.x, p.y, spr(sp), s, 1.0f, bottom, 0, 0, 0);
    } else if (id >= 100) {
      const Item &it = item[id - 100];
      float b = 0.5f + 0.03f * sinf(now * 0.004f + it.x);
      int sp = it.kind == IT_MED ? SP_MEDKIT : it.kind == IT_AMMO ? kAmmoSpr[it.sub] : it.kind == IT_COIN ? SP_COIN : kGuns[it.sub].spr;
      float sc = it.kind == IT_GUN ? 0.5f : it.kind == IT_COIN ? 0.3f : 0.42f;
      uint16_t dim = (it.kind == IT_AMMO && kGuns[gun].ammo != it.sub) ? 140 : 0;
      drawSprite(it.x, it.y, spr(sp), sc, 1.0f, b, RGB565(20, 20, 30), dim, 0);
    } else {
      const Enemy &e = enemy[id];
      const EnemyDef &d = kEnemies[e.type];
      int sp = d.spr;
      if (d.kind == K_SHIELD) {                        // show the shield only from the front
        float toMe = atan2f(posY - e.y, posX - e.x);
        sp = fabsf(angDiff(toMe, e.face)) < SHIELD_ARC ? SP_SHIELD_F : SP_SHIELD_B;
      }
      float drop = 0.5f + d.scale / 6;
      if (d.kind == K_BAT) drop = 0.5f - 0.45f + 0.04f * sinf(now * 0.01f + id);   // flying
      if (d.kind == K_LEAPER && e.phase == 2) drop -= 0.25f;                        // airborne
      float squash = d.kind == K_LEAPER && e.phase == 1 ? 0.75f : 1.0f;             // crouch
      uint16_t alpha = d.alpha ? (uint16_t)(d.alpha + 60 * sinf(now * 0.012f + id)) : 0;
      uint16_t tint = RGB565(255, 230, 220), tf = e.hurt > 0 ? 220 : 0;
      if (!tf && e.burn > 0) { tint = RGB565(255, 120, 20); tf = 90; }
      if (!tf && e.revived) { tint = RGB565(120, 255, 150); tf = 50; }
      if (e.alive) {
        drawSprite(e.x, e.y, spr(sp), d.scale, squash, drop, tint, tf, alpha);
      } else {
        float k2 = e.dying / 0.45f; if (k2 < 0) k2 = 0;
        drawSprite(e.x, e.y, spr(sp), d.scale, 0.15f + 0.85f * k2, drop, RGB565(150, 0, 0), (uint16_t)(200 * (1 - k2)), alpha);
      }
    }
  }
}

static void renderCrosshair() {
  uint16_t c = hitMarkT > 0 ? hitMarkCol : gun == G_SNIPER || gun == G_RAIL ? RGB565(255, 80, 80) : RGB565(230, 230, 230);
  int arm = gun == G_SHOTGUN || gun == G_FLAMER ? 7 : 4;
  if (hitMarkT > 0) arm += 2;
  for (int i = -arm; i <= arm; i++) {
    if (i >= -1 && i <= 1 && gun != G_SNIPER && gun != G_RAIL) continue;
    fb[pix(SCREEN_W / 2 + i, hz)] = c;
    fb[pix(SCREEN_W / 2, hz + i)] = c;
  }
}

static void renderGun() {
  int gx = SCREEN_W / 2 + (int)swayX + (int)(sinf(bobPhase * 0.5f) * 2 * fabsf(bob) / 3);
  int ry = (int)(gunRecoil * 14) + (int)bob;
  int base = SCREEN_H - 64 + ry;
  const uint16_t DK = RGB565(34, 34, 40), MD = RGB565(58, 58, 66), HL = RGB565(110, 110, 122);
  const uint16_t WOOD = RGB565(110, 70, 38), C_OLIVE = RGB565(85, 100, 50);
  int flashX = gx, flashY = base - 10, flashR = 13;
  switch (gun) {
    case G_PISTOL:
      cv->fillRect(gx - 20, base + 26, 40, 42, DK);
      cv->fillRect(gx - 8, base + 4, 16, 50, MD); cv->fillRect(gx - 8, base + 4, 4, 50, HL);
      cv->fillRect(gx - 17, base + 36, 9, 26, WOOD);
      flashY = base; flashR = 10;
      break;
    case G_DUAL:
      for (int s = -1; s <= 1; s += 2) {
        int x = gx + s * 46, yy = base + (s == (int)dualSide ? 0 : 8);
        cv->fillRect(x - 14, yy + 30, 28, 40, DK);
        cv->fillRect(x - 6, yy + 10, 12, 42, MD); cv->fillRect(x - 6, yy + 10, 3, 42, HL);
      }
      flashX = gx + (int)dualSide * 46; flashY = base + 6; flashR = 9;
      break;
    case G_SMG:
      cv->fillRect(gx - 24, base + 16, 48, 52, DK);
      cv->fillRect(gx - 6, base - 14, 12, 40, MD); cv->fillRect(gx - 6, base - 14, 3, 40, HL);
      cv->fillRect(gx + 12, base + 30, 10, 34, MD);
      cv->fillRect(gx - 22, base + 34, 9, 26, WOOD);
      flashY = base - 16; flashR = 9;
      break;
    case G_SHOTGUN:
      cv->fillRect(gx - 30, base + 22, 60, 46, WOOD);
      cv->fillRect(gx - 15, base - 10, 13, 70, MD); cv->fillRect(gx + 2, base - 10, 13, 70, MD);
      cv->fillRect(gx - 15, base - 10, 3, 70, HL); cv->fillRect(gx + 2, base - 10, 3, 70, HL);
      cv->fillCircle(gx - 9, base - 10, 4, BLACK565); cv->fillCircle(gx + 8, base - 10, 4, BLACK565);
      cv->fillRect(gx - 22, base + 10, 44, 10, WOOD);
      flashY = base - 14; flashR = 18;
      break;
    case G_SNIPER:
      cv->fillRect(gx - 22, base + 30, 44, 38, RGB565(50, 60, 45));
      cv->fillRect(gx - 4, base - 30, 8, 80, MD); cv->fillRect(gx - 4, base - 30, 2, 80, HL);
      cv->fillRect(gx - 9, base + 4, 18, 26, RGB565(25, 25, 30));
      cv->fillCircle(gx, base + 4, 8, RGB565(25, 25, 30)); cv->fillCircle(gx, base + 4, 4, RGB565(90, 140, 200));
      flashY = base - 32; flashR = 10;
      break;
    case G_RPG:
      cv->fillRect(gx - 18, base - 12, 36, 80, C_OLIVE);
      cv->fillRect(gx - 18, base - 12, 6, 80, RGB565(120, 140, 70));
      cv->fillRect(gx - 14, base - 16, 28, 6, RGB565(40, 40, 40));
      if (ammo[AM_ROCKET] > 0 && fireCd <= 0.3f) cv->fillTriangle(gx - 10, base - 16, gx + 10, base - 16, gx, base - 34, RGB565(200, 50, 40));
      cv->fillRect(gx + 18, base + 30, 10, 24, MD);
      flashY = base - 18; flashR = 20;
      break;
    case G_SAW: {
      int j = sawT > 0 ? (int)(esp_random() % 5) - 2 : 0;
      cv->fillRect(gx - 26 + j, base + 24, 52, 44, RGB565(220, 110, 30));
      cv->fillRect(gx - 10 + j, base - 26, 20, 54, RGB565(170, 172, 182));
      for (int y = base - 26; y < base + 28; y += 6) {
        cv->fillRect(gx - 13 + j, y + (sawT > 0 ? (millis() / 30) % 6 : 0), 3, 3, DK);
        cv->fillRect(gx + 10 + j, y + (sawT > 0 ? (millis() / 30) % 6 : 0), 3, 3, DK);
      }
      flashR = 0;
      break;
    }
    case G_FLAMER:
      cv->fillRect(gx - 22, base + 20, 44, 48, RGB565(170, 40, 30));
      cv->fillRect(gx - 6, base - 12, 12, 40, MD); cv->fillRect(gx - 6, base - 12, 3, 40, HL);
      cv->fillCircle(gx, base - 14, 4, RGB565(80, 160, 255));                 // pilot light
      flashR = 0;
      if (flameT > 0) for (int k = 0; k < 14; k++) {                         // the stream
        int fy2 = base - 16 - (int)(esp_random() % 110), spread = (base - fy2) / 3;
        int fx2 = gx + (int)(esp_random() % (2 * spread + 1)) - spread;
        uint16_t c = k % 3 == 0 ? RGB565(255, 240, 160) : k % 3 == 1 ? RGB565(255, 150, 40) : RGB565(220, 60, 20);
        cv->fillCircle(fx2, fy2, 3 + (base - fy2) / 14, c);
      }
      break;
    case G_PLASMA:
      cv->fillRect(gx - 24, base + 18, 48, 50, RGB565(60, 80, 110));
      cv->fillRect(gx - 8, base - 14, 16, 46, RGB565(70, 90, 120));
      cv->fillRect(gx - 4, base - 10, 8, 36, RGB565(90, 200, 255));
      flashY = base - 16; flashR = 10;
      break;
    case G_RAIL: {
      cv->fillRect(gx - 22, base + 26, 44, 42, RGB565(40, 50, 60));
      cv->fillRect(gx - 10, base - 30, 6, 80, HL); cv->fillRect(gx + 4, base - 30, 6, 80, HL);
      uint16_t coil = railCharge > 0 ? RGB565(150, 255, 240) : RGB565(40, 180, 160);
      for (int y = base - 26; y < base + 24; y += 8) cv->fillRect(gx - 4, y, 8, 4, coil);
      if (railCharge > 0) cv->fillCircle(gx, base - 32, 3 + (int)(10 * railCharge / RAIL_CHARGE), RGB565(150, 255, 240));
      flashY = base - 32; flashR = 12;
      break;
    }
    case G_GREN:
      cv->fillRect(gx - 26, base + 22, 52, 46, WOOD);
      cv->fillRect(gx - 14, base - 6, 28, 50, C_OLIVE);
      cv->fillCircle(gx, base - 6, 12, RGB565(40, 45, 30));
      flashY = base - 8; flashR = 14;
      break;
  }
  if (railBeamT > 0)                                    // the rail: muzzle to crosshair
    for (int k = -2; k <= 2; k++) cv->drawLine(gx + k, base - 32, SCREEN_W / 2 + k / 2, hz, k == 0 ? RGB565(255, 255, 255) : RGB565(80, 255, 230));
  if (muzzle > 0 && flashR) {
    cv->fillCircle(flashX, flashY, flashR, gun == G_PLASMA ? RGB565(120, 220, 255) : RGB565(255, 220, 90));
    cv->fillCircle(flashX, flashY, flashR / 2, RGB565(255, 255, 235));
  }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
static void hudBar(int y, int val, int max, uint16_t c, const char *label) {
  const int x = 46, w = 74, h = 7;
  int f = val <= 0 ? 0 : (val >= max ? w : w * val / max);
  cv->fillRect(4, y - 1, x + w, h + 2, RGB565(12, 10, 10));
  cvText(7, y, 1, RGB565(220, 220, 220), "%s", label);
  cv->fillRect(x, y, w, h, RGB565(60, 24, 20));
  if (f) cv->fillRect(x, y, f, h, c);
}

static void overlay(uint16_t c, uint16_t f) {
  for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = blend(fb[i], c, f);
}

static void renderDamageEdges() {
  for (int s = 0; s < 2; s++) {
    float a = s ? dmgRight : dmgLeft;
    if (a <= 0) continue;
    for (int i = 0; i < 18; i++) {
      uint16_t f = (uint16_t)(a * 200 * (18 - i) / 18);
      int x = s ? SCREEN_W - 1 - i : i;
      uint16_t *col = &fb[pix(x, 0)];
      const int st = pix(x, 1) - pix(x, 0);
      for (int y = 0; y < VIEW_H; y++) col[y * st] = blend(col[y * st], RGB565(220, 0, 0), f);
    }
  }
}

static int ownedCount() { int n = 0; for (int g = 0; g < G_N; g++) n += owned[g]; return n; }

static void renderHUD() {
  // look indicator (top right) + level
  int cx = SCREEN_W - 42;
  cv->drawFastHLine(cx - 30, 9, 61, RGB565(90, 90, 90));
  cv->drawFastVLine(cx, 5, 9, RGB565(160, 160, 160));
  float tv = turnVel > 1 ? 1 : (turnVel < -1 ? -1 : turnVel);
  cv->fillCircle(cx + (int)(tv * 30), 9, 3, spin > 0.5f ? RGB565(255, 120, 60) : RGB565(255, 210, 90));
  cvText(SCREEN_W - 50, 18, 1, RGB565(200, 200, 200), "L%d", level);
  cvText(SCREEN_W - 30, 18, 1, RGB565(255, 210, 50), "$%d", coins);
  // health, ammo for the gun in hand
  uint16_t hpc = health > 40 ? RGB565(80, 220, 70) : RGB565(255, 60, 40);
  hudBar(6, health, 100, hpc, "HP");
  uint8_t at = kGuns[gun].ammo;
  if (at != AM_NONE) hudBar(18, ammo[at], kAmmoMax[at], RGB565(240, 200, 50), kAmmoName[at]);
  else { cv->fillRect(4, 17, 120, 9, RGB565(12, 10, 10)); cvText(7, 18, 1, RGB565(220, 220, 220), "NO AMMO NEEDED"); }
  // SWITCH button (top centre): current gun + how many you carry
  bool many = ownedCount() > 1;
  cv->fillRoundRect(SW_X0, 2, SW_X1 - SW_X0, SW_Y1 - 4, 6, RGB565(20, 24, 34));
  cv->drawRoundRect(SW_X0, 2, SW_X1 - SW_X0, SW_Y1 - 4, 6, many ? RGB565(120, 180, 255) : RGB565(70, 80, 100));
  char name[12]; snprintf(name, sizeof name, "%s", kGuns[gun].name);
  int w = (int)strlen(name) * 6;
  cvText((SW_X0 + SW_X1 - w) / 2, 7, 1, RGB565(220, 235, 255), "%s", name);
  char sub[12]; snprintf(sub, sizeof sub, many ? "SWITCH %d" : "%d GUN", ownedCount());
  w = (int)strlen(sub) * 6;
  cvText((SW_X0 + SW_X1 - w) / 2, 18, 1, many ? RGB565(120, 180, 255) : RGB565(110, 120, 140), "%s", sub);
  // boss bar
  for (int i = 0; i < enemyN; i++)
    if (enemy[i].alive && (kEnemies[enemy[i].type].kind == K_BOSS || kEnemies[enemy[i].type].kind == K_MANC)) {
      int bw = 120, f = bw * enemy[i].health / enemy[i].maxHp;
      cv->fillRect(SCREEN_W / 2 - bw / 2 - 1, 36, bw + 2, 7, RGB565(12, 10, 10));
      cv->fillRect(SCREEN_W / 2 - bw / 2, 37, f, 5, RGB565(220, 30, 30));
      break;
    }
  if (clearT > 0) {                                  // ROUND CLEARED + countdown
    char m[24]; snprintf(m, sizeof m, "ROUND CLEARED  %d", (int)ceilf(clearT));
    int w = (int)strlen(m) * 12 + 12;
    cv->fillRoundRect((SCREEN_W - w) / 2, 38, w, 22, 6, RGB565(20, 60, 25));
    cv->drawRoundRect((SCREEN_W - w) / 2, 38, w, 22, 6, RGB565(120, 255, 120));
    cvTextC(42, 2, RGB565(180, 255, 180), m);
    int bw = w - 12, f = (int)(bw * clearT / CLEAR_S);
    cv->fillRect((SCREEN_W - bw) / 2, 62, f, 3, RGB565(120, 255, 120));
  }
  if (toastT > 0) cvTextC(clearT > 0 ? 72 : 52, 2, RGB565(255, 230, 120), toastMsg);
}

static void renderTitle() {
  for (int y = 0; y < SCREEN_H; y++) {
    uint16_t row = shade(RGB565(70, 8, 6), (uint16_t)(40 + 120 * y / SCREEN_H));
    cv->fillRect(0, y, SCREEN_W, 1, row);
  }
  cvText(58, 8, 7, RGB565(220, 30, 20), "DOOM");
  cvText(56, 10, 7, RGB565(255, 70, 40), "DOOM");
  cvText(98, 68, 1, RGB565(255, 200, 60), "EWatch Edition");
  cvText(16, 84, 1, RGB565(220, 220, 220), "TILT L / R     - look");
  cvText(16, 97, 1, RGB565(220, 220, 220), "LEFT STICK     - walk / strafe");
  cvText(16, 110, 1, RGB565(220, 220, 220), "RIGHT SIDE     - fire (hold = auto)");
  cvText(16, 123, 1, RGB565(220, 220, 220), "SIDE BUTTON    - fire");
  cvText(16, 136, 1, RGB565(220, 220, 220), "TOP BUTTON     - switch weapon");
  cvText(16, 149, 1, RGB565(220, 220, 220), "PRESS TWICE    - re-level tilt");
  cvText(16, 162, 1, RGB565(220, 220, 220), "HOLD BUTTON    - exit");
  cvText(16, 176, 1, RGB565(160, 200, 160), "green hit = weak spot, grey = resisted");
  if (bestLevel > 0) { char m[24]; snprintf(m, sizeof m, "BEST: LEVEL %d", bestLevel); cvTextC(190, 1, RGB565(255, 200, 60), m); }
  cvTextC(210, 2, RGB565(255, 230, 120), "TAP TO START");
}

static void renderEnd(bool won) {
  overlay(won ? RGB565(0, 90, 0) : RGB565(120, 0, 0), 150);
  char m[32];
  if (won) {
    snprintf(m, sizeof m, "LEVEL %d", level);
    cvTextC(40, 4, RGB565(120, 255, 120), m);
    cvTextC(82, 4, RGB565(120, 255, 120), "CLEARED");
    snprintf(m, sizeof m, "+$%d   TOTAL $%d", levelCoins, coins);
    cvTextC(128, 2, RGB565(255, 210, 50), m);
    cvTextC(178, 2, RGB565(240, 230, 120), "TAP FOR THE SHOP");
  } else {
    cvTextC(50, 5, RGB565(255, 60, 50), "YOU DIED");
    snprintf(m, sizeof m, "REACHED LEVEL %d  BEST %d", level, bestLevel);
    cvTextC(110, 1, RGB565(255, 220, 200), m);
    cvTextC(160, 2, RGB565(240, 230, 120), "TAP OR PRESS");
    cvTextC(184, 2, RGB565(240, 230, 120), "TO RESTART");
  }
}

// ---------------------------------------------------------------------------
// Shop (between levels): tap a tile to buy; NEXT LEVEL (or the side button)
// ---------------------------------------------------------------------------
enum ShopKind { SH_HEAL, SH_HEALFULL, SH_AMMO, SH_GUN };
struct ShopItem { uint8_t kind, sub; int price; };
#define SHOP_MAX 24
static ShopItem shop[SHOP_MAX]; static int shopN, shopPage;
static const int SHOP_PER = 8;

static void buildShop() {
  shopN = 0; shopPage = 0;
  shop[shopN++] = {SH_HEAL, 0, 8};
  shop[shopN++] = {SH_HEALFULL, 0, 25};
  for (int a = 0; a < AM_N; a++) {                     // ammo for guns you carry
    bool need = false;
    for (int g = 0; g < G_N; g++) need |= owned[g] && kGuns[g].ammo == a;
    if (need) shop[shopN++] = {SH_AMMO, (uint8_t)a, kAmmoPrice[a]};
  }
  for (int g = 1; g < G_N && shopN < SHOP_MAX; g++)    // guns unlocked by the next level
    if (!owned[g] && level + 1 >= kGuns[g].unlock) shop[shopN++] = {SH_GUN, (uint8_t)g, kGuns[g].price};
}

static void shopLabel(const ShopItem &s, char *buf, int n) {
  switch (s.kind) {
    case SH_HEAL: snprintf(buf, n, "HEALTH +25"); break;
    case SH_HEALFULL: snprintf(buf, n, "HEALTH FULL"); break;
    case SH_AMMO: snprintf(buf, n, "%s x%d", kAmmoName[s.sub], kAmmoPack[s.sub]); break;
    default: snprintf(buf, n, "%s", kGuns[s.sub].name); break;
  }
}

static bool shopUseless(const ShopItem &s) {
  if (s.kind == SH_HEAL || s.kind == SH_HEALFULL) return health >= 100;
  if (s.kind == SH_AMMO) return ammo[s.sub] >= kAmmoMax[s.sub];
  return owned[s.sub];
}

static void shopBuy(int i) {
  ShopItem s = shop[i];                                // copy: buying a gun rebuilds shop[]
  if (shopUseless(s)) { toast(s.kind == SH_GUN ? "ALREADY OWNED" : "ALREADY FULL", 0.8f); return; }
  if (coins < s.price) { toast("NOT ENOUGH $", 0.8f); haptic(HapticRole::Refused); return; }
  coins -= s.price;
  switch (s.kind) {
    case SH_HEAL: health = health + 25 > 100 ? 100 : health + 25; break;
    case SH_HEALFULL: health = 100; break;
    case SH_AMMO: ammo[s.sub] = ammo[s.sub] + kAmmoPack[s.sub] > kAmmoMax[s.sub] ? kAmmoMax[s.sub] : ammo[s.sub] + kAmmoPack[s.sub]; break;
    default: owned[s.sub] = true; gun = s.sub;
             if (kGuns[s.sub].ammo != AM_NONE) { uint8_t a = kGuns[s.sub].ammo; ammo[a] += kGuns[s.sub].pickupAmmo; if (ammo[a] > kAmmoMax[a]) ammo[a] = kAmmoMax[a]; }
             {                                           // offer its ammo too (tiles stay put)
               bool listed = kGuns[s.sub].ammo == AM_NONE;
               for (int k = 0; k < shopN; k++) listed |= shop[k].kind == SH_AMMO && shop[k].sub == kGuns[s.sub].ammo;
               if (!listed && shopN < SHOP_MAX) shop[shopN++] = {SH_AMMO, kGuns[s.sub].ammo, kAmmoPrice[kGuns[s.sub].ammo]};
             }
             break;
  }
  char m[28], l[20]; shopLabel(s, l, sizeof l); snprintf(m, sizeof m, "BOUGHT %s", l); toast(m, 0.9f);
  haptic(HapticRole::PowerUp);
  EWLOGI("DOOM", "shop bought %s $%d left", l, coins);
}

static void tileRect(int k, int &x, int &y, int &w, int &h) {
  x = 8 + (k % 2) * 134; y = 34 + (k / 2) * 40; w = 128; h = 34;
}

static void renderShop() {
  cv->fillScreen(RGB565(16, 14, 22));
  cvText(8, 8, 2, RGB565(255, 210, 50), "SHOP");
  char m[32]; snprintf(m, sizeof m, "$%d", coins);
  cvText(SCREEN_W - 8 - (int)strlen(m) * 12, 8, 2, RGB565(255, 210, 50), "%s", m);
  snprintf(m, sizeof m, "HP %d  NEXT: LEVEL %d", health, level + 1);
  cvText(70, 12, 1, RGB565(180, 180, 200), "%s", m);
  for (int k = 0; k < SHOP_PER; k++) {
    int i = shopPage * SHOP_PER + k;
    if (i >= shopN) break;
    int x, y, w, h; tileRect(k, x, y, w, h);
    const ShopItem &s = shop[i];
    bool useless = shopUseless(s), afford = coins >= s.price;
    uint16_t edge = useless ? RGB565(70, 70, 80) : afford ? RGB565(80, 220, 100) : RGB565(150, 60, 60);
    cv->fillRoundRect(x, y, w, h, 5, RGB565(30, 28, 40));
    cv->drawRoundRect(x, y, w, h, 5, edge);
    char l[20]; shopLabel(s, l, sizeof l);
    cvText(x + 6, y + 6, 1, useless ? RGB565(120, 120, 130) : RGB565(235, 235, 245), "%s", l);
    if (useless) cvText(x + 6, y + 19, 1, RGB565(120, 120, 130), s.kind == SH_GUN ? "OWNED" : "FULL");
    else {
      snprintf(m, sizeof m, "$%d", s.price);
      cvText(x + 6, y + 19, 1, afford ? RGB565(255, 210, 50) : RGB565(200, 90, 90), "%s", m);
      if (s.kind == SH_AMMO) { snprintf(m, sizeof m, "have %d", ammo[s.sub]); cvText(x + 56, y + 19, 1, RGB565(150, 150, 170), "%s", m); }
    }
  }
  int pages = (shopN + SHOP_PER - 1) / SHOP_PER;
  if (pages > 1) {
    cv->drawRoundRect(8, 200, 40, 32, 5, RGB565(150, 150, 170)); cvText(22, 209, 2, RGB565(220, 220, 230), "<");
    snprintf(m, sizeof m, "%d/%d", shopPage + 1, pages); cvText(56, 210, 1, RGB565(180, 180, 200), "%s", m);
    cv->drawRoundRect(88, 200, 40, 32, 5, RGB565(150, 150, 170)); cvText(102, 209, 2, RGB565(220, 220, 230), ">");
  }
  cv->fillRoundRect(150, 200, 122, 32, 6, RGB565(40, 110, 50));
  cvText(160, 209, 2, RGB565(240, 255, 240), "NEXT >");
  if (toastT > 0) cvTextC(190, 1, RGB565(255, 230, 120), toastMsg);
}

static void shopTap(int fx, int fy) {
  if (fy >= 200) {
    int pages = (shopN + SHOP_PER - 1) / SHOP_PER;
    if (fx >= 150) { loadLevel(level + 1, false); state = PLAY; return; }
    if (fx < 50 && pages > 1) shopPage = (shopPage + pages - 1) % pages;
    else if (fx >= 86 && fx < 130 && pages > 1) shopPage = (shopPage + 1) % pages;
    return;
  }
  for (int k = 0; k < SHOP_PER; k++) {
    int i = shopPage * SHOP_PER + k;
    if (i >= shopN) break;
    int x, y, w, h; tileRect(k, x, y, w, h);
    if (fx >= x && fx < x + w && fy >= y && fy < y + h) { shopBuy(i); return; }
  }
}

// ---------------------------------------------------------------------------
// Per-frame input + update
// ---------------------------------------------------------------------------
static void aimAssist(float dt) {
  if (fabsf(turnVel) > 0.6f) return;
  int best = -1; float bestOff = 0.45f;
  for (int i = 0; i < enemyN; i++) {
    if (!enemy[i].alive) continue;
    float tx, ty; worldToCamera(enemy[i].x, enemy[i].y, tx, ty);
    if (ty <= 0.5f || ty > 16.0f) continue;
    float off = tx / ty;
    if (fabsf(off) < fabsf(bestOff) && lineOfSight(posX, posY, enemy[i].x, enemy[i].y)) {
      bestOff = off; best = i;
    }
  }
  if (best < 0 || fabsf(bestOff) < 0.02f) return;
  ang += constrain(bestOff, -0.45f, 0.45f) * 1.6f * dt;
  setDir();
}

static void readTilt(float dt) {
  int16_t ax = 0, ay = 0, az = 0;
  bool ok = readAccel(ax, ay, az);
  ewsimAccel(ok, ax, ay, az);
  tiltOk = ok; rawAx = ax; rawAy = ay; rawAz = az;
  if (!ok) return;
#if DOOM_ROT == 1
  float lat = -(float)ay / 4096.0f;
#else
  float lat = (float)ay / 4096.0f;
#endif
  lat *= TILT_SIGN;
  if (!tiltZeroed) { tiltLat = lat; tiltZeroed = true; }
  tiltLat += (lat - tiltLat) * (1.0f - expf(-TILT_LP * dt));
  if (millis() - playStartMs < 300) { tiltZero = tiltLat; return; }
  float t = tiltLat - tiltZero;
  if (fabsf(t) < TILT_DEAD * 2.5f) tiltZero += t * (1.0f - expf(-dt / TILT_DRIFT));
}

static void relevel() {
  if (!tiltZeroed) return;
  tiltZero = tiltLat;
  turnVel = spin = 0;
  toast("LEVEL SET", 0.8f);
  haptic(HapticRole::Confirm);
}

static void handlePlay(float dt, bool finger, int fx, int fy, bool fireNow, bool fireHeld) {
  float wantWalk = 0, wantStrafe = 0, wantTurn = 0;
  bool sprint = false;
  knobX = JOY_CX; knobY = JOY_CY;
  if (finger && joyActive) {
    float dx = fx - JOY_CX, dy = fy - JOY_CY;
    float rr = sqrtf(dx * dx + dy * dy), r = rr;
    if (r > JOY_RADIUS) { dx *= JOY_RADIUS / r; dy *= JOY_RADIUS / r; r = JOY_RADIUS; }
    knobX = JOY_CX + (int)dx; knobY = JOY_CY + (int)dy;
    if (r > JOY_DEAD) {
      float k = (r - JOY_DEAD) / (JOY_RADIUS - JOY_DEAD) / r;
      wantStrafe = dx * k;
      wantWalk = -dy * k;
      sprint = rr >= JOY_RADIUS * 0.95f && wantWalk > 0.6f;
    }
  }

  readTilt(dt);
  float u = 0;
  if (tiltOk && tiltZeroed && millis() - playStartMs >= 300) {
    float t = tiltLat - tiltZero, m = fabsf(t);
    if (m > TILT_DEAD) {
      u = (m - TILT_DEAD) / (TILT_FULL - TILT_DEAD);
      if (u > 1) u = 1;
      wantTurn = (t > 0 ? 1 : -1) * (u * u * 0.7f + u * 0.3f);
    }
  }
  spin = u >= 0.9f ? (spin + dt / 0.6f > 1 ? 1 : spin + dt / 0.6f) : (spin - dt * 4 < 0 ? 0 : spin - dt * 4);
  wantTurn *= 1.0f + TURN_BOOST * spin;

  auto chase = [&](float &v, float want) {
    float rate = fabsf(want) > fabsf(v) ? JOY_ACCEL : JOY_DECEL;
    v += (want - v) * (1.0f - expf(-rate * dt));
    if (fabsf(v) < 0.01f) v = 0;
  };
  chase(walkVel, wantWalk * (sprint ? SPRINT : 1.0f));
  chase(strafeVel, wantStrafe);
  turnVel += (wantTurn - turnVel) * (1.0f - expf(-10.0f * dt));
  if (fabsf(turnVel) < 0.01f) turnVel = 0;

  if (turnVel != 0) { ang += turnVel * TURN_SPD * dt; setDir(); }
  float sv = strafeVel * STRAFE_SPD;
  if (walkVel != 0 || sv != 0) {
    float mx = dirX * walkVel - dirY * sv, my = dirY * walkVel + dirX * sv;
    movePlayer(mx * MOVE_SPD * dt, my * MOVE_SPD * dt);
    float sp = sqrtf(walkVel * walkVel + sv * sv);
    bobPhase += dt * (8 + 3 * sp); bob = sinf(bobPhase) * 3 * (sp > 1.2f ? 1.2f : sp);
  } else {
    bob *= 0.8f;
  }
  swayX += (-turnVel * 16 - sv * 6 - swayX) * (1.0f - expf(-8.0f * dt));
  aimAssist(dt);

  updateItems();
  if (fireCd > 0) fireCd -= dt;
  if (clickCd > 0) clickCd -= dt;
  const GunDef &g = kGuns[gun];
  if (fireNow) tryFire();
  else if (fireHeld) {
    // Held: SMG, plasma, flamer, chainsaw keep going; the rest only with a
    // demon in the crosshair, so they don't empty into walls.
    if (g.autoFire || targetUnderCrosshair() >= 0) tryFire();
  }
  if (railCharge > 0) {                                // railgun: charge then shoot
    railCharge += dt;
    if (railCharge >= RAIL_CHARGE) { railCharge = 0; fireGun(); fireCd = g.cd; }
  }

  if (g.ammo == AM_BULLET && ammo[AM_BULLET] == 0) {
    ammoRegen += dt;
    if (ammoRegen > 3.0f) { ammo[AM_BULLET] = 3; ammoRegen = 0; }
  } else {
    ammoRegen = 0;
  }

  updateEnemies(dt);
  if (state == PLAY) updateProjectiles(dt);

  if (gunRecoil > 0) gunRecoil -= dt * 6;  if (gunRecoil < 0) gunRecoil = 0;
  if (muzzle > 0) muzzle -= dt;
  if (flameT > 0) flameT -= dt;
  if (sawT > 0) sawT -= dt;
  if (railBeamT > 0) railBeamT -= dt;
  if (hitMarkT > 0) hitMarkT -= dt;
  if (hurtFlash > 0) hurtFlash -= dt;
  if (boomFlash > 0) boomFlash -= dt;
  dmgLeft = dmgLeft > dt * 1.6f ? dmgLeft - dt * 1.6f : 0;
  dmgRight = dmgRight > dt * 1.6f ? dmgRight - dt * 1.6f : 0;
  if (pickFlash > 0) pickFlash -= dt;

  // Last kill: keep playing for CLEAR_S so the coins can be picked up, then
  // the cleared screen. (A necro raise can't happen: necros are dead too.)
  if (state == PLAY && kills >= enemyN) {
    if (clearT <= 0) {
      clearT = CLEAR_S;
      haptic(HapticRole::LevelUp);
      EWLOGI("DOOM", "round cleared level %d: %ds to collect", level, (int)CLEAR_S);
    } else if ((clearT -= dt) <= 0) {
      clearT = 0;
      state = WIN;
      if (level > bestLevel) bestLevel = level;
      EWLOGI("DOOM", "cleared level %d hp=%d $%d (+%d)", level, health, coins, levelCoins);
    }
  }
}

static void renderControls() {
  uint16_t ring = joyActive ? RGB565(230, 230, 230) : RGB565(150, 150, 150);
  cv->drawCircle(JOY_CX, JOY_CY, (int)JOY_RADIUS, ring);
  cv->drawCircle(JOY_CX, JOY_CY, (int)JOY_RADIUS + 1, BLACK565);
  cv->fillCircle(knobX, knobY, 14, joyActive ? RGB565(255, 210, 90) : RGB565(170, 140, 70));
  cv->drawCircle(knobX, knobY, 15, BLACK565);
  uint16_t fc = muzzle > 0 || flameT > 0 ? RGB565(255, 120, 60) : RGB565(170, 40, 30);
  cv->drawCircle(FIRE_CX, FIRE_CY, FIRE_R, fc);
  cv->drawCircle(FIRE_CX, FIRE_CY, FIRE_R - 1, fc);
  cvText(FIRE_CX - 11, FIRE_CY - 3, 1, RGB565(255, 220, 200), "FIRE");
}

static void renderExitHold() {
  if (exitHold <= 0) return;
  int w = (int)(200 * exitHold);
  cv->fillRect(38, 90, 204, 30, RGB565(20, 20, 20));
  cv->drawRect(38, 90, 204, 30, RGB565(255, 80, 60));
  cv->fillRect(40, 92, w, 26, RGB565(200, 40, 30));
  cvText(98, 97, 2, RGB565(255, 255, 255), "EXIT...");
}

void doomInit() {
  cvOs = frameCanvas();
  cv = cvOs;
  if (!cv) {
    if (gfx) { gfx->fillScreen(0xF800); gfx->setCursor(20, 120);
               gfx->setTextColor(0xFFFF); gfx->print("DOOM: no PSRAM canvas"); }
    return;
  }
  cvOwn = new Arduino_Canvas(PANEL_W, PANEL_H, gfx);
  if (cvOwn && !cvOwn->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete cvOwn; cvOwn = nullptr; }
  if (cvOwn) cvOwn->setRotation(DOOM_ROT);
  if (!sDmaStrip[0]) {
    for (int i = 0; i < 2; i++)
      sDmaStrip[i] = (uint16_t *)heap_caps_malloc(PANEL_W * FLUSH_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  }
  sDmaOk = sDmaStrip[0] && sDmaStrip[1] && doomLcdDmaBegin();
  if (!sFlushQ) {
    sFlushQ = xQueueCreate(1, sizeof(const uint16_t *));
    sFlushIdle = xSemaphoreCreateBinary();
    xSemaphoreGive(sFlushIdle);
    // Core 0, priority 1 (= the console); it yields while DMA strips are on
    // the wire and sleeps a tick every 6 strips (see flushBuffer). Measured:
    // prio 0 shares slices with IDLE: push 28 -> 45 ms (22 fps).
    xTaskCreatePinnedToCore(flushTask, "doomflush", 3072, nullptr, 1, nullptr, 0);
  }
  cvOs->setRotation(DOOM_ROT);
  fb = cv->getFramebuffer();
  pinMode(PIN_BTN, INPUT);
  EWLOGI("DOOM", "buffers=%d dma=%d", cvOwn ? 2 : 1, (int)sDmaOk);

  genWallTextures();
  genSprites();
  genGradients();
  loadLevel(1, true);

  state  = TITLE;
  lastMs = millis();
  lastActivityMs = 0;
  renderTitle();
  present();
}

void doomLeave() {
  waitFlushIdle();
  doomLcdDmaEnd();
  sDmaOk = false;
  for (int i = 0; i < 2; i++) { if (sDmaStrip[i]) heap_caps_free(sDmaStrip[i]); sDmaStrip[i] = nullptr; }
  if (cvOwn) { delete cvOwn; cvOwn = nullptr; }
  if (cvOs) { cvOs->setRotation(0); cvOs->fillScreen(BLACK565); }
  cv = cvOs;
}

void doomFrame() {
  if (!cv) { delay(100); return; }

  uint32_t now = millis();
  float dt = (now - lastMs) / 1000.0f;
  lastMs = now;
  if (dt > 0.10f) dt = 0.10f;

  // ----- input -----
  int fx = 0, fy = 0;
  bool finger = readFinger(fx, fy);
  bool tapEdge = finger && !prevFinger;
  bool switchTap = false;
  if (tapEdge) {
    switchTap = state == PLAY && fy < SW_Y1 && fx >= SW_X0 - 8 && fx < SW_X1 + 8;
    joyActive = !switchTap && fx < SCREEN_W / 2 && fy >= SW_Y1;
  }
  if (!finger) joyActive = false;
  static bool touchIsSwitch = false;
  if (tapEdge) touchIsSwitch = switchTap;
  bool armed = (int32_t)(now - playStartMs) > 150;
  bool tapFire = tapEdge && !joyActive && !switchTap && armed;
  bool holdFire = finger && !joyActive && !touchIsSwitch && !tapEdge && armed;
  prevFinger = finger;

  bool btn = (digitalRead(PIN_BTN) == HIGH) || ewsimButton();
  bool btnEdge = btn && !prevBtn;
  if (btnEdge) btnDownMs = now;
  uint32_t held = btn ? now - btnDownMs : 0;
  exitHold = held > EXIT_SHOW_MS ? (float)(held - EXIT_SHOW_MS) / (EXIT_MS - EXIT_SHOW_MS) : 0;
  if (exitHold > 1) exitHold = 1;
  if (btn && held >= EXIT_MS) { sDoomExit = true; btnDownMs = now; haptic(HapticRole::LongPress); }
  bool doublePress = btnEdge && now - lastPressMs < DOUBLE_MS;
  if (btnEdge) lastPressMs = doublePress ? 0 : now;
  prevBtn = btn;

  // Tell the power manager you're playing (its idle clock doesn't see input
  // while Doom owns the touch panel + button): without this the screen went
  // off the moment you exited, which looked like a crash.
  if ((finger || btn) && now - lastActivityMs > 500) { powerOnActivity(); lastActivityMs = now; }

  if (toastT > 0) toastT -= dt;
  if (shakeT > 0) {
    shakeT -= dt;
    hz = HORIZON + (int)((esp_random() % 1000) / 1000.0f * 2 * shakeAmp - shakeAmp);
  } else {
    hz = HORIZON;
  }

  switch (state) {
    case TITLE:
      renderTitle();
      if (tapEdge || btnEdge) { loadLevel(1, true); state = PLAY; }
      break;

    case PLAY:
      if (doublePress) relevel();
      if (switchTap) switchWeapon();
      handlePlay(dt, finger, fx, fy, btnEdge || tapFire, holdFire);
      renderWalls();
      renderSprites();
      renderCrosshair();
      renderDamageEdges();
      renderGun();
      if (hurtFlash > 0) overlay(RGB565(200, 0, 0), (uint16_t)(hurtFlash * 320));
      if (pickFlash > 0) overlay(pickCol, (uint16_t)(pickFlash * 360));
      if (boomFlash > 0) overlay(RGB565(255, 160, 60), (uint16_t)(boomFlash * 500));
      renderControls();
      renderHUD();
      break;

    case DEAD:
      hz = HORIZON;
      renderWalls(); renderSprites(); renderGun();
      renderEnd(false);
      if (tapEdge || btnEdge) { loadLevel(1, true); state = PLAY; }
      break;

    case WIN:
      hz = HORIZON;
      renderWalls(); renderSprites();
      renderEnd(true);
      if (tapEdge || btnEdge) { buildShop(); state = SHOP; toastT = 0; }
      break;

    case SHOP:
      if (tapEdge) shopTap(fx, fy);
      else if (btnEdge) { loadLevel(level + 1, false); state = PLAY; }
      if (state == SHOP) renderShop();
      break;
  }

  renderExitHold();
  uint32_t tf = micros();
  present();
  static uint32_t statT0, statN, statWaitUs, statFrameUs, prevStart;
  uint32_t tEnd = micros();
  statWaitUs += tEnd - tf;
  if (prevStart) statFrameUs += tEnd - prevStart;
  prevStart = tEnd; statN++;
  if (now - statT0 >= 30000) {     // rare: a log line mid-`snap` corrupts the transfer
    if (statT0 && statN) {
      uint32_t fn = sFlushN, fu = sFlushUsAcc;
      EWLOGI("DOOM", "fps=%.1f frame_ms=%.1f push_ms=%.1f wait_ms=%.1f state=%d lvl=%d heap=%u",
             statN * 1000.0f / (now - statT0), statFrameUs / 1000.0f / statN,
             fn ? fu / 1000.0f / fn : 0.0f, statWaitUs / 1000.0f / statN,
             (int)state, level, (unsigned)ESP.getFreeHeap());
    }
    statT0 = now; statN = statWaitUs = statFrameUs = 0;
    sFlushN = 0; sFlushUsAcc = 0;
  }
}
