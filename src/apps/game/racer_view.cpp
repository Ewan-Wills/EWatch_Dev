// RacerView — pseudo-3D, OutRun-style racer.
//
// A steerable car on a scrolling road that curves toward the horizon, with
// traffic to overtake and a crash-or-survive game loop.
//
// Rendering is the classic projected-road trick: for each scanline below the
// horizon we invert the perspective to a world depth, scale the road width by
// it, and pick stripe/rumble colours from (playerZ + depth) so the bands rush
// toward the camera as the player advances — that motion IS the speed. Curves
// bend the far end of the road (a perspective-squared offset) and shove the
// player outward (centrifugal); the player tilts to follow. Traffic cars are
// the same projection applied to point sprites, drawn far-to-near.
//
// The whole frame is drawn into a 240x280 PSRAM canvas (frameCanvas()) and
// blitted once per frame for tear-free motion. Steering is watch tilt
// (MMA8451, sampled into model.ax by taskIO); a held finger boosts.
//
// This file also owns the tiny view-framework glue (currentView / viewsInit /
// switchTo / viewFor) the controller's render task drives. New game modules
// grow under src/apps/game/.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "view.h"
#include "model.h"
#include "event.h"
#include "display.h"
#include "haptic.h"
#include "storage.h"

// ---- Brand palette (spec: #0E0D0B sky-to-road, #E86B2B car/UI accent) ----
// RGB565(r,g,b) comes from Arduino_GFX_Library.
static const uint16_t COL_SKY      = RGB565(0x0E, 0x0D, 0x0B);
static const uint16_t COL_SKY_MID  = RGB565(0x2A, 0x20, 0x18);
static const uint16_t COL_HORIZON  = RGB565(0x6A, 0x3A, 0x1C);   // warm glow
static const uint16_t COL_GRASS_D  = RGB565(0x0E, 0x24, 0x0E);
static const uint16_t COL_GRASS_L  = RGB565(0x15, 0x31, 0x15);
static const uint16_t COL_ROAD_D   = RGB565(0x2C, 0x2C, 0x30);
static const uint16_t COL_ROAD_L   = RGB565(0x34, 0x34, 0x3A);
static const uint16_t COL_RUMBLE_L = RGB565(0xEE, 0xEE, 0xEE);
static const uint16_t COL_RUMBLE_R = RGB565(0xD8, 0x3A, 0x20);
static const uint16_t COL_LINE     = RGB565(0xEE, 0xEC, 0xC8);
static const uint16_t COL_ACCENT   = RGB565(0xE8, 0x6B, 0x2B);
static const uint16_t COL_ACCENT_D = RGB565(0xA0, 0x44, 0x18);
static const uint16_t COL_TEXT     = RGB565(0xFF, 0xFF, 0xFF);
static const uint16_t COL_SHADOW   = RGB565(0x05, 0x05, 0x06);
static const uint16_t COL_CRASH    = RGB565(0x90, 0x10, 0x08);
static const uint16_t COL_GLASS    = RGB565(0x12, 0x16, 0x1C);
static const uint16_t COL_TAIL     = RGB565(0xE0, 0x30, 0x18);

// Traffic body colours — visibly "other cars", never the player's accent.
static const uint16_t TRAFFIC_COLS[] = {
  RGB565(0x3C, 0x6E, 0xC8),   // blue
  RGB565(0x2F, 0xA8, 0x9C),   // teal
  RGB565(0xB0, 0xB4, 0xBC),   // silver
  RGB565(0x8A, 0x55, 0xC0),   // purple
  RGB565(0x5C, 0xB0, 0x4C),   // green
  RGB565(0xC8, 0xC0, 0x40),   // yellow
};
static const int TRAFFIC_NCOL = sizeof(TRAFFIC_COLS) / sizeof(TRAFFIC_COLS[0]);

