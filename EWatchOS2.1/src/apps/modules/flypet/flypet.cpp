#include <Arduino_GFX_Library.h>
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include "flypet.h"
#include "display.h"     // gfx, frameCanvas, backlightSet
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model + ModelLock
#include "view.h"        // theme(), switchTo(), contrastFor()
#include "ewlog.h"       // ewlogConsolePause (TRAP A: console steals Serial bytes)

// ===========================================================================
// Layout (canvas is 240 x 280)
// ===========================================================================
static const int16_t W = 240, H = 280;
static const int16_t HUD_Y   = 2;      // top stat bars
static const int16_t ARENA_X0 = 6,  ARENA_Y0 = 46;   // the fly's playpen
static const int16_t ARENA_X1 = 234, ARENA_Y1 = 236;
static const int16_t STATUS_Y = 250;   // status / connection line

static inline int16_t arenaW() { return ARENA_X1 - ARENA_X0; }
static inline int16_t arenaH() { return ARENA_Y1 - ARENA_Y0; }

// normalised 0..100 arena coords -> pixels
static inline float nx2px(float x) { return ARENA_X0 + (x / 100.0f) * arenaW(); }
static inline float ny2px(float y) { return ARENA_Y0 + (y / 100.0f) * arenaH(); }
static inline float px2nx(float px) { return (px - ARENA_X0) / arenaW() * 100.0f; }
static inline float py2ny(float py) { return (py - ARENA_Y0) / arenaH() * 100.0f; }

// ===========================================================================
// Palette (RGB565) — the fly keeps its own identity whatever the watch theme
// ===========================================================================
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static uint16_t blend(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  float ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  float br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint8_t r = (uint8_t)(ar + (br - ar) * t) << 3;
  uint8_t g = (uint8_t)(ag + (bg - ag) * t) << 2;
  uint8_t bl= (uint8_t)(ab + (bb - ab) * t) << 3;
  return rgb(r, g, bl);
}
static const uint16_t BODY_DK  = rgb(28, 30, 34);    // near-black carapace
static const uint16_t THORAX   = rgb(60, 72, 60);    // faint green iridescence
static const uint16_t ABDOMEN  = rgb(44, 44, 40);
static const uint16_t EYE_RED  = rgb(176, 42, 28);   // compound eye
static const uint16_t WING     = rgb(206, 214, 226); // translucent wing
static const uint16_t LEGCOL   = rgb(20, 20, 22);
static const uint16_t FOODCOL  = rgb(225, 180, 60);  // a sugar crumb

static const uint16_t STAT_HI = rgb(90, 200, 100);
static const uint16_t STAT_MD = rgb(230, 170, 40);
static const uint16_t STAT_LO = rgb(220, 60, 50);
static uint16_t statColor(uint8_t v) { return v > 50 ? STAT_HI : (v > 25 ? STAT_MD : STAT_LO); }

// ===========================================================================
// Behaviour / state
// ===========================================================================
enum class FlyAction : uint8_t { Wander, Walk, Groom, Rest, Feed };
static const char *actionName(FlyAction a) {
  switch (a) {
    case FlyAction::Walk:  return "walking";
    case FlyAction::Groom: return "grooming";
    case FlyAction::Rest:  return "resting";
    case FlyAction::Feed:  return "feeding";
    default:               return "wandering";
  }
}

struct FlyState {
  float x = 50, y = 55;          // position (arena 0..100)
  float tx = 50, ty = 55;        // target
  float heading = 0;             // radians, facing direction
  FlyAction action = FlyAction::Wander;
  uint8_t hunger = 35;           // 0 full .. 100 starving
  uint8_t energy = 80;           // 100 full .. 0 exhausted
  uint8_t mood   = 70;           // 100 happy .. 0 miserable
  bool  hasFood = false;
  float foodX = 0, foodY = 0;
};
static FlyState fly;

