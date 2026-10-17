// Hopper — a side-scrolling platformer for EWatchOS (module "hopper").
//
// Plays like the classic handheld platformers: run, jump (hold for higher),
// stomp, kick shells, bump prize blocks, smash bricks when big, ground-pound,
// throw fireballs, collect three hidden gems per level, touch the midway flag,
// slide down the goal reed. Four levels: meadow, cave, treetops, fort + boss.
// All art, names and levels are original (companions/hopper/build_assets.py
// generates hopper_data.h and proves every level and gem is reachable with the
// physics constants below — keep the two in sync).
//
// Controls
//   tilt the watch       run left / right   (or AUTO-RUN mode: always runs right)
//   tap / hold screen    jump / jump higher
//   swipe down mid-air   ground pound
//   side button tap      fireball (with the chili power)
//   side button hold     pause (resume / quit)
//
// Rendering: everything is drawn straight into the shared 240x280 frame canvas
// (RGB565, flash-resident tiles and sprites with a magenta key), then flushed
// once. Physics runs at a fixed 120 Hz step, decoupled from the ~15-20 fps the
// panel flush allows.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "view.h"
#include "model.h"
#include "event.h"
#include "display.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "ewsim.h"
#include "hopper_lcd.h"
#include "hopper_data.h"
#include <esp_heap_caps.h>
#include "font/glcdfont.h"   // GFX classic 5x7 font (static copy), for text drawn into strips

namespace {

// ---------------------------------------------------------------- geometry
constexpr int W = 240, H = 280, HUD = 24, TS = 16, ROWS = 16, MAXW = 256;
constexpr int GT = 14;                       // ground-top row used by the level builder
constexpr float STEP = 1.0f / 120.0f;        // physics step (matches the checker)

// ---------------------------------------------------------------- physics
// Mirrored in build_assets.py (P = ...). Change both together.
constexpr float GRAV_HOLD = 1000.0f, GRAV = 2200.0f, MAX_FALL = 420.0f;
constexpr float JUMP_V = 370.0f, JUMP_RUN = 0.20f, MAXV = 150.0f;
constexpr float ACC_GROUND = 620.0f, ACC_AIR = 440.0f, DEC_GROUND = 560.0f, DEC_AIR = 220.0f;
constexpr float AUTO_RUN_V = 105.0f;
constexpr float STOMP_V = 260.0f, STOMP_V_HELD = 390.0f;
constexpr float COYOTE = 0.08f, JUMP_BUF = 0.14f;
constexpr float POUND_V = 520.0f;
constexpr float E_GRAV = 1400.0f;
constexpr int   PW = 12, PH_SMALL = 14, PH_BIG = 22;

// Tilt: lateral gravity along the panel's x axis (model.ax, 4096 = 1 g).
// Right side down -> ax negative -> run right (same sign as the Racer).
constexpr float TILT_SIGN = -1.0f, TILT_FULL = 1500.0f, TILT_DEAD = 0.12f;

// ---------------------------------------------------------------- colours
constexpr uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF;
constexpr uint16_t C_HUD = RGB565(16, 14, 22);
constexpr uint16_t C_ACCENT = RGB565(0x60, 0xC8, 0x48);   // frog green
constexpr uint16_t C_GOLD = RGB565(252, 214, 64);
constexpr uint16_t C_RED = RGB565(226, 52, 44);

// ---------------------------------------------------------------- tiles
enum Tile : uint8_t {
  T_EMPTY, T_GROUND, T_BRICK, T_QCOIN, T_QPOW, T_Q1UP, T_USED, T_SOLID,
  T_LOG_TL, T_LOG_TR, T_LOG_L, T_LOG_R, T_COIN, T_SEMI, T_LAVA, T_POLE,
  T_POLE_TOP, T_FORT, T_FORT_TOP, T_DOOR, T_DOOR_TOP, T_BUSH, T_SWITCH,
  T_BRICKC, T_GEM, T_CHECK,
};

static inline bool isSolid(uint8_t t) {
  switch (t) {
    case T_GROUND: case T_BRICK: case T_QCOIN: case T_QPOW: case T_Q1UP: case T_USED:
    case T_SOLID: case T_LOG_TL: case T_LOG_TR: case T_LOG_L: case T_LOG_R: case T_BRICKC:
      return true;
    default: return false;
  }
}

// ---------------------------------------------------------------- entities
enum EType : uint8_t {
  E_NONE, E_LADYBUG, E_SNAIL, E_SHELL, E_SPIKY, E_BOSS,
  E_BERRY, E_CHILI, E_ONEUP, E_FIREBALL, E_COINPOP, E_DEBRIS, E_SCORE,
  E_PLAT_H, E_PLAT_V, E_FIREBAR, E_FLAG,
};
enum : uint8_t { S_LIVE = 0, S_SQUASHED = 1, S_KNOCKED = 2, S_EMERGE = 3 };

struct Body {
  float x, y, vx, vy;
  int16_t w, h;
  bool onGround, hitWall;
  int8_t headTx, headTy;    // tile bumped by the head this move (-1 = none)
};

struct Ent {
  EType   type;
  uint8_t state;
  bool    active;
  int8_t  dir;              // -1 left, +1 right
  Body    b;
  float   t;                // age / phase timer
  float   ox, oy;           // origin (platforms, firebars, boss arena)
  float   pdx, pdy;         // platform motion this step (carry)
  int16_t hp, val;
  float   inv;              // boss invulnerability
};

constexpr int MAXE = 72;
static Ent    ents[MAXE];
static uint8_t map_[ROWS][MAXW];
static uint16_t *fb = nullptr;
static int     clipTop = HUD;
static int     clipBot = H;
// The world is drawn one 16-row strip at a time into internal RAM, then copied
// to the PSRAM canvas once: every overdraw (sky, hills, tiles, sprites) hits
// SRAM instead of PSRAM. fb is offset so drawing code keeps screen coordinates.
constexpr int STRIP = 16;
static uint16_t *stripBuf = nullptr;        // the strip being drawn
static uint16_t *stripMem[2] = {nullptr, nullptr};   // DMA-capable pair: draw one, send the other
static bool      lcdDma = false;

// The current theme's tiles, copied to internal RAM on level load (reading
// them from flash every frame thrashes the cache the PSRAM framebuffer shares).
static uint16_t *tileRam = nullptr;          // HOP_ART_COUNT * 256, or null -> flash
static uint8_t   tileOpaque[HOP_ART_COUNT];  // no key pixels: blit by memcpy
static int       tileRamTheme = -1;

static void tilesLoad(int theme) {
  if (!tileRam) tileRam = (uint16_t *)heap_caps_malloc(HOP_ART_COUNT * 256 * 2,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  for (int a = 0; a < HOP_ART_COUNT; a++) {
    const uint16_t *src = HOP_TILEART[theme][a];
    bool op = true;
    for (int i = 0; i < 256; i++) if (src[i] == HOP_KEY) { op = false; break; }
    tileOpaque[a] = op;
    if (tileRam) memcpy(tileRam + a * 256, src, 512);
  }
  tileRamTheme = theme;
  for (auto &m : stripMem)
    if (!m) m = (uint16_t *)heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  stripBuf = stripMem[0];
  if (!lcdDma && stripMem[0] && stripMem[1]) lcdDma = hopLcdDmaBegin();
}
static void tilesFree() {
  if (tileRam) { heap_caps_free(tileRam); tileRam = nullptr; }
  if (lcdDma) { hopLcdDmaEnd(); lcdDma = false; }
  for (auto &m : stripMem) if (m) { heap_caps_free(m); m = nullptr; }
  stripBuf = nullptr;
  tileRamTheme = -1;
}
static inline const uint16_t *tilePx(int theme, int a) {
  return (tileRam && tileRamTheme == theme) ? tileRam + a * 256 : HOP_TILEART[theme][a];
}

struct Bump { int16_t tx, ty; float t; };
static Bump bumps[6];
struct MultiCoin { int16_t tx, ty; uint8_t n; };
static MultiCoin multi[8];

// Console requests (set from the console task, consumed in render()).
static volatile int  reqWarp = -1;
static volatile int  reqWarpRow = -1;   // feet row, -1 = first surface from the top
static volatile int  reqLevel = -1;
static volatile int  reqForm = -1;
static volatile bool reqStatus = false;
static volatile bool godMode = false;

// ---------------------------------------------------------------- drawing
static inline void fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < clipTop) { h -= clipTop - y; y = clipTop; }
  if (x + w > W) w = W - x;
  if (y + h > clipBot) h = clipBot - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) {
    uint16_t *d = fb + yy * W + x;
    for (int i = 0; i < w; i++) d[i] = c;
  }
}

static void blit(const uint16_t *src, int sw, int sh, int dx, int dy,
                 bool fx = false, bool fy = false) {
  int x0 = dx < 0 ? -dx : 0, x1 = dx + sw > W ? W - dx : sw;
  int y0 = dy < clipTop ? clipTop - dy : 0, y1 = dy + sh > clipBot ? clipBot - dy : sh;
  if (x0 >= x1 || y0 >= y1) return;
  for (int y = y0; y < y1; y++) {
    const uint16_t *row = src + (fy ? (sh - 1 - y) : y) * sw;
    uint16_t *d = fb + (dy + y) * W + dx;
    if (!fx) {
      for (int x = x0; x < x1; x++) { uint16_t c = row[x]; if (c != HOP_KEY) d[x] = c; }
    } else {
      for (int x = x0; x < x1; x++) { uint16_t c = row[sw - 1 - x]; if (c != HOP_KEY) d[x] = c; }
    }
  }
}

// Opaque 16x16 tile: straight row copies.
static void blitTile(const uint16_t *src, int dx, int dy, bool opaque) {
  if (!opaque) { blit(src, 16, 16, dx, dy); return; }
  int x0 = dx < 0 ? -dx : 0, x1 = dx + 16 > W ? W - dx : 16;
  int y0 = dy < clipTop ? clipTop - dy : 0, y1 = dy + 16 > clipBot ? clipBot - dy : 16;
  if (x0 >= x1 || y0 >= y1) return;
  for (int y = y0; y < y1; y++)
    memcpy(fb + (dy + y) * W + dx + x0, src + y * 16 + x0, (x1 - x0) * 2);
}

static inline void spr(HopSpr s, int x, int y, bool fx = false, bool fy = false) {
  const HopSprite &sp = HOP_SPR[s];
  blit(sp.px, sp.w, sp.h, x, y, fx, fy);
}

// Halve brightness of a band (pause / card backdrops).
static void dim(int y0, int y1) {
  for (int i = y0 * W; i < y1 * W; i++) fb[i] = (fb[i] >> 1) & 0x7BEF;
}