// ---- Geometry ----
static constexpr int   W       = 240;
static constexpr int   H       = 280;
static constexpr int   HORIZON = 100;                 // sky 0..HORIZON, road below
static constexpr float ROWS    = (float)(H - HORIZON);

// ---- Road projection / look (tunable) ----
static constexpr float ROAD_SCALE     = 6000.0f;   // depth recession constant
static constexpr float STRIPE_LEN     = 220.0f;    // world units per colour band
static constexpr float ROAD_HALF_BASE = 150.0f;    // road half-width (px) at bottom
static constexpr float RUMBLE_FRAC    = 0.14f;     // edge strip width / half-width
static constexpr float CENTER_FRAC    = 0.045f;    // centre dash width / half-width
static constexpr float CURVE_AMPL     = 150.0f;    // max horizon bend (px)

// ---- Driving model (tunable) ----
static constexpr float START_SPEED    = 2400.0f;   // gentle opening cruise
static constexpr float MAX_SPEED      = 9000.0f;   // eventual cap (reached via ramp)
static constexpr float RAMP_DIST_M    = 2500.0f;   // metres to ramp START_SPEED -> MAX_SPEED
static constexpr float ACCEL          = 4200.0f;   // units / sec^2
static constexpr float OFFROAD_MAX    = 3200.0f;   // speed cap on grass
static constexpr float OFFROAD_DECEL  = 12000.0f;  // braking force on grass
static constexpr float BOOST_MULT     = 1.35f;     // top-speed multiplier while boosting
static constexpr float IDLE_SPEED     = 1600.0f;   // menu road scroll
static constexpr float DIST_PER_M     = 100.0f;    // world units per scored metre
static constexpr float SPEED_KMH      = 3.6f / DIST_PER_M;
static constexpr float CENTRIFUGAL    = 0.85f;     // curve push on the player

// ---- Steering (tunable) ----
static constexpr float STEER_SIGN     = -1.0f;     // tilt direction (flipped)
static constexpr float STEER_FULL     = 2600.0f;   // accel raw (~0.63g) = full lock
static constexpr float STEER_DEADZONE = 0.08f;     // ignore small resting tilt
static constexpr float STEER_SMOOTH   = 9.0f;      // input low-pass (per sec)
static constexpr float STEER_RATE     = 2.2f;      // lateral units/sec at full lock
static constexpr float PLAYERX_LIMIT  = 2.2f;      // hard limit out onto the grass

// ---- Traffic / collision (tunable) ----
static constexpr int   NUM_TRAFFIC    = 6;
static constexpr float SPAWN_MIN      = 4000.0f;   // world units ahead
static constexpr float SPAWN_MAX      = 16000.0f;
static constexpr float RENDER_Z       = 16000.0f;  // farthest car we draw
static constexpr float DESPAWN_BEHIND = 350.0f;    // recycle once this far behind
static constexpr float TRAFFIC_VMIN   = 2200.0f;   // their cruising speed range
static constexpr float TRAFFIC_VMAX   = 5200.0f;
static constexpr float CRASH_Z        = 46.0f;     // longitudinal hit window
static constexpr float CRASH_X        = 0.34f;     // lateral hit window (road = ±1)

static const float LANES[] = { -0.62f, -0.22f, 0.22f, 0.62f };
static const int   NLANES  = sizeof(LANES) / sizeof(LANES[0]);

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float frand(float a, float b) {
  return a + (b - a) * (random(1001) / 1000.0f);
}

// Clipped horizontal span into the canvas.
static inline void hline(Arduino_GFX *g, int x, int y, int w, uint16_t c) {
  if (w <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > W) w = W - x;
  if (w <= 0) return;
  g->drawFastHLine(x, y, w, c);
}

struct TrafficCar {
  float    z;        // absolute world distance
  float    lane;     // lateral position, road = ±1
  float    speed;    // world units / sec
  uint16_t col;
};

class RacerView : public View {
public:
  void onEnter() override { enterStart(); }