// PC link
static uint32_t gLastPcMs = 0;             // last valid FLY command
static bool     gPcDrivingLast = false;
static char     gStateLabel[20] = "";      // optional label from PC

// autonomous timers
static uint32_t gActionUntil = 0;          // when current autonomous action ends
static uint32_t gLastDecayMs = 0;          // stat-decay clock
static uint32_t gLastStatTxMs = 0;         // telemetry throttle

// transient on-screen message
static char     gMsg[22] = "";
static uint32_t gMsgUntil = 0;
static void setMsg(const char *m, uint32_t ms = 1400) {
  strncpy(gMsg, m, sizeof(gMsg) - 1); gMsg[sizeof(gMsg) - 1] = 0;
  gMsgUntil = millis() + ms;
}

static inline bool pcDriving() { return (millis() - gLastPcMs) < 4000; }

static float frand(float lo, float hi) {
  return lo + (hi - lo) * ((float)esp_random() / (float)UINT32_MAX);
}
static uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 100 ? 100 : (uint8_t)v); }

// ===========================================================================
// Serial protocol
// ===========================================================================
// Pull an integer value for key=... out of a line (returns def if absent).
static bool findKey(const char *line, const char *key, char *out, size_t outsz) {
  size_t klen = strlen(key);
  const char *p = line;
  while ((p = strstr(p, key)) != nullptr) {
    // require the match to start a token (preceded by space or start) and be
    // followed immediately by '='
    bool boundary = (p == line) || (p[-1] == ' ');
    if (boundary && p[klen] == '=') {
      p += klen + 1;
      size_t i = 0;
      while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < outsz - 1)
        out[i++] = *p++;
      out[i] = 0;
      return true;
    }
    p += klen;
  }
  return false;
}
static bool findFloat(const char *line, const char *key, float *v) {
  char tmp[24];
  if (!findKey(line, key, tmp, sizeof(tmp))) return false;
  *v = atof(tmp);
  return true;
}

static void applyCommand(const char *line) {
  // only lines beginning with "FLY" are commands
  if (strncmp(line, "FLY", 3) != 0) return;

  char act[16];
  if (findKey(line, "action", act, sizeof(act))) {
    if      (!strcmp(act, "walk"))   fly.action = FlyAction::Walk;
    else if (!strcmp(act, "groom"))  fly.action = FlyAction::Groom;
    else if (!strcmp(act, "rest"))   fly.action = FlyAction::Rest;
    else if (!strcmp(act, "feed"))   fly.action = FlyAction::Feed;
    else                             fly.action = FlyAction::Wander;
  }
  float f;
  if (findFloat(line, "x", &f)) { fly.tx = f < 0 ? 0 : (f > 100 ? 100 : f); }
  if (findFloat(line, "y", &f)) { fly.ty = f < 0 ? 0 : (f > 100 ? 100 : f); }
  if (findFloat(line, "hunger", &f)) fly.hunger = clamp8((int)f);
  if (findFloat(line, "energy", &f)) fly.energy = clamp8((int)f);
  if (findFloat(line, "mood",   &f)) fly.mood   = clamp8((int)f);
  findKey(line, "state", gStateLabel, sizeof(gStateLabel));

  // A feed command plants food at the target so the walk-to-eat reads clearly.
  if (fly.action == FlyAction::Feed) {
    fly.hasFood = true; fly.foodX = fly.tx; fly.foodY = fly.ty;
  }
  gLastPcMs = millis();
}