static inline uint16_t lerp565(uint16_t a, uint16_t b, int t, int n) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (br - ar) * t / n, g = ag + (bg - ag) * t / n, bl = ab + (bb - ab) * t / n;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Text through GFX on the same canvas. Positions are clamped on-screen: GFX
// glyph writes at negative coordinates land before the buffer (KB note
// arduino-gfx-custom-font-top-clip).
static void text(Arduino_GFX *g, const char *s, int x, int y, uint8_t size, uint16_t col,
                 bool shadow = true) {
  int w = (int)strlen(s) * 6 * size;
  if (x < 0) x = 0;
  if (x > W - w) x = W - w;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (y > H - 8 * size) y = H - 8 * size;
  g->setTextSize(size);
  if (shadow) {
    g->setTextColor(C_BLACK);
    g->setCursor(x + size, y + size);
    g->print(s);
  }
  g->setTextColor(col);
  g->setCursor(x, y);
  g->print(s);
}
static void textC(Arduino_GFX *g, const char *s, int y, uint8_t size, uint16_t col,
                  bool shadow = true) {
  text(g, s, (W - (int)strlen(s) * 6 * size) / 2, y, size, col, shadow);
}

// Text drawn straight into fb (clipped like fill), so it works in strips too.
static void fbChars(const char *s, int x, int y, int size, uint16_t col) {
  for (; *s; s++, x += 6 * size) {
    const unsigned char *gl = font + (unsigned char)*s * 5;
    for (int cx = 0; cx < 5; cx++) {
      uint8_t line = gl[cx];
      for (int cy = 0; cy < 8; cy++, line >>= 1)
        if (line & 1) fill(x + cx * size, y + cy * size, size, size, col);
    }
  }
}
static void fbText(const char *s, int x, int y, int size, uint16_t col, bool shadow = true) {
  int w = (int)strlen(s) * 6 * size;
  if (x > W - w) x = W - w;
  if (x < 0) x = 0;
  if (shadow) fbChars(s, x + size, y + size, size, C_BLACK);
  fbChars(s, x, y, size, col);
}
static void fbTextC(const char *s, int y, int size, uint16_t col) {
  fbText(s, (W - (int)strlen(s) * 6 * size) / 2, y, size, col);
}

static float frand(float a, float b) { return a + (b - a) * (random(10001) / 10000.0f); }

// ======================================================================
class HopperView : public View {
public:
  void onEnter() override {
    Preferences p;
    p.begin("hopper", true);
    best_ = p.getUInt("best", 0);
    unlocked_ = p.getUChar("unlock", 0);
    autoRun_ = p.getUChar("ctrl", 0) == 1;
    p.end();
    if (unlocked_ >= HOP_LEVELS) unlocked_ = HOP_LEVELS - 1;
    enterTitle();
    lastMs_ = millis();
    fpsT0_ = lastMs_; frames_ = 0;
    EWLOGI("HOP", "enter heap_int=%u best=%lu unlock=%u ctrl=%s",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)best_, unlocked_,
           autoRun_ ? "auto" : "tilt");
  }

  void onExit() override {
    saveBest();
    tilesFree();
    reqWarp = -1; reqLevel = -1; reqForm = -1;
  }

  // The render task waits this long AFTER each frame before the next, and the
  // panel flush alone is ~49 ms: a short wait lets the flush set the pace
  // (33 ms here measured 10 fps on the watch, 98 ms per frame).
  uint16_t desiredFrameMs() const override { return 2; }

  void onEvent(const Event &e) override {
    uint32_t now = millis();
    switch (e.type) {
      case EventType::Touch:
        touchDown_ = true; lastTouchMs_ = now;
        onTap(e.x, e.y);
        break;
      case EventType::TouchHold:
        touchDown_ = true; lastTouchMs_ = now;
        break;
      case EventType::TouchUp:
        touchDown_ = false;
        break;
      case EventType::Gesture:
        if (e.gesture == Gesture::SwipeDown && mode_ == M_PLAY) startPound();
        break;
      case EventType::ButtonDown:
        btnDownMs_ = now; btnHeld_ = true; btnConsumed_ = false;
        break;
      case EventType::ButtonUp:
        btnHeld_ = false;
        break;
      case EventType::ButtonShort:
        if (btnConsumed_) { btnConsumed_ = false; break; }
        onButton();
        break;
      default:
        break;
    }
  }

  void render() override {
    Arduino_Canvas *cv = frameCanvas();
    if (!cv) return;
    fb = cv->getFramebuffer();
    if (!fb) return;
    Arduino_GFX *g = cv;
    g->setFont((const GFXfont *)nullptr);

    uint32_t now = millis();
    uint32_t us0 = micros();
    float dt = (now - lastMs_) / 1000.0f;
    lastMs_ = now;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0) dt = 0;
    animT_ += dt;

    handleConsole();

    // Measured on the watch: the game ran at 80 MHz although it asks for
    // frames (same as Doom, see doom/NOTES.md). Make sure of 240 every frame.
    if (getCpuFrequencyMhz() < 240) {
      powerPerfDemand(true);
      if (getCpuFrequencyMhz() < 240) { setCpuFrequencyMhz(240); clkFix_++; }
      else clkGov_++;
    }

    // Missed TouchUp fail-safe (holds arrive every ~30 ms while the finger is down).
    if (touchDown_ && now - lastTouchMs_ > 160) touchDown_ = false;
    // Side-button hold = pause.
    if (btnHeld_ && !btnConsumed_ && now - btnDownMs_ > 600 && mode_ == M_PLAY) {
      btnConsumed_ = true;
      pause();
    }

    bool direct = false;
    switch (mode_) {
      case M_TITLE:   tickTitle(dt);  drawTitle(g); break;
      case M_CARD:    tickCard(dt);   drawCard(g); break;
      case M_PLAY:
      case M_DYING:
      case M_FLAG:
      case M_CLEAR:
        tickGame(dt);
        if (presentDirect()) { direct = true; break; }
        drawWorld();
        drawPopups();
        if (mode_ == M_CLEAR) drawClear();
        clipTop = 0; drawHud(); clipTop = HUD;
        break;
      case M_PAUSE:
        drawWorld(); drawPopups();
        clipTop = 0; drawHud(); clipTop = HUD;
        drawPause(g);
        break;
      case M_OVER:    tickOver(dt);   drawOver(g); break;
      case M_WIN:     tickOver(dt);   drawWin(g); break;
    }

    uint32_t us1 = micros();
    if (!direct) cv->flush();
    uint32_t us2 = micros();
    drawUs_ += us1 - us0 - pushUs_; flushUs_ += us2 - us1 + pushUs_;
    pushUs_ = 0;

    frames_++;
    if (now - fpsT0_ >= 5000) {
      EWLOGI("HOP", "fps=%u mode=%u x=%d draw_ms=%u flush_ms=%u cpu=%u clk_gov=%u clk_force=%u dma=%d",
             (unsigned)(frames_ * 1000 / (now - fpsT0_)), (unsigned)mode_, (int)p_.x,
             (unsigned)(drawUs_ / frames_ / 1000), (unsigned)(flushUs_ / frames_ / 1000),
             (unsigned)getCpuFrequencyMhz(), (unsigned)clkGov_, (unsigned)clkFix_, lcdDma ? 1 : 0);
      clkGov_ = clkFix_ = 0;
      frames_ = 0; fpsT0_ = now; drawUs_ = flushUs_ = 0;
    }
  }