  void onEvent(const Event &e) override {
    switch (e.type) {
      case EventType::Touch:
        if      (state_ == State::Start)    enterPlay();
        else if (state_ == State::GameOver) enterPlay();   // tap to retry
        else { boosting_ = true; lastTouchMs_ = millis(); }
        break;
      case EventType::TouchHold:
        if (state_ == State::Play) { boosting_ = true; lastTouchMs_ = millis(); }
        break;
      case EventType::TouchUp:
        boosting_ = false;
        break;
      case EventType::ButtonShort:
        if      (state_ == State::Start)    enterPlay();
        else if (state_ == State::GameOver) enterStart();
        else                                endRun();        // give up mid-run
        break;
      default:
        break;
    }
  }

  void render() override {
    Arduino_Canvas *canvas = frameCanvas();
    Arduino_GFX *g = canvas ? (Arduino_GFX *)canvas : gfx;
    if (!g) return;

    uint32_t now = millis();
    float dt = (now - lastFrameMs_) / 1000.0f;
    lastFrameMs_ = now;
    if (dt > 0.05f) dt = 0.05f;          // clamp after a stall (e.g. NVS write)
    if (dt < 0.0f)  dt = 0.0f;

    switch (state_) {
      case State::Start:
        playerZ_ += IDLE_SPEED * dt;
        currentCurve_ = curveAt(playerZ_);
        drawScene(g);
        drawStartUi(g, now);
        break;

      case State::Play:
        currentCurve_ = curveAt(playerZ_);
        update(dt, now);
        if (state_ == State::Play) { drawScene(g); drawHud(g); break; }
        [[fallthrough]];               // crashed this frame -> draw game over

      case State::GameOver:
        if (now - crashFlashMs_ < 160) g->fillScreen(COL_CRASH);
        else                           drawScene(g);
        drawGameOverUi(g, now);
        break;
    }

    if (canvas) canvas->flush();          // single tear-free blit
  }

private:
  enum class State { Start, Play, GameOver };
  State    state_        = State::Start;
  float    playerX_      = 0.0f;          // -1..1 = road edges; beyond = grass
  float    playerZ_      = 0.0f;          // forward distance (world units)
  float    speed_        = 0.0f;          // world units / sec
  float    steer_        = 0.0f;          // smoothed steer input -1..1
  float    currentCurve_ = 0.0f;          // road bend at the player, -1..1
  bool     boosting_     = false;
  uint32_t runDist_      = 0;             // metres travelled this run
  uint32_t overtakes_    = 0;             // cars passed this run
  uint32_t lastTouchMs_  = 0;
  uint32_t lastFrameMs_  = 0;
  uint32_t lastRumbleMs_ = 0;
  uint32_t lastSaveMs_   = 0;
  uint32_t crashFlashMs_ = 0;
  TrafficCar traffic_[NUM_TRAFFIC];

  // ---- procedural road shape ----
  // Continuous function of distance -> bend (-1..1). Smooth long sweepers with
  // a shorter overlay so the road never feels repetitive.
  static float curveAt(float z) {
    return 0.7f * sinf(z * 0.00010f) + 0.3f * sinf(z * 0.00027f + 1.3f);
  }

  // ---- projection helpers (shared by road + sprites) ----
  float halfWAt(float rowDepth) const { return ROAD_HALF_BASE * rowDepth / ROWS; }
  float bendAt(float rowDepth)  const {
    float n = (ROWS - rowDepth) / ROWS;          // ~1 at horizon, 0 at the car
    return currentCurve_ * n * n * CURVE_AMPL;
  }
  float centerAt(float rowDepth) const {
    return W * 0.5f - playerX_ * halfWAt(rowDepth) + bendAt(rowDepth);
  }

  // ---- state transitions ----
  void enterStart() {
    state_ = State::Start;
    boosting_ = false;
    { ModelLock lk; model.sleepTimeoutSec = 20; }   // idle-sleep on the menu
    lastFrameMs_ = millis();
  }