// --- background serial listener ----------------------------------------------
// A dedicated FreeRTOS task owns the USB-CDC *read* side: it assembles complete
// newline-terminated lines and hands them to the UI task through a queue. All
// mutation of `fly` therefore stays on the UI task (render() drains the queue
// and calls applyCommand), so there is no data race on the pet state, and the
// task never writes to Serial — telemetry writes stay on the UI task too, so
// the two directions never contend. Started lazily & idempotently from
// FlyPetView::onEnter (see flypetSerialStart), never from main.cpp.
//
// The OS debug console (ewcon) also reads Serial, so the two would split the
// byte stream between them. While FlyPet is on screen it pauses the console
// (onEnter/onExit) and this task only reads while gOwnSerial is set; once the
// screen exits it stops touching Serial and the console gets it back.
struct SerLine { char s[160]; };
static QueueHandle_t gCmdQueue = nullptr;
static volatile bool gOwnSerial = false;   // FlyPet screen owns Serial RX

static void flypetSerialTask(void *) {
  esp_task_wdt_add(nullptr);
  SerLine ln;
  uint8_t len = 0;
  for (;;) {
    esp_task_wdt_reset();
    if (!gOwnSerial || !ewlogConsolePaused()) { len = 0; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    int c = Serial.read();
    if (c < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        ln.s[len] = 0;
        if (gCmdQueue) xQueueSend(gCmdQueue, &ln, 0);  // drop if UI is behind
        len = 0;
      }
    } else if (len < sizeof(ln.s) - 1) {
      ln.s[len++] = (char)c;
    } else {
      len = 0;  // overrun — resync at the next newline
    }
  }
}

// Drain any queued FLY lines on the UI task and apply them to `fly`.
static void drainSerial() {
  if (!gCmdQueue) return;
  SerLine ln;
  while (xQueueReceive(gCmdQueue, &ln, 0) == pdTRUE) applyCommand(ln.s);
}

void flypetSerialStart() {
  static bool started = false;
  if (started) return;
  started = true;
  gCmdQueue = xQueueCreate(6, sizeof(SerLine));
  xTaskCreatePinnedToCore(flypetSerialTask, "flypet", 4096, nullptr, 3, nullptr, 0);
}

static void txStat() {
  // Telemetry back to the PC — prefixed so the parser ignores boot/log noise.
  Serial.printf(">>STAT hunger=%u energy=%u mood=%u action=%s x=%d y=%d\n",
                fly.hunger, fly.energy, fly.mood, actionName(fly.action),
                (int)fly.x, (int)fly.y);
}

// ===========================================================================
// Autonomous "brain lite" — only runs when no PC is driving. Deliberately
// simple: behaviour-inspired, not a connectome. The real model is Mac-side.
// ===========================================================================
static void pickAutonomousAction() {
  uint32_t now = millis();
  // Needs first: hungry + food present -> go eat.
  if (fly.hasFood && fly.hunger > 30) {
    fly.action = FlyAction::Feed; fly.tx = fly.foodX; fly.ty = fly.foodY;
    gActionUntil = now + 4000; return;
  }
  if (fly.energy < 20) {                 // exhausted -> rest
    fly.action = FlyAction::Rest; gActionUntil = now + frand(3000, 6000); return;
  }
  float r = frand(0, 1);
  if (r < 0.55f) {                       // wander to a new spot
    fly.action = FlyAction::Walk;
    fly.tx = frand(8, 92); fly.ty = frand(10, 90);
    gActionUntil = now + frand(1500, 3500);
  } else if (r < 0.80f) {                // preen
    fly.action = FlyAction::Groom; gActionUntil = now + frand(1500, 3000);
  } else {                              // pause
    fly.action = FlyAction::Rest; gActionUntil = now + frand(1200, 2500);
  }
}

static void decayStats() {
  uint32_t now = millis();
  if (now - gLastDecayMs < 1000) return;  // once a second
  gLastDecayMs = now;
  // Hunger creeps up; grooming/resting recover energy; walking burns it.
  int hungerUp = 1;
  if (fly.action == FlyAction::Feed && fly.hasFood) hungerUp = -6;
  fly.hunger = clamp8((int)fly.hunger + hungerUp);

  int dE = (fly.action == FlyAction::Walk)  ? -1 :
           (fly.action == FlyAction::Rest)  ? +2 :
           (fly.action == FlyAction::Groom) ? +1 : 0;
  fly.energy = clamp8((int)fly.energy + dE);

  // Mood follows how well fed / rested it is.
  int target = 100 - (int)fly.hunger / 2 - (100 - (int)fly.energy) / 3;
  if (fly.mood < target) fly.mood = clamp8((int)fly.mood + 2);
  else if (fly.mood > target) fly.mood = clamp8((int)fly.mood - 1);
}