private:
  enum Mode : uint8_t { M_TITLE, M_CARD, M_PLAY, M_DYING, M_FLAG, M_CLEAR, M_PAUSE, M_OVER, M_WIN };
  Mode mode_ = M_TITLE;

  // persistent
  uint32_t best_ = 0;
  uint8_t  unlocked_ = 0;
  bool     autoRun_ = false;

  // run state
  int      level_ = 0;
  int      lives_ = 5;
  uint32_t score_ = 0;
  int      coins_ = 0;
  uint8_t  gems_ = 0;               // bit per gem of the current level
  uint8_t  form_ = 0;               // 0 small, 1 big, 2 fire
  bool     checkpoint_ = false;
  int      time_ = 300;
  float    timeAcc_ = 0;

  // level
  const HopLevel *lv_ = nullptr;
  int      lw_ = 0;
  uint8_t  theme_ = 0;
  int      startTx_ = 3, startTy_ = 13, ckTx_ = -1, ckTy_ = -1;
  int      poleTx_ = -1, doorTx_ = -1;
  int      gemTx_[3] = {-1, -1, -1}, gemTy_[3] = {-1, -1, -1};
  uint16_t skyRow_[H];
  float    camX_ = 0;

  // player
  Body     p_{};
  int8_t   face_ = 1;
  float    coyote_ = 0, jumpBuf_ = 0, invuln_ = 0, walkT_ = 0;
  bool     pounding_ = false;
  float    poundHang_ = 0;
  int      onPlat_ = -1;
  int      chain_ = 0;
  float    kickGrace_ = 0;
  float    modeT_ = 0;
  int      tally_ = 0;
  bool     bossLevelClear_ = false;

  // input
  bool     touchDown_ = false;
  uint32_t lastTouchMs_ = 0, btnDownMs_ = 0;
  bool     btnHeld_ = false, btnConsumed_ = false;
  float    tiltIn_ = 0;
  float    ax0_ = 0; int axN_ = 0;     // tilt neutral (averaged during the level card)

  // timing
  uint32_t lastMs_ = 0, fpsT0_ = 0, frames_ = 0, drawUs_ = 0, flushUs_ = 0;
  uint32_t clkGov_ = 0, clkFix_ = 0, pushUs_ = 0;   // 80 MHz seen at frame start: fixed by governor / forced
  float    animT_ = 0, acc_ = 0;

  // ============================================================ flow
  void enterTitle() {
    mode_ = M_TITLE; modeT_ = 0;
    loadLevel(0, false);
    camX_ = 0;
  }

  void newGame(int startLevel) {
    lives_ = 5; score_ = 0; coins_ = 0; form_ = 0;
    level_ = startLevel;
    gems_ = 0; checkpoint_ = false;
    enterCard();
  }

  void enterCard() {
    mode_ = M_CARD; modeT_ = 0;
    ax0_ = 0; axN_ = 0;
    loadLevel(level_, true);
  }

  void startPlay() {
    mode_ = M_PLAY; modeT_ = 0;
    if (axN_ > 0) ax0_ /= axN_;
    tiltIn_ = 0;
    acc_ = 0;
    EWLOGI("HOP", "start level=%s form=%u lives=%d ck=%d ax0=%d", lv_->id, form_, lives_,
           checkpoint_ ? 1 : 0, (int)ax0_);
  }

  void pause() {
    if (mode_ != M_PLAY) return;
    mode_ = M_PAUSE;
    haptic(HapticRole::ToggleOff);
    EWLOGI("HOP", "pause");
  }

  void onTap(int x, int y) {
    switch (mode_) {
      case M_TITLE:
        if (modeT_ < 0.3f) return;
        if (y >= 146 && y < 186) { newGame(0); return; }
        if (unlocked_ > 0 && y >= 190 && y < 222) { newGame(unlocked_); return; }
        if (y >= 228 && y < 256) {
          autoRun_ = !autoRun_;
          Preferences p; p.begin("hopper", false); p.putUChar("ctrl", autoRun_ ? 1 : 0); p.end();
          haptic(HapticRole::ModeChange);
          EWLOGI("HOP", "ctrl=%s", autoRun_ ? "auto" : "tilt");
        }
        break;
      case M_CARD:
        if (modeT_ > 0.5f) startPlay();
        break;
      case M_PLAY:
        jumpBuf_ = JUMP_BUF;
        break;
      case M_PAUSE:
        if (y >= 112 && y < 152) { mode_ = M_PLAY; lastMs_ = millis(); EWLOGI("HOP", "resume"); }
        else if (y >= 164 && y < 204) { saveBest(); enterTitle(); EWLOGI("HOP", "quit"); }
        break;
      case M_OVER:
      case M_WIN:
        if (modeT_ > 1.0f) { saveBest(); enterTitle(); }
        break;
      default: break;
    }
  }

  void onButton() {
    switch (mode_) {
      case M_TITLE: saveBest(); switchTo(Screen::AppList); return;
      case M_PLAY:  if (form_ == 2) throwFire(); break;
      case M_PAUSE: mode_ = M_PLAY; lastMs_ = millis(); EWLOGI("HOP", "resume"); break;
      case M_OVER: case M_WIN: saveBest(); enterTitle(); break;
      default: break;
    }
  }

  void saveBest() {
    if (score_ > best_) best_ = score_;
    Preferences p;
    p.begin("hopper", false);
    if (p.getUInt("best", 0) != best_) p.putUInt("best", best_);
    if (p.getUChar("unlock", 0) != unlocked_) p.putUChar("unlock", unlocked_);
    p.end();
  }

  // ============================================================ console
  void handleConsole() {
    if (reqLevel >= 0) {
      int l = reqLevel; reqLevel = -1;
      if (l < HOP_LEVELS) { if (mode_ == M_TITLE || mode_ == M_OVER || mode_ == M_WIN) { lives_ = 5; score_ = 0; coins_ = 0; form_ = 0; }
                            level_ = l; gems_ = 0; checkpoint_ = false; enterCard(); startPlay(); }
    }
    if (reqWarp >= 0 && (mode_ == M_PLAY || mode_ == M_PAUSE)) {
      int tx = reqWarp; reqWarp = -1;
      if (tx >= lw_) tx = lw_ - 1;
      p_.x = tx * TS + 2; p_.y = 0; p_.vx = p_.vy = 0;
      // drop from the top of the column onto the first surface
      if (reqWarpRow >= 0) {
        p_.y = reqWarpRow * TS - p_.h;          // feet at that row; falls from there
      } else {
        for (int ty = 0; ty < ROWS; ty++) {
          if (isSolid(map_[ty][tx]) || map_[ty][tx] == T_SEMI) { p_.y = ty * TS - p_.h; break; }
        }
      }
      camX_ = p_.x - 100; clampCam();
      mode_ = M_PLAY;
      EWLOGI("HOP", "warp col=%d y=%d", tx, (int)p_.y);
    }
    if (reqForm >= 0) {
      int f = reqForm; reqForm = -1;
      if (f >= 0 && f <= 2) setForm(f);
    }
    if (reqStatus) {
      reqStatus = false;
      EWLOGI("HOP", "status mode=%u level=%s x=%d y=%d form=%u lives=%d coins=%d score=%lu "
             "gems=%u time=%d tilt=%d", (unsigned)mode_, lv_ ? lv_->id : "-", (int)p_.x,
             (int)p_.y, form_, lives_, coins_, (unsigned long)score_, gems_, time_,
             (int)(tiltIn_ * 100));
    }
  }

  // ============================================================ level load
  void loadLevel(int idx, bool spawnPlayer) {
    lv_ = &HOP_LEVEL[idx];
    lw_ = lv_->width > MAXW ? MAXW : lv_->width;
    theme_ = lv_->theme;
    memset(map_, 0, sizeof(map_));
    for (auto &e : ents) e.type = E_NONE;
    for (auto &b : bumps) b.t = -1;
    for (auto &m : multi) m.n = 0;
    poleTx_ = doorTx_ = -1; ckTx_ = ckTy_ = -1;
    int gi = 0;
    for (int i = 0; i < 3; i++) gemTx_[i] = gemTy_[i] = -1;
    // gems are numbered left to right
    for (int tx = 0; tx < lw_; tx++) {
      for (int ty = 0; ty < ROWS; ty++) {
        char c = lv_->rows[ty][tx];
        uint8_t t = T_EMPTY;
        switch (c) {
          case '#': t = T_GROUND; break;
          case 'B': t = T_BRICK; break;
          case '?': t = T_QCOIN; break;
          case 'M': t = T_QPOW; break;
          case 'L': t = T_Q1UP; break;
          case 'm': t = T_BRICKC; break;
          case 'X': t = T_SOLID; break;
          case 'f': t = T_SOLID; spawn(E_FIREBAR, tx * TS, ty * TS); break;
          case '[': t = T_LOG_TL; break;
          case ']': t = T_LOG_TR; break;
          case '{': t = T_LOG_L; break;
          case '}': t = T_LOG_R; break;
          case 'o': t = T_COIN; break;
          case '-': t = T_SEMI; break;
          case '~': t = T_LAVA; break;
          case '|': t = T_POLE; if (poleTx_ < 0) poleTx_ = tx; break;
          case '^': t = T_POLE_TOP; break;
          case 'F': t = T_FORT; break;
          case 'T': t = T_FORT_TOP; break;
          case 'D': t = T_DOOR; doorTx_ = tx; break;
          case 'd': t = T_DOOR_TOP; break;
          case 'b': t = T_BUSH; break;
          case 'k': t = T_CHECK; ckTx_ = tx; ckTy_ = ty; break;
          case 'A': t = T_SWITCH; break;
          case '*':
            if (gi < 3) { gemTx_[gi] = tx; gemTy_[gi] = ty; if (!(gems_ & (1 << gi))) t = T_GEM; gi++; }
            break;
          case '@': startTx_ = tx; startTy_ = ty; break;
          case 'e': spawn(E_LADYBUG, tx * TS + 1, ty * TS + 2); break;
          case 's': spawn(E_SNAIL, tx * TS + 1, ty * TS + 2); break;
          case 'p': spawn(E_SPIKY, tx * TS + 1, ty * TS + 2); break;
          case 'K': spawn(E_BOSS, tx * TS, ty * TS - 12); break;
          case 'h': spawn(E_PLAT_H, tx * TS, ty * TS); break;
          case 'v': spawn(E_PLAT_V, tx * TS, ty * TS); break;
          default: break;
        }
        map_[ty][tx] = t;
      }
    }
    if (poleTx_ >= 0) {
      int e = spawn(E_FLAG, poleTx_ * TS - 15, (GT - 10) * TS);
      (void)e;
    }
    if (tileRamTheme != theme_) tilesLoad(theme_);
    // sky gradient
    const HopTheme &th = HOP_THEME[theme_];
    for (int y = 0; y < H; y++) skyRow_[y] = lerp565(th.skyTop, th.skyBot, y, H);

    time_ = lv_->time; timeAcc_ = 0;
    bossLevelClear_ = false;
    if (spawnPlayer) {
      int tx = startTx_, ty = startTy_;
      if (checkpoint_ && ckTx_ >= 0) { tx = ckTx_; ty = ckTy_; map_[ckTy_][ckTx_] = T_CHECK; }
      p_ = Body{};
      p_.w = PW; p_.h = form_ ? PH_BIG : PH_SMALL;
      p_.x = tx * TS + 2;
      p_.y = (ty + 1) * TS - p_.h;
      face_ = 1; invuln_ = 0; coyote_ = 0; jumpBuf_ = 0; pounding_ = false; onPlat_ = -1;
      chain_ = 0; kickGrace_ = 0; walkT_ = 0;
      camX_ = p_.x - 100; clampCam();
    }
  }

  int spawn(EType t, float x, float y) {
    for (int i = 0; i < MAXE; i++) {
      Ent &e = ents[i];
      if (e.type != E_NONE) continue;
      e = Ent{};
      e.type = t; e.state = S_LIVE; e.active = false; e.dir = -1;
      e.b.x = x; e.b.y = y; e.ox = x; e.oy = y; e.t = 0;
      e.b.headTx = e.b.headTy = -1;
      switch (t) {
        case E_LADYBUG: case E_SPIKY: e.b.w = 14; e.b.h = 13; e.b.y = y + 1; break;
        case E_SNAIL:   e.b.w = 14; e.b.h = 13; e.b.y = y + 1; break;
        case E_SHELL:   e.b.w = 14; e.b.h = 10; break;
        case E_BOSS:    e.b.w = 28; e.b.h = 26; e.hp = 6; break;
        case E_BERRY: case E_CHILI: case E_ONEUP: e.b.w = 14; e.b.h = 14; e.active = true; break;
        case E_FIREBALL: e.b.w = 8; e.b.h = 8; e.active = true; break;
        case E_PLAT_H: case E_PLAT_V: e.b.w = 48; e.b.h = 6; e.active = true; break;
        case E_FIREBAR: e.active = true; e.dir = ((int)(x / TS) & 1) ? 1 : -1; break;
        default: e.active = true; break;
      }
      return i;
    }
    return -1;
  }

  void clampCam() {
    float maxc = lw_ * TS - W;
    if (camX_ > maxc) camX_ = maxc;
    if (camX_ < 0) camX_ = 0;
  }

  // ============================================================ tiles
  uint8_t tileAt(int tx, int ty) const {
    if (tx < 0 || tx >= lw_ || ty < 0 || ty >= ROWS) return T_EMPTY;
    return map_[ty][tx];
  }
  bool solidAt(int tx, int ty) const {
    if (tx < 0 || tx >= lw_) return true;      // level edges are walls
    if (ty < 0 || ty >= ROWS) return false;
    return isSolid(map_[ty][tx]);
  }

  // Move a body through the tile map. Semi-solid planks and moving platforms
  // catch it only when falling onto them from above.
  void moveBody(Body &b, float dt, bool platforms, int *platOut = nullptr) {
    b.hitWall = false; b.headTx = b.headTy = -1;
    // X
    float dx = b.vx * dt;
    if (dx != 0) {
      float nx = b.x + dx;
      int ty0 = (int)floorf(b.y / TS), ty1 = (int)floorf((b.y + b.h - 1) / TS);
      if (dx > 0) {
        int tx = (int)floorf((nx + b.w - 1) / TS);
        for (int ty = ty0; ty <= ty1; ty++)
          if (solidAt(tx, ty)) { nx = tx * TS - b.w; b.hitWall = true; break; }
      } else {
        int tx = (int)floorf(nx / TS);
        for (int ty = ty0; ty <= ty1; ty++)
          if (solidAt(tx, ty)) { nx = (tx + 1) * TS; b.hitWall = true; break; }
      }
      b.x = nx;
    }
    // Y
    float dy = b.vy * dt;
    float oldBottom = b.y + b.h;
    b.onGround = false;
    if (platOut) *platOut = -1;
    float ny = b.y + dy;
    int tx0 = (int)floorf(b.x / TS), tx1 = (int)floorf((b.x + b.w - 1) / TS);
    if (dy > 0) {
      int ty = (int)floorf((ny + b.h - 0.01f) / TS);
      for (int tx = tx0; tx <= tx1; tx++) {
        uint8_t t = tileAt(tx, ty);
        bool land = solidAt(tx, ty) && ty >= 0 && ty < ROWS;
        if (!land && t == T_SEMI && oldBottom <= ty * TS + 0.5f) land = true;
        if (land) { ny = ty * TS - b.h; b.vy = 0; b.onGround = true; break; }
      }
      if (platforms && !b.onGround) {
        for (int i = 0; i < MAXE; i++) {
          Ent &e = ents[i];
          if (e.type != E_PLAT_H && e.type != E_PLAT_V) continue;
          float top = e.b.y;
          if (b.x + b.w <= e.b.x || b.x >= e.b.x + e.b.w) continue;
          if (oldBottom <= top + 1.0f && ny + b.h >= top) {
            ny = top - b.h; b.vy = 0; b.onGround = true;
            if (platOut) *platOut = i;
            break;
          }
        }
      }
    } else if (dy < 0) {
      int ty = (int)floorf(ny / TS);
      if (ty >= 0) {
        int best = -1; float bestD = 1e9f;
        float cx = b.x + b.w * 0.5f;
        for (int tx = tx0; tx <= tx1; tx++) {
          if (!solidAt(tx, ty)) continue;
          float d = fabsf((tx * TS + TS * 0.5f) - cx);
          if (d < bestD) { bestD = d; best = tx; }
        }
        if (best >= 0) {
          ny = (ty + 1) * TS; b.vy = 0;
          b.headTx = best; b.headTy = ty;
        }
      }
    }
    b.y = ny;
  }

  void addBump(int tx, int ty) {
    for (auto &b : bumps) if (b.t < 0) { b.tx = tx; b.ty = ty; b.t = 0; return; }
  }

  void bumpTile(int tx, int ty, bool big) {
    uint8_t t = tileAt(tx, ty);
    float bx = tx * TS, by = ty * TS;
    switch (t) {
      case T_QCOIN:
        map_[ty][tx] = T_USED; addBump(tx, ty); coinPop(bx, by); break;
      case T_QPOW: {
        map_[ty][tx] = T_USED; addBump(tx, ty);
        int i = spawn(form_ == 0 ? E_BERRY : E_CHILI, bx + 1, by);
        if (i >= 0) { ents[i].state = S_EMERGE; ents[i].dir = 1; }
        haptic(HapticRole::HitLight);
        break;
      }
      case T_Q1UP: {
        map_[ty][tx] = T_USED; addBump(tx, ty);
        int i = spawn(E_ONEUP, bx + 1, by);
        if (i >= 0) { ents[i].state = S_EMERGE; ents[i].dir = 1; }
        break;
      }
      case T_BRICKC: {
        addBump(tx, ty); coinPop(bx, by);
        MultiCoin *m = nullptr;
        for (auto &mc : multi) if (mc.n && mc.tx == tx && mc.ty == ty) m = &mc;
        if (m) {
          m->n++;
        } else {
          for (auto &mc : multi) if (!mc.n) { m = &mc; m->tx = tx; m->ty = ty; m->n = 1; break; }
        }
        if (!m || m->n >= 8) map_[ty][tx] = T_USED;   // 8 coins, then spent
        break;
      }
      case T_BRICK:
        if (big) {
          map_[ty][tx] = T_EMPTY;
          for (int k = 0; k < 4; k++) {
            int i = spawn(E_DEBRIS, bx + (k & 1) * 8, by + (k >> 1) * 8);
            if (i >= 0) {
              ents[i].b.vx = (k & 1) ? 70 : -70;
              ents[i].b.vy = (k >> 1) ? -220 : -330;
            }
          }
          addScore(50, bx, by);
          haptic(HapticRole::HitHeavy);
        } else {
          addBump(tx, ty);
          haptic(HapticRole::HitLight);
        }
        break;
      default:
        return;
    }
    // things standing on the bumped block get knocked
    for (auto &e : ents) {
      if (e.type == E_NONE || e.state == S_KNOCKED) continue;
      if (e.b.x + e.b.w <= bx || e.b.x >= bx + TS) continue;
      if (fabsf((e.b.y + e.b.h) - by) > 4) continue;
      if (isEnemy(e.type)) knock(e, e.b.x < bx + 8 ? -1 : 1, 100);
      else if (e.type == E_BERRY || e.type == E_ONEUP) { e.b.vy = -220; e.dir = (e.b.x + 7 < bx + 8) ? -1 : 1; }
    }
    if (tileAt(tx, ty - 1) == T_COIN) { map_[ty - 1][tx] = T_EMPTY; coinPop(bx, by - TS); }
  }

  void coinPop(float x, float y) {
    int i = spawn(E_COINPOP, x, y - 8);
    if (i >= 0) ents[i].b.vy = -320;
    addCoin();
  }

  void addCoin() {
    coins_++;
    score_ += 200;
    if (coins_ >= 100) { coins_ -= 100; oneUp(p_.x, p_.y); }
  }

  void oneUp(float x, float y) {
    lives_++;
    int i = spawn(E_SCORE, x, y - 10);
    if (i >= 0) ents[i].val = -1;   // "1UP"
    haptic(HapticRole::LevelUp);
    EWLOGI("HOP", "oneup lives=%d", lives_);
  }

  void addScore(int pts, float x, float y) {
    score_ += pts;
    int i = spawn(E_SCORE, x, y);
    if (i >= 0) ents[i].val = pts;
  }

  static bool isEnemy(EType t) {
    return t == E_LADYBUG || t == E_SNAIL || t == E_SHELL || t == E_SPIKY;
  }

  void knock(Ent &e, int dir, int pts) {
    if (e.type == E_BOSS) return;
    e.state = S_KNOCKED;
    e.b.vy = -260; e.b.vx = 50.0f * dir;
    e.t = 0;
    if (pts) addScore(pts, e.b.x, e.b.y);
    haptic(HapticRole::HitLight);
  }

  // ============================================================ player
  void setForm(int f) {
    int oldH = p_.h;
    form_ = (uint8_t)f;
    p_.h = f ? PH_BIG : PH_SMALL;
    p_.y += oldH - p_.h;               // keep the feet where they were
    EWLOGI("HOP", "form=%d", f);
  }

  void hurt(const char *why) {
    if (godMode || invuln_ > 0 || mode_ != M_PLAY) return;
    if (form_ == 2) { setForm(1); }
    else if (form_ == 1) { setForm(0); }
    else { die(why); return; }
    invuln_ = 1.6f;
    haptic(HapticRole::Damage);
    EWLOGI("HOP", "hurt by=%s form=%u", why, form_);
  }

  void die(const char *why) {
    if (mode_ != M_PLAY) return;
    if (godMode && strcmp(why, "pit") != 0) return;
    mode_ = M_DYING; modeT_ = 0;
    p_.vx = 0; p_.vy = -340;
    pounding_ = false;
    haptic(HapticRole::GameOver);
    EWLOGI("HOP", "die cause=%s level=%s x=%d", why, lv_->id, (int)p_.x);
  }

  void startPound() {
    if (p_.onGround || pounding_ || mode_ != M_PLAY) return;
    pounding_ = true; poundHang_ = 0.14f;
    p_.vx = 0; p_.vy = 0;
    EWLOGI("HOP", "pound");
  }

  void throwFire() {
    int n = 0;
    for (auto &e : ents) if (e.type == E_FIREBALL) n++;
    if (n >= 2) return;
    int i = spawn(E_FIREBALL, face_ > 0 ? p_.x + p_.w : p_.x - 8, p_.y + 4);
    if (i < 0) return;
    ents[i].dir = face_;
    ents[i].b.vx = 230.0f * face_;
    ents[i].b.vy = 80;
    haptic(HapticRole::Shoot);
  }

  void readTilt(float dt) {
    int16_t ax; bool ok;
    { ModelLock lk; ax = model.ax; ok = model.imuOk; }
    float target = 0;
    if (ok) {
      float v = TILT_SIGN * ((float)ax - ax0_) / TILT_FULL;
      float a = fabsf(v);
      if (a < TILT_DEAD) v = 0;
      else { a = (a - TILT_DEAD) / (1.0f - TILT_DEAD); if (a > 1) a = 1; v = v > 0 ? a : -a; }
      target = v;
    }
    float k = dt * 14.0f; if (k > 1) k = 1;
    tiltIn_ += (target - tiltIn_) * k;
  }

  void stepPlayer(float dt) {
    // ---- horizontal input
    float target;
    if (autoRun_) target = AUTO_RUN_V;
    else          target = tiltIn_ * MAXV;
    if (pounding_) target = 0;

    if (target > 1) face_ = 1; else if (target < -1) face_ = -1;
    float acc;
    bool speedingUp = (target > 0 && p_.vx < target) || (target < 0 && p_.vx > target);
    if (speedingUp && !(fabsf(target) < 1))
      acc = p_.onGround ? ACC_GROUND : ACC_AIR;
    else
      acc = p_.onGround ? DEC_GROUND : DEC_AIR;
    if (p_.vx < target) { p_.vx += acc * dt; if (p_.vx > target) p_.vx = target; }
    else if (p_.vx > target) { p_.vx -= acc * dt; if (p_.vx < target) p_.vx = target; }

    // ---- platform carry
    if (onPlat_ >= 0 && p_.onGround) {
      Ent &pl = ents[onPlat_];
      if (pl.type == E_PLAT_H || pl.type == E_PLAT_V) {
        float svx = p_.vx, svy = p_.vy;
        p_.vx = pl.pdx / dt; p_.vy = 0;
        moveBody(p_, dt, false);
        p_.vx = svx; p_.vy = svy;
        p_.y = pl.b.y - p_.h;
      }
    }

    // ---- vertical
    if (pounding_ && poundHang_ > 0) {
      poundHang_ -= dt; p_.vy = 0;
    } else if (pounding_) {
      p_.vy = POUND_V;
    } else {
      bool held = touchDown_;
      float gr = (held && p_.vy < 0) ? GRAV_HOLD : GRAV;
      p_.vy += gr * dt;
      if (p_.vy > MAX_FALL) p_.vy = MAX_FALL;
    }

    bool wasGround = p_.onGround;
    float preVy = p_.vy;
    int plat = -1;
    moveBody(p_, dt, true, &plat);
    onPlat_ = plat;

    // ---- head bump
    if (p_.headTy >= 0) {
      // corner forgiveness: a few px of overlap slides you past the edge
      int tx = p_.headTx;
      float left = tx * TS, right = left + TS;
      bool soloL = !solidAt(tx - 1, p_.headTy), soloR = !solidAt(tx + 1, p_.headTy);
      if (soloR && p_.x + 4 >= right - 0.5f && p_.x < right) { p_.x = right; p_.vy = preVy; }
      else if (soloL && p_.x + p_.w - 4 <= left + 0.5f && p_.x + p_.w > left) { p_.x = left - p_.w; p_.vy = preVy; }
      else { bumpTile(p_.headTx, p_.headTy, form_ > 0); }
    }

    // ---- landing
    if (p_.onGround) {
      coyote_ = COYOTE;
      if (!wasGround) chain_ = 0;
      if (pounding_) poundLand();
    } else {
      coyote_ -= dt;
    }

    // ---- jump
    if (jumpBuf_ > 0) jumpBuf_ -= dt;
    if (jumpBuf_ > 0 && (p_.onGround || coyote_ > 0) && !pounding_) {
      p_.vy = -(JUMP_V + JUMP_RUN * fabsf(p_.vx));
      p_.onGround = false; coyote_ = 0; jumpBuf_ = 0; onPlat_ = -1;
      haptic(HapticRole::Tap);
    }

    if (p_.onGround && fabsf(p_.vx) > 5) walkT_ += dt * (0.5f + fabsf(p_.vx) / 60.0f);
    if (invuln_ > 0) invuln_ -= dt;
    if (kickGrace_ > 0) kickGrace_ -= dt;

    // ---- tiles the body overlaps
    int tx0 = (int)floorf(p_.x / TS), tx1 = (int)floorf((p_.x + p_.w - 1) / TS);
    int ty0 = (int)floorf(p_.y / TS), ty1 = (int)floorf((p_.y + p_.h - 1) / TS);
    for (int ty = ty0; ty <= ty1; ty++) {
      for (int tx = tx0; tx <= tx1; tx++) {
        uint8_t t = tileAt(tx, ty);
        switch (t) {
          case T_COIN: map_[ty][tx] = T_EMPTY; addCoin(); break;
          case T_GEM:
            map_[ty][tx] = T_EMPTY;
            for (int i = 0; i < 3; i++) if (gemTx_[i] == tx && gemTy_[i] == ty) gems_ |= 1 << i;
            score_ += 1000;
            haptic(HapticRole::PowerUp);
            EWLOGI("HOP", "gem mask=%u", gems_);
            break;
          case T_CHECK:
            if (!checkpoint_) {
              checkpoint_ = true;
              haptic(HapticRole::Confirm);
              EWLOGI("HOP", "checkpoint col=%d", tx);
              if (form_ == 0) setForm(1);   // the midway flag powers you up
            }
            break;
          case T_LAVA:
            if (p_.y + p_.h > ty * TS + 6) die("lava");
            break;
          case T_SWITCH:
            bossClear();
            break;
          default: break;
        }
        if (mode_ != M_PLAY) return;
      }
    }

    // ---- goal reed
    // (touching the reed's base block counts: walking into it must not get stuck)
    if (poleTx_ >= 0 && p_.x + p_.w >= poleTx_ * TS - 1) { grabPole(); return; }

    // ---- pit
    if (p_.y > ROWS * TS + 8) die("pit");
  }

  void poundLand() {
    pounding_ = false;
    haptic(HapticRole::HitHeavy);
    int ty = (int)floorf((p_.y + p_.h + 1) / TS);
    int tx0 = (int)floorf(p_.x / TS), tx1 = (int)floorf((p_.x + p_.w - 1) / TS);
    for (int tx = tx0; tx <= tx1; tx++) {
      uint8_t t = tileAt(tx, ty);
      if (t == T_BRICK && form_ > 0) {
        bumpTile(tx, ty, true);
      } else if (t == T_QCOIN || t == T_QPOW || t == T_Q1UP || t == T_BRICKC) {
        bumpTile(tx, ty, form_ > 0);
      }
    }
    // shockwave: knock walkers near the impact
    for (auto &e : ents) {
      if (!isEnemy(e.type) || e.state != S_LIVE || !e.active) continue;
      if (!e.b.onGround) continue;
      if (fabsf((e.b.y + e.b.h) - (p_.y + p_.h)) > 4) continue;
      if (fabsf((e.b.x + e.b.w * 0.5f) - (p_.x + p_.w * 0.5f)) < 40) knock(e, e.b.x > p_.x ? 1 : -1, 100);
    }
  }

  void grabPole() {
    mode_ = M_FLAG; modeT_ = 0;
    p_.x = poleTx_ * TS + 6 - p_.w;
    p_.vx = 0; p_.vy = 0;
    pounding_ = false;
    float baseY = (GT - 1) * TS;              // top of the reed's base block
    float hTiles = (baseY - (p_.y + p_.h)) / TS;
    int pts = hTiles >= 8 ? 5000 : hTiles >= 6 ? 2000 : hTiles >= 4 ? 800 : hTiles >= 2 ? 400 : 100;
    addScore(pts, p_.x + 14, p_.y);
    haptic(HapticRole::LevelUp);
    EWLOGI("HOP", "goal level=%s height=%d pts=%d time=%d gems=%u", lv_->id, (int)hTiles, pts,
           time_, gems_);
  }

  void bossClear() {
    if (mode_ != M_PLAY) return;
    for (auto &e : ents) if (e.type == E_BOSS && e.state != S_KNOCKED) { e.state = S_KNOCKED; e.b.vy = -200; }
    mode_ = M_CLEAR; modeT_ = 0; tally_ = 0; bossLevelClear_ = true;
    p_.vx = 0;
    haptic(HapticRole::LevelUp);
    EWLOGI("HOP", "boss_clear level=%s time=%d", lv_->id, time_);
  }

  // ============================================================ entities
  void stepEnts(float dt) {
    float viewR = camX_ + W + 24, viewL = camX_ - 24;
    for (int i = 0; i < MAXE; i++) {
      Ent &e = ents[i];
      if (e.type == E_NONE) continue;
      e.t += dt;
      Body &b = e.b;

      if (!e.active) {
        if (b.x < viewR && b.x + b.w > viewL - 200) {
          e.active = true;
          e.dir = (b.x > p_.x) ? -1 : 1;
        } else continue;
      }
      if (e.state == S_KNOCKED) {
        b.vy += E_GRAV * dt;
        b.x += b.vx * dt; b.y += b.vy * dt;
        if (b.y > H + 32) e.type = E_NONE;
        continue;
      }

      switch (e.type) {
        case E_LADYBUG: case E_SPIKY: case E_SNAIL: {
          if (e.state == S_SQUASHED) { if (e.t > 0.5f) e.type = E_NONE; break; }
          float sp = e.type == E_LADYBUG ? 32 : 26;
          b.vx = sp * e.dir;
          b.vy += E_GRAV * dt; if (b.vy > MAX_FALL) b.vy = MAX_FALL;
          moveBody(b, dt, true);
          if (b.hitWall) e.dir = -e.dir;
          if (e.type == E_SNAIL && b.onGround) {
            int tx = e.dir > 0 ? (int)floorf((b.x + b.w) / TS) : (int)floorf((b.x - 1) / TS);
            int ty = (int)floorf((b.y + b.h + 1) / TS);
            if (!solidAt(tx, ty) && tileAt(tx, ty) != T_SEMI) e.dir = -e.dir;
          }
          if (b.y > ROWS * TS + 16) e.type = E_NONE;
          break;
        }
        case E_SHELL: {
          b.vx = e.val ? 210.0f * e.dir : 0;
          b.vy += E_GRAV * dt; if (b.vy > MAX_FALL) b.vy = MAX_FALL;
          float prevX = b.x;
          moveBody(b, dt, true);
          if (b.hitWall && e.val) {
            int tx = e.dir > 0 ? (int)floorf((b.x + b.w) / TS) : (int)floorf((prevX - 1) / TS);
            int ty = (int)floorf((b.y + b.h * 0.5f) / TS);
            uint8_t t = tileAt(tx, ty);
            if (t == T_BRICK || t == T_QCOIN || t == T_QPOW || t == T_Q1UP || t == T_BRICKC)
              bumpTile(tx, ty, true);
            e.dir = -e.dir;
          }
          if (e.val) {   // a moving shell bowls over other enemies
            for (int j = 0; j < MAXE; j++) {
              Ent &o = ents[j];
              if (j == i || !isEnemy(o.type) || o.state != S_LIVE || !o.active) continue;
              if (overlap(b, o.b)) { knock(o, e.dir, 200); }
            }
          }
          if (b.y > ROWS * TS + 16 || b.x < camX_ - 300 || b.x > camX_ + W + 300) e.type = E_NONE;
          break;
        }
        case E_BOSS: stepBoss(e, dt); break;
        case E_BERRY: case E_ONEUP: case E_CHILI: {
          if (e.state == S_EMERGE) {
            b.y = e.oy - 16.0f * (e.t / 0.6f);
            if (e.t >= 0.6f) { e.state = S_LIVE; b.y = e.oy - 16; }
            break;
          }
          b.vx = e.type == E_CHILI ? 0 : 60.0f * e.dir;
          b.vy += E_GRAV * dt; if (b.vy > MAX_FALL) b.vy = MAX_FALL;
          moveBody(b, dt, true);
          if (b.hitWall) e.dir = -e.dir;
          if (b.y > ROWS * TS + 16) e.type = E_NONE;
          break;
        }
        case E_FIREBALL: {
          b.vx = 230.0f * e.dir;
          b.vy += 1300 * dt; if (b.vy > 360) b.vy = 360;
          bool wasDown = b.vy > 0;
          moveBody(b, dt, true);
          if (b.onGround && wasDown) b.vy = -200;
          if (b.hitWall || e.t > 2.0f || b.x < camX_ - 16 || b.x > camX_ + W + 16 || b.y > ROWS * TS) { e.type = E_NONE; break; }
          for (int j = 0; j < MAXE; j++) {
            Ent &o = ents[j];
            if (o.type == E_NONE || o.state == S_KNOCKED || !o.active) continue;
            if (isEnemy(o.type) && overlap(b, o.b)) { knock(o, e.dir, 200); e.type = E_NONE; break; }
            if (o.type == E_BOSS && overlap(b, o.b)) { hitBoss(o, 1); e.type = E_NONE; break; }
          }
          break;
        }
        case E_COINPOP:
          b.vy += 1100 * dt; b.y += b.vy * dt;
          if (e.t > 0.5f) e.type = E_NONE;
          break;
        case E_DEBRIS:
          b.vy += 1300 * dt; b.x += b.vx * dt; b.y += b.vy * dt;
          if (e.t > 1.2f) e.type = E_NONE;
          break;
        case E_SCORE:
          b.y -= 30 * dt;
          if (e.t > 0.8f) e.type = E_NONE;
          break;
        case E_PLAT_H: {
          float nx = e.ox + 56.0f * sinf(e.t * 0.9f);
          e.pdx = nx - b.x; e.pdy = 0; b.x = nx;
          break;
        }
        case E_PLAT_V: {
          float ny = e.oy + 48.0f * sinf(e.t * 0.8f);
          e.pdx = 0; e.pdy = ny - b.y; b.y = ny;
          break;
        }
        case E_FIREBAR: break;
        case E_FLAG:
          if (mode_ == M_FLAG && modeT_ > 0.1f) {
            float bottom = (GT - 2) * TS;
            if (b.y < bottom) b.y += 150 * dt;
          }
          break;
        default: break;
      }
    }
  }
  void stepBoss(Ent &e, float dt) {
    Body &b = e.b;
    if (e.inv > 0) e.inv -= dt;
    float sp = e.hp > 3 ? 45 : 70;
    if (fabsf(b.x - e.ox) > 80) e.dir = b.x > e.ox ? -1 : 1;
    else if (e.t > 1.0f && ((p_.x > b.x + 20 && e.dir < 0) || (p_.x + 12 < b.x - 4 && e.dir > 0)) && random(100) < 2)
      e.dir = -e.dir;
    b.vx = sp * e.dir;
    b.vy += E_GRAV * dt; if (b.vy > MAX_FALL) b.vy = MAX_FALL;
    moveBody(b, dt, false);
    if (b.hitWall) e.dir = -e.dir;
    if (b.onGround && e.t > 2.2f) { b.vy = e.hp > 3 ? -300 : -380; e.t = 0; }
  }

  void hitBoss(Ent &e, int dmg) {
    if (e.inv > 0 || e.state != S_LIVE) return;
    e.hp -= dmg; e.inv = 0.8f;
    haptic(HapticRole::HitHeavy);
    EWLOGI("HOP", "boss_hit hp=%d", e.hp);
    if (e.hp <= 0) {
      e.state = S_KNOCKED; e.b.vy = -300; e.b.vx = 0;
      addScore(5000, e.b.x, e.b.y);
      bossClear();
    }
  }

  static bool overlap(const Body &a, const Body &b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
  }

  int stompScore() {
    static const int tbl[] = {100, 200, 400, 500, 800, 1000, 2000, 4000, 5000, 8000};
    int c = chain_++;
    if (c >= 10) return -1;
    return tbl[c];
  }

  void stompPoints(float x, float y) {
    int s = stompScore();
    if (s < 0) oneUp(x, y);
    else addScore(s, x, y);
  }

  void interact() {
    if (mode_ != M_PLAY) return;
    for (int i = 0; i < MAXE; i++) {
      Ent &e = ents[i];
      if (e.type == E_NONE || !e.active || e.state == S_KNOCKED || e.state == S_SQUASHED) continue;
      Body &b = e.b;
      switch (e.type) {
        case E_LADYBUG: case E_SNAIL: case E_SPIKY: case E_SHELL: case E_BOSS: {
          if (!overlap(p_, b)) break;
          bool fromAbove = p_.vy > 0 && (p_.y + p_.h) - b.y < (e.type == E_BOSS ? 12 : 9);
          if (pounding_ && e.type != E_SPIKY && e.type != E_BOSS) fromAbove = true;
          if (e.type == E_SPIKY) { hurt("spiky"); break; }
          if (e.type == E_BOSS) {
            if (fromAbove && e.inv <= 0) {
              hitBoss(e, 2);
              p_.vy = -STOMP_V_HELD; pounding_ = false;
            } else if (e.inv <= 0 || !fromAbove) hurt("boss");
            break;
          }
          if (e.type == E_SHELL) {
            if (e.val == 0) {   // idle shell: kick it
              e.val = 1;
              e.dir = (b.x + b.w * 0.5f > p_.x + p_.w * 0.5f) ? 1 : -1;
              kickGrace_ = 0.25f;
              addScore(400, b.x, b.y);
              haptic(HapticRole::HitLight);
              if (fromAbove) p_.vy = -STOMP_V;
            } else if (fromAbove) {
              e.val = 0;
              p_.vy = touchDown_ ? -STOMP_V_HELD : -STOMP_V;
              stompPoints(b.x, b.y);
              haptic(HapticRole::HitLight);
            } else if (kickGrace_ <= 0) {
              hurt("shell");
            }
            break;
          }
          if (fromAbove) {
            p_.vy = touchDown_ ? -STOMP_V_HELD : -STOMP_V;
            if (pounding_) { pounding_ = false; p_.vy = -STOMP_V; }
            stompPoints(b.x, b.y);
            haptic(HapticRole::HitHeavy);
            if (e.type == E_LADYBUG) { e.state = S_SQUASHED; e.t = 0; }
            else {   // snail -> shell
              float sx = b.x, sy = b.y + b.h - 10;
              e.type = E_NONE;
              int j = spawn(E_SHELL, sx, sy);
              if (j >= 0) { ents[j].active = true; ents[j].val = 0; }
            }
          } else {
            hurt(e.type == E_LADYBUG ? "ladybug" : "snail");
          }
          break;
        }
        case E_BERRY:
          if (e.state == S_LIVE && overlap(p_, b)) {
            e.type = E_NONE;
            if (form_ == 0) setForm(1);
            addScore(1000, b.x, b.y);
            haptic(HapticRole::PowerUp);
          }
          break;
        case E_CHILI:
          if (e.state == S_LIVE && overlap(p_, b)) {
            e.type = E_NONE;
            setForm(2);
            addScore(1000, b.x, b.y);
            haptic(HapticRole::PowerUp);
          }
          break;
        case E_ONEUP:
          if (e.state == S_LIVE && overlap(p_, b)) { e.type = E_NONE; oneUp(b.x, b.y); }
          break;
        case E_FIREBAR: {
          float cx = e.ox + 8, cy = e.oy + 8;
          float a = e.t * 1.7f * e.dir;
          float ca = cosf(a), sa = sinf(a);
          for (int k = 1; k < 6; k++) {
            float bx = cx + ca * k * 8, by = cy + sa * k * 8;
            float nx = bx < p_.x ? p_.x : (bx > p_.x + p_.w ? p_.x + p_.w : bx);
            float ny = by < p_.y ? p_.y : (by > p_.y + p_.h ? p_.y + p_.h : by);
            if ((nx - bx) * (nx - bx) + (ny - by) * (ny - by) < 3.5f * 3.5f) { hurt("firebar"); break; }
          }
          break;
        }
        default: break;
      }
      if (mode_ != M_PLAY) return;
    }
  }

  // ============================================================ game tick
  void tickGame(float dt) {
    modeT_ += dt;
    for (auto &bp : bumps) if (bp.t >= 0) { bp.t += dt; if (bp.t > 0.18f) bp.t = -1; }

    if (mode_ == M_PLAY) {
      if (!autoRun_) readTilt(dt);
      acc_ += dt;
      int n = 0;
      while (acc_ >= STEP && n < 12) {
        acc_ -= STEP; n++;
        stepEnts(STEP);
        stepPlayer(STEP);
        interact();
        if (mode_ != M_PLAY) break;
      }
      if (mode_ == M_PLAY) {
        timeAcc_ += dt;
        while (timeAcc_ >= 0.5f) {
          timeAcc_ -= 0.5f;
          if (--time_ <= 0) { time_ = 0; die("time"); break; }
        }
      }
      // camera window
      float sx = p_.x - camX_;
      if (sx > 116) camX_ = p_.x - 116;
      if (sx < 80) camX_ = p_.x - 80;
      clampCam();
    } else if (mode_ == M_DYING) {
      if (modeT_ > 0.4f) { p_.vy += 1300 * dt; p_.y += p_.vy * dt; }
      if (modeT_ > 2.4f) {
        lives_--;
        form_ = 0;
        if (lives_ <= 0) {
          mode_ = M_OVER; modeT_ = 0;
          if (score_ > best_) best_ = score_;
          saveBest();
          EWLOGI("HOP", "gameover score=%lu", (unsigned long)score_);
        } else {
          enterCard();
        }
      }
    } else if (mode_ == M_FLAG) {
      stepEnts(dt);
      float baseFeet = (GT - 1) * TS;
      if (p_.y + p_.h < baseFeet) {
        p_.y += 140 * dt;
        if (p_.y + p_.h > baseFeet) p_.y = baseFeet - p_.h;
      } else if (modeT_ > 1.0f) {
        // hop off and walk into the fort
        face_ = 1;
        if (p_.x < poleTx_ * TS + 10) { p_.x = poleTx_ * TS + 10; }
        p_.vx = 70; p_.vy += GRAV * dt;
        moveBody(p_, dt, false);
        walkT_ += dt * 1.6f;
        if (doorTx_ >= 0 && p_.x >= doorTx_ * TS + 2) { mode_ = M_CLEAR; modeT_ = 0; tally_ = 0; }
      }
      float sx = p_.x - camX_;
      if (sx > 150) camX_ = p_.x - 150;
      clampCam();
    } else if (mode_ == M_CLEAR) {
      stepEnts(dt);
      // tally the remaining time into the score
      if (modeT_ > 0.6f && time_ > 0) {
        int k = time_ > 6 ? 6 : time_;
        time_ -= k; score_ += 50 * k;
      }
      if (modeT_ > 0.6f && time_ == 0 && modeT_ > 3.0f) levelDone();
    }
  }

  void levelDone() {
    EWLOGI("HOP", "clear level=%s score=%lu gems=%u coins=%d", lv_->id, (unsigned long)score_,
           gems_, coins_);
    if (score_ > best_) best_ = score_;
    gems_ = 0; checkpoint_ = false;
    if (level_ + 1 >= HOP_LEVELS) {
      mode_ = M_WIN; modeT_ = 0;
      saveBest();
      EWLOGI("HOP", "win score=%lu", (unsigned long)score_);
      return;
    }
    level_++;
    if (level_ > unlocked_) unlocked_ = level_;
    saveBest();
    enterCard();
  }

  void tickTitle(float dt) {
    modeT_ += dt;
    camX_ += 22 * dt;
    if (camX_ > lw_ * TS - W) camX_ = 0;
  }

  void tickCard(float dt) {
    modeT_ += dt;
    int16_t ax; bool ok;
    { ModelLock lk; ax = model.ax; ok = model.imuOk; }
    if (ok && modeT_ > 0.6f) { ax0_ += ax; axN_++; }
    if (modeT_ > 2.2f) startPlay();
  }

  void tickOver(float dt) { modeT_ += dt; }

  // ============================================================ drawing
  int16_t hillTop0_[W], hillTop1_[W];

  void computeHills() {
    if (theme_ != 0 && theme_ != 2) return;
    int cam = (int)camX_;
    int base = HUD + (theme_ == 2 ? 236 : 222);
    for (int x = 0; x < W; x++) {
      float w0 = cam * 0.22f + x, w1 = cam * 0.45f + x + 400;
      hillTop0_[x] = base - (int)(60 + 24 * sinf(w0 * 0.013f) + 9 * sinf(w0 * 0.051f + 1.7f));
      hillTop1_[x] = base - (int)(34 + 18 * sinf(w1 * 0.024f) + 9 * sinf(w1 * 0.051f + 1.7f));
    }
  }

  // Sky + parallax for rows clipTop..clipBot (computeHills() first).
  void drawBackground() {
    int cam = (int)camX_;
    if (theme_ == 0 || theme_ == 2) {
      uint16_t c0 = theme_ == 0 ? RGB565(140, 200, 150) : RGB565(186, 222, 214);
      uint16_t e0 = theme_ == 0 ? RGB565(110, 176, 130) : RGB565(160, 200, 200);
      uint16_t c1 = theme_ == 0 ? RGB565(70, 160, 84) : RGB565(120, 190, 140);
      uint16_t e1 = theme_ == 0 ? RGB565(40, 120, 60) : RGB565(90, 160, 120);
      for (int y = clipTop; y < clipBot; y++) {
        uint16_t sky = skyRow_[y];
        uint16_t *d = fb + y * W;
        for (int x = 0; x < W; x++) {
          int t1 = hillTop1_[x], t0 = hillTop0_[x];
          d[x] = y > t1 ? c1 : y == t1 ? e1 : y > t0 ? c0 : y == t0 ? e0 : sky;
        }
      }
    } else {
      for (int y = clipTop; y < clipBot; y++) {
        uint16_t c = skyRow_[y];
        uint16_t *d = fb + y * W;
        for (int x = 0; x < W; x++) d[x] = c;
      }
    }
    if (theme_ == 1) {
      // dim rock columns
      uint16_t col = RGB565(36, 30, 60), col2 = RGB565(26, 22, 46);
      int off = (int)(cam * 0.5f);
      for (int k = off / 80 - 1; k <= (off + W) / 80 + 1; k++) {
        int x = k * 80 - off;
        int w = 26 + (k * 7 & 15);
        fill(x, HUD, w, H - HUD, col);
        fill(x + w - 5, HUD, 5, H - HUD, col2);
        int st = 30 + (k * 13 & 31);
        fill(x + w / 2 - 3, HUD, 6, st, col2);
      }
    } else if (theme_ == 3) {
      // fort hall: pillars with lit windows
      uint16_t col = RGB565(48, 26, 34), win = RGB565(140, 70, 30);
      int off = (int)(cam * 0.4f);
      for (int k = off / 96 - 1; k <= (off + W) / 96 + 1; k++) {
        int x = k * 96 - off;
        fill(x, HUD, 30, H - HUD, col);
        fill(x + 46, HUD + 60, 14, 26, win);
        fill(x + 46, HUD + 60, 14, 3, RGB565(200, 120, 40));
      }
    }
    if (theme_ != 1 && theme_ != 3) {
      int off = (int)(cam * 0.5f);
      for (int k = off / 150 - 1; k <= (off + W) / 150 + 1; k++) {
        int x = k * 150 - off + ((k * 53) & 31);
        int y = HUD + 10 + ((k * 37) & 63);
        spr(SPR_CLOUD, x, y);
      }
    }
  }

  int artFor(uint8_t t, int tx, int ty) const {
    int fr = ((int)(animT_ * 3)) & 1;
    switch (t) {
      case T_GROUND: return tileAt(tx, ty - 1) == T_GROUND ? HOP_ART_GROUND : HOP_ART_GROUND_TOP;
      case T_BRICK: case T_BRICKC: return HOP_ART_BRICK;
      case T_QCOIN: case T_QPOW: case T_Q1UP: return fr ? HOP_ART_PRIZE2 : HOP_ART_PRIZE1;
      case T_USED: return HOP_ART_USED;
      case T_SOLID: return HOP_ART_SOLID;
      case T_LOG_TL: return HOP_ART_LOG_TL;
      case T_LOG_TR: return HOP_ART_LOG_TR;
      case T_LOG_L: return HOP_ART_LOG_L;
      case T_LOG_R: return HOP_ART_LOG_R;
      case T_COIN: return fr ? HOP_ART_COIN2 : HOP_ART_COIN1;
      case T_SEMI: return HOP_ART_SEMI;
      case T_LAVA: return fr ? HOP_ART_LAVA2 : HOP_ART_LAVA1;
      case T_POLE: return HOP_ART_POLE;
      case T_POLE_TOP: return HOP_ART_POLE_TOP;
      case T_FORT: return HOP_ART_FORT;
      case T_FORT_TOP: return HOP_ART_FORT_TOP;
      case T_DOOR: return HOP_ART_DOOR;
      case T_DOOR_TOP: return HOP_ART_DOOR_TOP;
      case T_BUSH: return HOP_ART_BUSH;
      case T_SWITCH: return HOP_ART_SWITCH;
      case T_GEM: return HOP_ART_GEM;
      case T_CHECK: return checkpoint_ ? HOP_ART_CHECK_ON : HOP_ART_CHECK;
      default: return -1;
    }
  }

  void drawTiles() {
    int cam = (int)camX_;
    int tx0 = cam / TS, tx1 = (cam + W) / TS;
    // rows overlapping the clip band (+1 above for bumped blocks rising into it)
    int ty0 = (clipTop - HUD) / TS - 1, ty1 = (clipBot - 1 - HUD) / TS + 1;
    if (ty0 < 0) ty0 = 0;
    if (ty1 > ROWS - 1) ty1 = ROWS - 1;
    for (int ty = ty0; ty <= ty1; ty++) {
      for (int tx = tx0; tx <= tx1 && tx < lw_; tx++) {
        uint8_t t = map_[ty][tx];
        if (t == T_EMPTY) continue;
        int a = artFor(t, tx, ty);
        if (a < 0) continue;
        int yoff = 0;
        for (auto &bp : bumps)
          if (bp.t >= 0 && bp.tx == tx && bp.ty == ty) yoff = -(int)(sinf(bp.t / 0.18f * 3.14159f) * 5);
        blitTile(tilePx(theme_, a), tx * TS - cam, HUD + ty * TS + yoff, tileOpaque[a]);
      }
    }
  }

  void drawEnt(const Ent &e, bool behindTiles) {
    int cam = (int)camX_;
    int x = (int)e.b.x - cam, y = HUD + (int)e.b.y;
    int fr = ((int)(e.t * 6)) & 1;
    bool knocked = e.state == S_KNOCKED;
    bool emerging = e.state == S_EMERGE;
    if (behindTiles != emerging) return;
    switch (e.type) {
      case E_LADYBUG:
        if (e.state == S_SQUASHED) spr(SPR_LADYBUG_FLAT, x - 1, y - 3);
        else spr(fr ? SPR_LADYBUG2 : SPR_LADYBUG, x - 1, y - 3, e.dir > 0, knocked);
        break;
      case E_SPIKY: spr(fr ? SPR_SPIKY2 : SPR_SPIKY, x - 1, y - 3, e.dir > 0, knocked); break;
      case E_SNAIL: spr(fr ? SPR_SNAIL2 : SPR_SNAIL, x - 1, y - 3, e.dir > 0, knocked); break;
      case E_SHELL: spr((e.val && fr) ? SPR_SHELL2 : SPR_SHELL, x - 1, y - 6, false, knocked); break;
      case E_BOSS:
        if (e.inv > 0 && ((int)(e.inv * 16) & 1)) break;
        spr(fr ? SPR_BOSS2 : SPR_BOSS, x - 2, y - 6, e.dir > 0, knocked);
        break;
      case E_BERRY:  spr(SPR_BERRY, x - 1, y - 2); break;
      case E_ONEUP:  spr(SPR_BERRY_1UP, x - 1, y - 2); break;
      case E_CHILI:  spr(SPR_CHILI, x - 1, y - 2); break;
      case E_FIREBALL: spr(SPR_FIREBALL, x, y, fr, false); break;
      case E_COINPOP: spr(fr ? SPR_COIN2 : SPR_COIN1, x, y); break;
      case E_DEBRIS: spr(SPR_DEBRIS, x, y, fr, false); break;
      case E_PLAT_H: case E_PLAT_V:
        for (int k = 0; k < 3; k++) spr(SPR_SEMI, x + k * 16, y);
        break;
      case E_FIREBAR: {
        float cx = e.ox + 8 - cam, cy = HUD + e.oy + 8;
        float a = e.t * 1.7f * e.dir;
        float ca = cosf(a), sa = sinf(a);
        for (int k = 0; k < 6; k++) spr(SPR_FIREBALL, (int)(cx + ca * k * 8) - 4, (int)(cy + sa * k * 8) - 4);
        break;
      }
      case E_FLAG: spr(SPR_FLAG, x, y); break;
      default: break;
    }
  }

  void drawPlayer() {
    if (mode_ == M_CLEAR && !bossLevelClear_) return;    // inside the fort
    if (invuln_ > 0 && ((int)(invuln_ * 20) & 1) && mode_ == M_PLAY) return;
    int cam = (int)camX_;
    int x = (int)p_.x - cam - 2, y = HUD + (int)p_.y - 2;
    bool air = !p_.onGround && mode_ != M_FLAG;
    int wf = ((int)(walkT_ * 8)) % 4;
    int frame = air ? 3 : (fabsf(p_.vx) > 5 || (mode_ == M_FLAG && modeT_ > 1.0f)) ? (wf == 1 ? 1 : wf == 3 ? 2 : 0) : 0;
    if (mode_ == M_FLAG && p_.y + p_.h < (GT - 1) * TS) frame = 3;
    int base = form_ == 0 ? SPR_FROG : form_ == 1 ? SPR_BIG : SPR_FBIG;
    if (mode_ == M_DYING) { spr(SPR_FROG, x, y, false, true); return; }
    spr((HopSpr)(base + frame), x, y, face_ < 0, false);
  }

  void drawWorldRows(bool title) {
    drawBackground();
    if (!title) for (auto &e : ents) if (e.type != E_NONE && e.type != E_SCORE) drawEnt(e, true);
    drawTiles();
    if (title) {
      float hop = fabsf(sinf(animT_ * 3.2f));
      int fy = HUD + 13 * TS - 2 - (int)(hop * 30);
      spr(hop > 0.15f ? SPR_FROG_JUMP : SPR_FROG, 208, fy);
      return;
    }
    for (auto &e : ents) if (e.type != E_NONE && e.type != E_SCORE) drawEnt(e, false);
    drawPlayer();
  }

  void drawWorld(bool title = false) {
    computeHills();
    if (!stripBuf) {                     // no SRAM strip: draw straight to the canvas
      clipTop = HUD; clipBot = H;
      drawWorldRows(title);
      return;
    }
    uint16_t *canvasFb = fb;
    for (int y0 = HUD; y0 < H; y0 += STRIP) {
      fb = stripBuf - y0 * W;
      clipTop = y0; clipBot = y0 + STRIP;
      drawWorldRows(title);
      memcpy(canvasFb + y0 * W, stripBuf, W * STRIP * 2);
    }
    fb = canvasFb;
    clipTop = HUD; clipBot = H;
  }

  void drawPopups() {
    int cam = (int)camX_;
    char buf[12];
    for (auto &e : ents) {
      if (e.type != E_SCORE) continue;
      if (e.val == 0) continue;
      if (e.val < 0) strcpy(buf, "1UP"); else snprintf(buf, sizeof(buf), "%d", e.val);
      int x = (int)e.b.x - cam, y = HUD + (int)e.b.y;
      if (x < -20 || x > W || y < HUD) continue;
      fbText(buf, x, y, 1, e.val < 0 ? C_ACCENT : C_WHITE);
    }
  }

  // HUD rows 0..HUD-1, clipped to the current strip (clipTop..clipBot).
  void drawHud() {
    int ct = clipTop, cb = clipBot;
    if (clipTop >= HUD) return;
    if (clipBot > HUD) clipBot = HUD;
    fill(0, 0, W, HUD, C_HUD);
    fill(0, HUD - 1, W, 1, RGB565(50, 46, 60));
    spr(SPR_HUD_FROG, 6, 8);
    spr(SPR_HUD_COIN, 46, 8);
    for (int i = 0; i < 3; i++) spr((gems_ & (1 << i)) ? SPR_HUD_GEM : SPR_HUD_GEM0, 86 + i * 10, 8);
    char buf[16];
    snprintf(buf, sizeof(buf), "x%d", lives_);
    fbText(buf, 16, 8, 1, C_WHITE, false);
    snprintf(buf, sizeof(buf), "x%02d", coins_);
    fbText(buf, 56, 8, 1, C_GOLD, false);
    snprintf(buf, sizeof(buf), "%07lu", (unsigned long)score_);
    fbText(buf, 120, 8, 1, C_WHITE, false);
    snprintf(buf, sizeof(buf), "%d", time_);
    fbText(buf, 204, 8, 1, time_ < 60 ? C_RED : C_WHITE, false);
    // small clock glyph
    fill(194, 9, 6, 6, C_WHITE); fill(195, 10, 4, 4, C_HUD); fill(196, 11, 1, 2, C_WHITE);
    clipTop = ct; clipBot = cb;
  }

  // Play modes: render each 16-row strip in SRAM, byte-swap it and push it
  // straight to the panel. Skips the PSRAM canvas and its per-pixel flush
  // (measured on the watch: canvas flush 49-65 ms per frame).
  bool presentDirect() {
    Arduino_DataBus *bus = displayBus();
    if (!stripBuf || !gfx || !bus) return false;
    Arduino_TFT *tft = static_cast<Arduino_TFT *>(gfx);   // displayBegin() made an ST7789
    computeHills();
    uint16_t *canvasFb = fb;
    bool inFlight = false;
    int k = 0;
    for (int y0 = 0; y0 < H; y0 += STRIP, k++) {
      int y1 = y0 + STRIP < H ? y0 + STRIP : H;
      if (lcdDma) stripBuf = stripMem[k & 1];     // the other buffer may still be sending
      fb = stripBuf - y0 * W;
      clipTop = y0; clipBot = y1;
      if (y0 < HUD) drawHud();
      if (y1 > HUD) {
        clipTop = y0 > HUD ? y0 : HUD;
        drawWorldRows(false);
        drawPopups();
        if (mode_ == M_CLEAR) drawClear();
      }
      // writePixels: bulk transfer (and ewsim's tap copies rows, not bytes);
      // it may byte-swap the strip in place, which is fine: it's redrawn.
      uint32_t n = (uint32_t)(y1 - y0) * W;
      uint32_t t0 = micros();
      if (lcdDma) {
        ewsimTapRows(y0, y1 - y0, stripBuf);       // keep ewt snapshots honest
        for (uint32_t i = 0; i < n; i++) stripBuf[i] = __builtin_bswap16(stripBuf[i]);
        if (inFlight) { if (!hopLcdDmaWait()) lcdDma = false; tft->endWrite(); inFlight = false; }
        tft->startWrite();
        tft->writeAddrWindow(0, y0, W, y1 - y0);
        if (lcdDma && hopLcdDmaSend(stripBuf, n * 2)) inFlight = true;
        else { tft->endWrite(); }
      } else {
        tft->startWrite();
        tft->writeAddrWindow(0, y0, W, y1 - y0);
        bus->writePixels(stripBuf, n);
        tft->endWrite();
      }
      pushUs_ += micros() - t0;
    }
    if (inFlight) {
      uint32_t t0 = micros();
      if (!hopLcdDmaWait()) lcdDma = false;
      tft->endWrite();
      pushUs_ += micros() - t0;
    }
    stripBuf = stripMem[0];
    fb = canvasFb;
    clipTop = HUD; clipBot = H;
    return true;
  }

  void drawTitle(Arduino_GFX *g) {
    // the meadow scrolls behind the menu; the hero hops on the spot
    drawWorld(true);
    clipTop = 0;
    fill(0, 0, W, HUD, skyRow_[0]);
    dim(130, 262);

    textC(g, "HOPPER", 34, 5, C_ACCENT);
    textC(g, "a frog's leap adventure", 80, 1, C_WHITE);
    char buf[32];
    snprintf(buf, sizeof(buf), "BEST %07lu", (unsigned long)best_);
    textC(g, buf, 98, 1, C_GOLD);

    // PLAY
    g->fillRoundRect(50, 148, 140, 36, 10, C_ACCENT);
    g->drawRoundRect(50, 148, 140, 36, 10, C_BLACK);
    textC(g, "PLAY", 158, 2, C_WHITE);
    if (unlocked_ > 0) {
      snprintf(buf, sizeof(buf), "CONTINUE %s", HOP_LEVEL[unlocked_].id);
      g->fillRoundRect(40, 192, 160, 28, 8, RGB565(60, 56, 80));
      textC(g, buf, 202, 1, C_WHITE);
    }
    g->drawRoundRect(44, 230, 152, 24, 8, C_WHITE);
    textC(g, autoRun_ ? "CONTROL: AUTO-RUN" : "CONTROL: TILT", 238, 1, C_WHITE);
    textC(g, autoRun_ ? "tap = jump, hold = higher" : "tilt = run, tap = jump", 264, 1, RGB565(200, 200, 210));
    clipTop = HUD;
  }

  void drawCard(Arduino_GFX *g) {
    clipTop = 0;
    for (int i = 0; i < W * H; i++) fb[i] = C_BLACK;
    char buf[32];
    snprintf(buf, sizeof(buf), "WORLD %s", lv_->id);
    textC(g, buf, 80, 3, C_WHITE);
    textC(g, lv_->title, 118, 2, C_ACCENT);
    spr(SPR_FROG, 92, 156);
    snprintf(buf, sizeof(buf), "x %d", lives_);
    text(g, buf, 116, 160, 2, C_WHITE);
    if (!autoRun_) textC(g, "hold the watch level...", 214, 1, RGB565(160, 160, 170));
    textC(g, "tap to start", 236, 1, RGB565(160, 160, 170));
    clipTop = HUD;
  }

  void drawPause(Arduino_GFX *g) {
    clipTop = 0;
    dim(HUD, H);
    textC(g, "PAUSED", 70, 3, C_WHITE);
    g->fillRoundRect(50, 114, 140, 36, 10, C_ACCENT);
    textC(g, "RESUME", 124, 2, C_WHITE);
    g->fillRoundRect(50, 166, 140, 36, 10, RGB565(120, 40, 40));
    textC(g, "QUIT", 176, 2, C_WHITE);
    char buf[24];
    snprintf(buf, sizeof(buf), "gems %d/3", __builtin_popcount(gems_));
    textC(g, buf, 220, 1, C_GOLD);
    clipTop = HUD;
  }

  void drawClear() {
    if (modeT_ < 0.4f) return;
    fbTextC(bossLevelClear_ ? "FORT CLEARED!" : "COURSE CLEAR!", 90, 2, C_GOLD);
    char buf[24];
    snprintf(buf, sizeof(buf), "gems %d/3", __builtin_popcount(gems_));
    fbTextC(buf, 118, 1, C_WHITE);
  }

  void drawOver(Arduino_GFX *g) {
    clipTop = 0;
    for (int i = 0; i < W * H; i++) fb[i] = C_BLACK;
    textC(g, "GAME OVER", 100, 3, C_RED);
    char buf[32];
    snprintf(buf, sizeof(buf), "SCORE %07lu", (unsigned long)score_);
    textC(g, buf, 150, 1, C_WHITE);
    snprintf(buf, sizeof(buf), "BEST  %07lu", (unsigned long)best_);
    textC(g, buf, 166, 1, C_GOLD);
    if (modeT_ > 1.0f && ((int)(animT_ * 2) & 1)) textC(g, "tap to continue", 220, 1, C_WHITE);
    clipTop = HUD;
  }

  void drawWin(Arduino_GFX *g) {
    clipTop = 0;
    for (int y = 0; y < H; y++) { uint16_t c = lerp565(RGB565(20, 40, 30), RGB565(10, 10, 20), y, H); for (int x = 0; x < W; x++) fb[y * W + x] = c; }
    textC(g, "YOU WIN!", 60, 4, C_GOLD);
    textC(g, "the fort has fallen", 106, 1, C_WHITE);
    float hop = fabsf(sinf(animT_ * 4));
    spr(SPR_FBIG_JUMP, 112, 150 - (int)(hop * 22));
    char buf[32];
    snprintf(buf, sizeof(buf), "SCORE %07lu", (unsigned long)score_);
    textC(g, buf, 180, 2, C_WHITE);
    if (modeT_ > 1.0f && ((int)(animT_ * 2) & 1)) textC(g, "tap to continue", 236, 1, C_WHITE);
    clipTop = HUD;
  }
};

static HopperView gHopper;

}  // namespace

View *hopperViewPtr() { return &gHopper; }

// Console (ewlog): hop status | hop level N | hop warp COL [ROW] | hop form 0-2 | hop god 0|1
bool hopperConsole(const char *line) {
  if (strncmp(line, "hop", 3) != 0 || (line[3] != ' ' && line[3] != 0)) return false;
  const char *a = line + 3;
  while (*a == ' ') a++;
  if (strncmp(a, "level ", 6) == 0)      reqLevel = atoi(a + 6);
  else if (strncmp(a, "warp ", 5) == 0) {
    char *end = nullptr;
    long col = strtol(a + 5, &end, 10);
    reqWarpRow = (end && *end) ? atoi(end) : -1;
    reqWarp = (int)col;
  }
  else if (strncmp(a, "form ", 5) == 0)  reqForm = atoi(a + 5);
  else if (strncmp(a, "god ", 4) == 0)   godMode = atoi(a + 4) != 0;
  else if (strncmp(a, "status", 6) == 0) reqStatus = true;
  else { Serial.println("usage: hop status | level N | warp COL [ROW] | form 0-2 | god 0|1"); return true; }
  Serial.println("hop ok");
  return true;
}