  void enterPlay() {
    state_ = State::Play;
    playerX_ = 0; speed_ = 0; steer_ = 0;
    boosting_ = false; runDist_ = 0; overtakes_ = 0;
    // keep advancing playerZ_ so the road shape carries over from the menu
    for (int i = 0; i < NUM_TRAFFIC; i++) spawnCar(i, playerZ_ + i * 2200.0f);
    { ModelLock lk; model.sleepTimeoutSec = 0; }     // never sleep while driving
    lastFrameMs_ = millis();
    hapticBuzz(220, 70);
  }

  void endRun() {
    commitBest(/*force=*/true);
    enterStart();
  }

  void crash() {
    commitBest(/*force=*/true);
    state_ = State::GameOver;
    crashFlashMs_ = millis();
    boosting_ = false;
    speed_ = 0;
    hapticBuzz(255, 280);             // full-strength crash jolt
    { ModelLock lk; model.sleepTimeoutSec = 20; }
  }

  void spawnCar(int i, float baseZ) {
    traffic_[i].z     = baseZ + frand(SPAWN_MIN, SPAWN_MAX);
    traffic_[i].lane  = LANES[random(NLANES)];
    traffic_[i].speed = frand(TRAFFIC_VMIN, TRAFFIC_VMAX);
    traffic_[i].col   = TRAFFIC_COLS[random(TRAFFIC_NCOL)];
  }

  // Keep the in-RAM best fresh and throttle NVS writes (flash wear) to at most
  // once every 5 s — unless forced (end of run / crash).
  void commitBest(bool force) {
    { ModelLock lk;
      if (runDist_ > model.bestDistance) model.bestDistance = runDist_; }
    uint32_t now = millis();
    if (force || now - lastSaveMs_ > 5000) { Storage::save(); lastSaveMs_ = now; }
  }

  // ---- simulation ----
  void update(float dt, uint32_t now) {
    // steering from tilt
    int16_t ax; bool ok;
    { ModelLock lk; ax = model.ax; ok = model.imuOk; }
    float target = ok ? clampf((STEER_SIGN * ax) / STEER_FULL, -1.0f, 1.0f) : 0.0f;
    if (fabsf(target) < STEER_DEADZONE) target = 0.0f;
    steer_ += (target - steer_) * clampf(STEER_SMOOTH * dt, 0.0f, 1.0f);

    // boost (held finger); fail safe if a TouchUp was missed
    if (boosting_ && (now - lastTouchMs_) > 140) boosting_ = false;

    bool offRoad = fabsf(playerX_) > 1.0f;

    // longitudinal speed. The cruise cap starts low and ramps up with distance
    // travelled, so you open slowly and the road gets faster the further you
    // survive — that ramp is the difficulty curve.
    float ramp   = clampf(playerZ_ / (RAMP_DIST_M * DIST_PER_M), 0.0f, 1.0f);
    float cruise = START_SPEED + (MAX_SPEED - START_SPEED) * ramp;
    float maxv   = cruise * (boosting_ ? BOOST_MULT : 1.0f);
    if (offRoad && maxv > OFFROAD_MAX) maxv = OFFROAD_MAX;
    if (speed_ < maxv) speed_ += ACCEL * dt;
    else               speed_ -= (offRoad ? OFFROAD_DECEL : ACCEL) * dt;
    speed_ = clampf(speed_, 0.0f, MAX_SPEED * BOOST_MULT);

    playerZ_ += speed_ * dt;
    runDist_  = (uint32_t)(playerZ_ / DIST_PER_M);

    // lateral: tilt steers, the curve shoves you to the outside
    float speedFrac = speed_ / MAX_SPEED;
    playerX_ += steer_ * STEER_RATE * dt * (0.5f + 0.5f * speedFrac);
    playerX_ += -currentCurve_ * speedFrac * CENTRIFUGAL * dt;
    playerX_  = clampf(playerX_, -PLAYERX_LIMIT, PLAYERX_LIMIT);

    // off-road rumble
    if (offRoad && speed_ > 250.0f && (now - lastRumbleMs_) > 110) {
      lastRumbleMs_ = now;
      hapticBuzz(90, 35);
    }

    // traffic: advance, recycle, and test for collisions
    for (int i = 0; i < NUM_TRAFFIC; i++) {
      traffic_[i].z += traffic_[i].speed * dt;
      float relZ = traffic_[i].z - playerZ_;
      if (relZ < -DESPAWN_BEHIND) {        // overtaken and gone
        overtakes_++;
        spawnCar(i, playerZ_);
        continue;
      }
      if (relZ > 0 && relZ < CRASH_Z &&
          fabsf(playerX_ - traffic_[i].lane) < CRASH_X) {
        crash();
        return;                            // stop simulating this frame
      }
    }

    commitBest(/*force=*/false);
  }