// ===========================================================================
// Movement — shared by PC-driven and autonomous modes
// ===========================================================================
static void stepMotion(float dt) {
  bool moving = (fly.action == FlyAction::Walk || fly.action == FlyAction::Feed);
  if (moving) {
    float dx = fly.tx - fly.x, dy = fly.ty - fly.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 1.0f) {
      fly.heading = atan2f(dy, dx);
      float spd = (fly.action == FlyAction::Feed ? 34.0f : 26.0f); // units/sec
      float step = spd * dt;
      if (step > dist) step = dist;
      fly.x += cosf(fly.heading) * step;
      fly.y += sinf(fly.heading) * step;
    } else if (fly.action == FlyAction::Feed && fly.hasFood) {
      // arrived at the crumb — eat it
      if (fly.hunger <= 4) { fly.hasFood = false; }
    }
  }
  // keep inside the arena
  if (fly.x < 3) fly.x = 3; if (fly.x > 97) fly.x = 97;
  if (fly.y < 4) fly.y = 4; if (fly.y > 96) fly.y = 96;
}

// ===========================================================================
// Drawing helpers
// ===========================================================================
static void textC(Arduino_GFX *g, const char *s, int16_t cx, int16_t y,
                  uint8_t size, uint16_t color) {
  g->setTextSize(size);
  g->setTextColor(color);
  int16_t w = (int16_t)strlen(s) * 6 * size;
  g->setCursor(cx - w / 2, y);
  g->print(s);
}

// A tapered limb/wing: circles from p0->p1, radius r0->r1.
static void blobLine(Arduino_GFX *g, float x0, float y0, float x1, float y1,
                     float r0, float r1, uint16_t col, int n) {
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    float x = x0 + (x1 - x0) * t;
    float y = y0 + (y1 - y0) * t;
    float r = r0 + (r1 - r0) * t;
    if (r < 0.6f) r = 0.6f;
    g->fillCircle((int16_t)x, (int16_t)y, (int16_t)(r + 0.5f), col);
  }
}

// Draw the fly at pixel (cx,cy) facing `heading`, with animation `phase` ms.
static void drawFly(Arduino_GFX *g, float cx, float cy, float heading,
                    FlyAction action, const FlyState &s, uint32_t phase) {
  float ph = phase / 1000.0f;
  // forward + right unit vectors
  float fx = cosf(heading), fy = sinf(heading);
  float rx = -sinf(heading), ry = cosf(heading);

  bool moving  = (action == FlyAction::Walk || action == FlyAction::Feed);
  bool resting = (action == FlyAction::Rest);
  bool groom   = (action == FlyAction::Groom);

  // gentle idle bob perpendicular shimmer
  float bob = (resting ? 0.6f : 1.4f) * sinf(ph * (resting ? 2.0f : 7.0f));
  cx += rx * bob; cy += ry * bob;

  // --- legs (6): three pairs, animated walking sweep ---
  float walkPh = phase / 90.0f;
  float legLen = 9.0f;
  for (int i = 0; i < 3; i++) {
    float along = (i - 1) * 6.0f;             // -6,0,+6 from thorax
    float bx = cx + fx * along, by = cy + fy * along;
    // walking: legs sweep fore/aft out of phase; still: splayed
    float sweep = moving ? 3.2f * sinf(walkPh + i * 2.0f) : 1.2f;
    float sweepO = moving ? 3.2f * sinf(walkPh + i * 2.0f + 3.14159f) : 1.2f;
    // right leg
    float lx = bx + rx * legLen + fx * sweep;
    float ly = by + ry * legLen + fy * sweep;
    g->drawLine((int16_t)bx, (int16_t)by, (int16_t)lx, (int16_t)ly, LEGCOL);
    g->drawLine((int16_t)(bx+rx), (int16_t)(by+ry), (int16_t)lx, (int16_t)ly, LEGCOL);
    // left leg
    float mx = bx - rx * legLen + fx * sweepO;
    float my = by - ry * legLen + fy * sweepO;
    g->drawLine((int16_t)bx, (int16_t)by, (int16_t)mx, (int16_t)my, LEGCOL);
    g->drawLine((int16_t)(bx-rx), (int16_t)(by-ry), (int16_t)mx, (int16_t)my, LEGCOL);
  }

  // --- wings (two), behind the body, over the abdomen ---
  // flap fast when moving, folded flat when resting
  float flap = moving ? (0.55f + 0.45f * fabsf(sinf(phase / 45.0f)))
                      : (resting ? 0.42f : 0.6f + 0.2f * sinf(ph * 2.0f));
  float wingBaseX = cx - fx * 3.0f, wingBaseY = cy - fy * 3.0f;
  float wingLen = 17.0f;
  // right wing (swept back ~35 deg)
  {
    float wfx = fx * -0.45f + rx * 0.89f;   // back-and-out direction
    float wfy = fy * -0.45f + ry * 0.89f;
    // scale the spread by flap for a flutter illusion
    float ex = wingBaseX + wfx * wingLen * (0.6f + 0.4f * flap);
    float ey = wingBaseY + wfy * wingLen * (0.6f + 0.4f * flap);
    blobLine(g, wingBaseX, wingBaseY, ex, ey, 3.0f, 1.2f, WING, 7);
  }
  // left wing
  {
    float wfx = fx * -0.45f - rx * 0.89f;
    float wfy = fy * -0.45f - ry * 0.89f;
    float ex = wingBaseX + wfx * wingLen * (0.6f + 0.4f * flap);
    float ey = wingBaseY + wfy * wingLen * (0.6f + 0.4f * flap);
    blobLine(g, wingBaseX, wingBaseY, ex, ey, 3.0f, 1.2f, WING, 7);
  }

  // --- abdomen (tapered, behind) ---
  blobLine(g, cx - fx * 2.0f, cy - fy * 2.0f,
              cx - fx * 15.0f, cy - fy * 15.0f, 6.5f, 2.5f, ABDOMEN, 8);
  // faint segment banding
  for (int i = 1; i <= 3; i++) {
    float a = -4.0f - i * 3.0f;
    g->drawCircle((int16_t)(cx + fx * a), (int16_t)(cy + fy * a),
                  (int16_t)(6 - i * 1.2f), blend(ABDOMEN, BLACK, 0.4f));
  }

  // --- thorax ---
  g->fillCircle((int16_t)cx, (int16_t)cy, 7, THORAX);
  g->drawCircle((int16_t)cx, (int16_t)cy, 7, BODY_DK);
  g->fillCircle((int16_t)(cx - fx), (int16_t)(cy - fy), 3,
                blend(THORAX, WHITE, 0.25f));  // sheen

  // --- head + eyes ---
  float hx = cx + fx * 9.0f, hy = cy + fy * 9.0f;
  g->fillCircle((int16_t)hx, (int16_t)hy, 5, BODY_DK);
  // two big red compound eyes, left/right of head
  float eGlance = groom ? 0 : 0.8f * sinf(ph * 1.6f);
  g->fillCircle((int16_t)(hx + rx * 3.0f + fx * eGlance),
                (int16_t)(hy + ry * 3.0f + fy * eGlance), 3, EYE_RED);
  g->fillCircle((int16_t)(hx - rx * 3.0f + fx * eGlance),
                (int16_t)(hy - ry * 3.0f + fy * eGlance), 3, EYE_RED);
  if (s.mood > 55) {  // happy glint
    g->fillCircle((int16_t)(hx + rx * 3.0f + 1), (int16_t)(hy + ry * 3.0f - 1), 1, WHITE);
    g->fillCircle((int16_t)(hx - rx * 3.0f + 1), (int16_t)(hy - ry * 3.0f - 1), 1, WHITE);
  }

  // --- antennae ---
  g->drawLine((int16_t)hx, (int16_t)hy,
              (int16_t)(hx + fx * 6 + rx * 2), (int16_t)(hy + fy * 6 + ry * 2), BODY_DK);
  g->drawLine((int16_t)hx, (int16_t)hy,
              (int16_t)(hx + fx * 6 - rx * 2), (int16_t)(hy + fy * 6 - ry * 2), BODY_DK);

  // --- grooming: front legs rub over the head ---
  if (groom) {
    float g1 = sinf(phase / 70.0f) * 3.0f;
    float ax = hx + fx * 2 + g1 * rx, ay = hy + fy * 2 + g1 * ry;
    blobLine(g, cx + fx * 4, cy + fy * 4, ax, ay, 1.4f, 0.8f, LEGCOL, 4);
    float bx2 = hx + fx * 2 - g1 * rx, by2 = hy + fy * 2 - g1 * ry;
    blobLine(g, cx + fx * 4, cy + fy * 4, bx2, by2, 1.4f, 0.8f, LEGCOL, 4);
  }

  // --- resting: Zs ---
  if (resting) {
    uint16_t zc = rgb(170, 180, 200);
    for (int i = 0; i < 3; i++) {
      float p = ((phase / 120) % 90 + i * 30) / 90.0f;
      int16_t zy = (int16_t)(cy - fy * 10) - (int16_t)(p * 22) - 6;
      int16_t zx = (int16_t)(cx + 10 + i * 7 + 3 * sinf(p * 6.28f));
      g->setTextSize(i == 2 ? 2 : 1);
      g->setTextColor(blend(zc, BLACK, p * 0.7f));
      g->setCursor(zx, zy);
      g->print('z');
    }
  }
}