  // ---- the projected road ----
  void drawScene(Arduino_GFX *g) {
    // Sky: dark base, a warmer mid band, and a glow line at the horizon.
    g->fillRect(0, 0, W, HORIZON, COL_SKY);
    g->fillRect(0, HORIZON - 26, W, 26, COL_SKY_MID);
    g->fillRect(0, HORIZON - 6,  W, 6,  COL_HORIZON);

    for (int y = HORIZON; y < H; y++) {
      float rowDepth = (float)(y - HORIZON) + 0.0001f;   // 0 at horizon
      float worldZ   = playerZ_ + ROAD_SCALE / rowDepth; // far rows = larger Z
      bool  s        = ((long)(worldZ / STRIPE_LEN)) & 1L;

      g->drawFastHLine(0, y, W, s ? COL_GRASS_L : COL_GRASS_D);   // grass

      float halfW = halfWAt(rowDepth);
      float cx    = centerAt(rowDepth);
      int   x0    = (int)(cx - halfW);
      int   x1    = (int)(cx + halfW);
      hline(g, x0, y, x1 - x0, s ? COL_ROAD_L : COL_ROAD_D);      // tarmac

      int rw = (int)(halfW * RUMBLE_FRAC);
      if (rw < 2) rw = 2;
      uint16_t rum = s ? COL_RUMBLE_L : COL_RUMBLE_R;
      hline(g, x0, y, rw, rum);                                   // rumble L
      hline(g, x1 - rw, y, rw, rum);                             // rumble R

      if (s) {                                                    // centre dash
        int cw = (int)(halfW * CENTER_FRAC);
        if (cw < 1) cw = 1;
        hline(g, (int)cx - cw, y, 2 * cw, COL_LINE);
      }
    }

    drawTraffic(g);

    // player car: fixed near the bottom, leaning slightly into the steer
    drawPlayerCar(g, W / 2 + (int)(steer_ * 10.0f), H - 26);
  }

  // Project every visible traffic car, then paint far-to-near (painter's order).
  void drawTraffic(Arduino_GFX *g) {
    struct Vis { float relZ; int cx, y; float scale; uint16_t col; };
    Vis vis[NUM_TRAFFIC];
    int n = 0;
    for (int i = 0; i < NUM_TRAFFIC; i++) {
      float relZ = traffic_[i].z - playerZ_;
      if (relZ < 20.0f || relZ > RENDER_Z) continue;
      float rowDepth = ROAD_SCALE / relZ;
      rowDepth = clampf(rowDepth, 1.0f, ROWS);
      float scale = rowDepth / ROWS;
      vis[n].relZ  = relZ;
      vis[n].cx    = (int)(centerAt(rowDepth) + traffic_[i].lane * halfWAt(rowDepth));
      vis[n].y     = (int)(HORIZON + rowDepth);
      vis[n].scale = scale;
      vis[n].col   = traffic_[i].col;
      n++;
    }
    // insertion sort: farthest (largest relZ) first
    for (int a = 1; a < n; a++) {
      Vis k = vis[a];
      int b = a - 1;
      while (b >= 0 && vis[b].relZ < k.relZ) { vis[b + 1] = vis[b]; b--; }
      vis[b + 1] = k;
    }
    for (int k = 0; k < n; k++)
      drawTrafficCar(g, vis[k].cx, vis[k].y, vis[k].scale, vis[k].col);
  }