static void drawHud(Arduino_GFX *g) {
  const char *labels[3] = { "Food", "Enrg", "Mood" };
  uint8_t vals[3] = { (uint8_t)(100 - fly.hunger), fly.energy, fly.mood };
  int16_t cellW = W / 3;
  for (int i = 0; i < 3; i++) {
    int16_t x0 = i * cellW;
    textC(g, labels[i], x0 + cellW / 2, HUD_Y, 1, rgb(190, 190, 190));
    int16_t bx = x0 + 10, by = HUD_Y + 12, bw = cellW - 20, bh = 8;
    g->drawRect(bx, by, bw, bh, rgb(70, 70, 70));
    int16_t fw = (int16_t)((bw - 2) * vals[i] / 100);
    if (fw > 0) g->fillRect(bx + 1, by + 1, fw, bh - 2, statColor(vals[i]));
  }
}

// ===========================================================================
// View
// ===========================================================================
void FlyPetView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  ewlogConsolePause(true);       // FlyPet owns Serial RX while on screen
  gOwnSerial = true;
  flypetSerialStart();           // lazy: bring up the serial listener task
  gLastPcMs = 0;
  gActionUntil = 0;
  gLastDecayMs = millis();
  gMsg[0] = 0; gMsgUntil = 0;
  gStateLabel[0] = 0;
  Serial.println(">>HELLO flypet v1");
  hapticBuzz(70, 40);
}

void FlyPetView::onExit() {
  gOwnSerial = false;            // listener stops reading; console takes over
  ewlogConsolePause(false);
  Serial.println(">>BYE flypet");
}