  void drawTrafficCar(Arduino_GFX *g, int cx, int baseY, float scale, uint16_t col) {
    int bw = (int)(48 * scale); if (bw < 3) bw = 3;
    int bh = (int)(30 * scale); if (bh < 2) bh = 2;
    int x = cx - bw / 2;
    int y = baseY - bh;
    int sh = (int)(5 * scale); if (sh < 1) sh = 1;
    g->fillRect(cx - bw / 2, baseY - sh, bw, sh, COL_SHADOW);     // shadow
    if (bw >= 8 && bh >= 8) g->fillRoundRect(x, y, bw, bh, 3, col);
    else                    g->fillRect(x, y, bw, bh, col);       // body
    int cw = (int)(bw * 0.6f), ch = (int)(bh * 0.4f);
    if (cw > 2 && ch > 2) g->fillRect(cx - cw / 2, y + 2, cw, ch, COL_GLASS); // window
    if (scale > 0.30f) {                                          // tail lights
      int lw = (int)(7 * scale), lh = (int)(5 * scale);
      if (lw < 2) lw = 2; if (lh < 1) lh = 1;
      g->fillRect(x + 2, y + bh - lh - 1, lw, lh, COL_TAIL);
      g->fillRect(x + bw - lw - 2, y + bh - lh - 1, lw, lh, COL_TAIL);
    }
  }