void FlyPetView::render() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) {                       // PSRAM exhausted — degrade gracefully
    if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg);
               textC(gfx, "flypet (no mem)", W / 2, H / 2, 2, t.fg); }
    return;
  }

  drainSerial();

  // frame delta time from millis()
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  float dt = (lastFrame == 0) ? 0.033f : (now - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;        // clamp after a stall
  lastFrame = now;

  bool pc = pcDriving();
  if (!pc) {
    // autonomous: run the little brain
    if (now >= gActionUntil) pickAutonomousAction();
  }
  decayStats();
  stepMotion(dt);

  // periodic telemetry so the PC always has fresh stats (~ every 2 s)
  if (now - gLastStatTxMs > 2000) { txStat(); gLastStatTxMs = now; }

  ThemeColors t = theme();
  cv->fillScreen(t.bg);

  // arena frame
  cv->drawRect(ARENA_X0 - 2, ARENA_Y0 - 2, arenaW() + 4, arenaH() + 4,
               blend(t.bg, t.line, 0.5f));

  drawHud(cv);

  // food crumb
  if (fly.hasFood) {
    int16_t px = (int16_t)nx2px(fly.foodX), py = (int16_t)ny2px(fly.foodY);
    cv->fillCircle(px, py, 3, FOODCOL);
    cv->fillCircle(px - 2, py + 1, 2, blend(FOODCOL, BLACK, 0.25f));
  }

  drawFly(cv, nx2px(fly.x), ny2px(fly.y), fly.heading, fly.action, fly, now);

  // status line: transient message wins, else action + link state
  char line[40];
  const char *status;
  uint16_t statusCol;
  if (now < gMsgUntil) { status = gMsg; statusCol = t.accent; }
  else {
    snprintf(line, sizeof(line), "%s %s", actionName(fly.action),
             pc ? "[PC]" : "[solo]");
    status = line; statusCol = blend(t.fg, t.bg, 0.15f);
  }
  textC(cv, status, W / 2, STATUS_Y, 2, statusCol);

  // link dot (top-right of status area)
  cv->fillCircle(W - 10, STATUS_Y + 7, 4, pc ? STAT_HI : rgb(90, 90, 90));

  cv->flush();

  // one-shot haptic when the PC link state flips
  if (pc != gPcDrivingLast) {
    hapticBuzz(90, 50);
    setMsg(pc ? "PC connected" : "flying solo", 1200);
    gPcDrivingLast = pc;
  }

  // OS2.1: continuous animation is driven by desiredFrameMs() (returns 33 ms
  // while this view is on screen), not by bumping model.revision here.
}

static void feedAt(float nX, float nY) {
  fly.hasFood = true; fly.foodX = nX; fly.foodY = nY;
  fly.action = FlyAction::Feed; fly.tx = nX; fly.ty = nY;
  gActionUntil = millis() + 5000;
  setMsg("yum! a crumb", 1400);
  hapticBuzz(120, 70);
  Serial.printf(">>FEED x=%d y=%d\n", (int)nX, (int)nY);
}

void FlyPetView::onEvent(const Event &e) {
  // Back: hardware button short-press or a right-swipe (mapped to ButtonShort).
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  // Shake → the fly gets startled and buzzes to a new spot.
  if (e.type == EventType::ImuMotion) {
    fly.action = FlyAction::Walk;
    fly.tx = frand(10, 90); fly.ty = frand(10, 90);
    gActionUntil = millis() + 2500;
    hapticBuzz(80, 40);
    setMsg("bzzzt!", 900);
    return;
  }

  // Tap inside the arena → drop food there (feed). Tap sends an event to the PC.
  if (e.type == EventType::Touch) {
    if (e.y >= ARENA_Y0 && e.y <= ARENA_Y1 && e.x >= ARENA_X0 && e.x <= ARENA_X1) {
      feedAt(px2nx(e.x), py2ny(e.y));
    }
    return;
  }
}