  // Chunky back-of-car sprite in the brand accent.
  void drawPlayerCar(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 30, cy + 8,  60, 8,  4, COL_SHADOW);     // shadow
    g->fillRoundRect(cx - 32, cy - 4,  12, 18, 3, RGB565(0x12,0x12,0x12)); // L wheel
    g->fillRoundRect(cx + 20, cy - 4,  12, 18, 3, RGB565(0x12,0x12,0x12)); // R wheel
    g->fillRect     (cx - 28, cy - 18, 56, 4,     COL_ACCENT_D);   // spoiler
    g->fillRoundRect(cx - 26, cy - 16, 52, 26, 6, COL_ACCENT);     // body
    g->fillRoundRect(cx - 26, cy - 2,  52, 12, 4, COL_ACCENT_D);   // lower body
    g->fillRoundRect(cx - 16, cy - 22, 32, 12, 4, COL_ACCENT_D);   // cabin
    g->fillRoundRect(cx - 13, cy - 20, 26, 8,  3, COL_GLASS);      // glass
    g->fillRect     (cx - 24, cy - 7,  7,  5,     COL_TAIL);       // L light
    g->fillRect     (cx + 17, cy - 7,  7,  5,     COL_TAIL);       // R light
  }

  // ---- UI ----
  void drawStartUi(Arduino_GFX *g, uint32_t now) {
    uint32_t best; { ModelLock lk; best = model.bestDistance; }

    g->setTextColor(COL_ACCENT);
    g->setTextSize(3);
    g->setCursor(28, 26);
    g->print("3D RACER");

    char buf[24];
    g->setTextColor(COL_TEXT);
    g->setTextSize(2);
    snprintf(buf, sizeof(buf), "BEST %lu m", (unsigned long)best);
    g->setCursor(centerX(strlen(buf), 2), 150);
    g->print(buf);

    if ((now / 500) & 1) {                 // blink
      g->setTextColor(COL_ACCENT);
      g->setTextSize(2);
      const char *t = "TAP TO START";
      g->setCursor(centerX(strlen(t), 2), 196);
      g->print(t);
    }

    g->setTextColor(COL_TEXT);
    g->setTextSize(1);
    const char *h = "tilt to steer  -  hold to boost";
    g->setCursor(centerX(strlen(h), 1), 228);
    g->print(h);
  }

  void drawHud(Arduino_GFX *g) {
    char buf[20];

    // distance (top-left)
    g->setTextColor(COL_TEXT);
    g->setTextSize(2);
    snprintf(buf, sizeof(buf), "%lu m", (unsigned long)runDist_);
    g->setCursor(6, 6);
    g->print(buf);

    // speed in km/h (top-right)
    int kmh = (int)(speed_ * SPEED_KMH + 0.5f);
    snprintf(buf, sizeof(buf), "%d", kmh);
    int w = (int)strlen(buf) * 12;        // size-2 char ~12 px
    g->setTextColor(boosting_ ? COL_ACCENT : COL_TEXT);
    g->setCursor(W - 6 - w, 6);
    g->print(buf);
    g->setTextColor(COL_TEXT);
    g->setTextSize(1);
    g->setCursor(W - 6 - 24, 24);
    g->print("KM/H");

    // cars passed (below distance)
    g->setTextColor(COL_TEXT);
    g->setTextSize(1);
    snprintf(buf, sizeof(buf), "PASSED %lu", (unsigned long)overtakes_);
    g->setCursor(6, 26);
    g->print(buf);

    if (boosting_) {
      g->setTextColor(COL_ACCENT);
      g->setTextSize(2);
      const char *b = "BOOST";
      g->setCursor(centerX(strlen(b), 2), H - 24);
      g->print(b);
    }
  }

  void drawGameOverUi(Arduino_GFX *g, uint32_t now) {
    uint32_t best; { ModelLock lk; best = model.bestDistance; }

    g->fillRoundRect(20, 64, 200, 150, 10, RGB565(0x14, 0x09, 0x06));
    g->drawRoundRect(20, 64, 200, 150, 10, COL_ACCENT);

    g->setTextColor(COL_ACCENT);
    g->setTextSize(3);
    const char *t = "CRASHED";
    g->setCursor(centerX(strlen(t), 3), 80);
    g->print(t);

    char buf[24];
    g->setTextColor(COL_TEXT);
    g->setTextSize(2);
    snprintf(buf, sizeof(buf), "%lu m", (unsigned long)runDist_);
    g->setCursor(centerX(strlen(buf), 2), 118);
    g->print(buf);

    g->setTextSize(1);
    snprintf(buf, sizeof(buf), "passed %lu  -  best %lu m",
             (unsigned long)overtakes_, (unsigned long)best);
    g->setCursor(centerX(strlen(buf), 1), 148);
    g->print(buf);

    if ((now / 500) & 1) {
      g->setTextColor(COL_ACCENT);
      g->setTextSize(2);
      const char *r = "TAP TO RETRY";
      g->setCursor(centerX(strlen(r), 2), 178);
      g->print(r);
    }
  }

  static int centerX(size_t len, uint8_t size) {
    int w = (int)len * 6 * size;          // GFX base glyph is 6 px wide
    int x = (W - w) / 2;
    return x < 0 ? 0 : x;
  }
};

// ---------- view framework glue ----------
View *currentView = nullptr;
static RacerView gRacerView;

View *viewFor(Screen s) {
  switch (s) {
    case Screen::Racer: return &gRacerView;
  }
  return &gRacerView;
}

void switchTo(Screen s) {
  View *next = viewFor(s);
  if (next == currentView) return;
  if (currentView) currentView->onExit();
  { ModelLock lk; model.screen = s; model.revision++; }
  currentView = next;
  if (currentView) currentView->onEnter();
}

void viewsInit() {
  currentView = viewFor(Screen::Racer);
  if (currentView) currentView->onEnter();
}
