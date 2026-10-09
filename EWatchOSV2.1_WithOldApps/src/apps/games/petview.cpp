#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>
#include "petview.h"
#include "pet.h"
#include "display.h"     // gfx, frameCanvas, backlightSet
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model + ModelLock
#include "view.h"        // theme(), switchTo()
#include "apptimer.h"    // rtcEpochSec

// ===========================================================================
// Layout (canvas is 240 x 280)
// ===========================================================================
static const int16_t W = 240, H = 280;
static const int16_t HUD_Y    = 4;     // top stat bars
static const int16_t PET_CX   = 120;   // creature centre
static const int16_t PET_CY   = 126;
static const int16_t STAGE_Y  = 188;   // "Baby - Day 2"
static const int16_t STATUS_Y = 200;   // flavour / transient message
static const int16_t BAR_Y    = 224;   // action bar top
static const int16_t BAR_H    = 56;
static const int16_t BTN_W    = 60;    // four 60-px buttons

// ===========================================================================
// Creature palette — its own warm identity, independent of the watch theme so
// the pet stays recognisably itself whatever colours the user picks.
// ===========================================================================
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static uint16_t blend(uint16_t a, uint16_t b, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  float ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  float br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint8_t r = (uint8_t)((ar + (br - ar) * t)) << 3;
  uint8_t g = (uint8_t)((ag + (bg - ag) * t)) << 2;
  uint8_t bl= (uint8_t)((ab + (bb - ab) * t)) << 3;
  return rgb(r, g, bl);
}
static uint16_t darken(uint16_t c, float f) { return blend(c, BLACK, f); }

static const uint16_t BODY    = rgb(232, 107, 43);   // burnt orange #E86B2B
static const uint16_t BODY_DK = rgb(150, 60, 18);    // outline / shadow
static const uint16_t BELLY   = rgb(248, 200, 155);
static const uint16_t EYE_W   = rgb(255, 255, 255);
static const uint16_t PUPIL   = rgb(30, 22, 16);
static const uint16_t CHEEK   = rgb(255, 130, 110);
static const uint16_t SICKCOL = rgb(120, 150, 70);

// Stat-bar colours.
static const uint16_t STAT_HI = rgb(90, 200, 100);
static const uint16_t STAT_MD = rgb(230, 170, 40);
static const uint16_t STAT_LO = rgb(220, 60, 50);
static uint16_t statColor(uint8_t v) { return v > 50 ? STAT_HI : (v > 25 ? STAT_MD : STAT_LO); }

// ===========================================================================
// Transient animation state (owned by the view; pet.cpp owns the lasting mood)
// ===========================================================================
enum class Anim : uint8_t { None, Eat, Play, Refuse, Evolve, Treat };
static Anim     gAnim      = Anim::None;
static uint32_t gAnimStart = 0;
static uint32_t gAnimDur   = 0;
static char     gMsg[20]   = "";
static uint32_t gMsgUntil  = 0;

// Touch press tracking for tap-vs-swipe.
static bool     gPressActive = false, gPressMoved = false;
static uint16_t gPressX = 0, gPressY = 0;
static uint32_t gPressMs = 0;

static uint8_t  gSel = 0;            // highlighted action (swipe to cycle)
static bool     gDimmed = false;    // backlight currently dimmed for sleep
static uint8_t  gBright = 200;      // user brightness captured on enter

static void startAnim(Anim a, uint32_t durMs) {
  gAnim = a; gAnimStart = millis(); gAnimDur = durMs;
}
static bool animActive() {
  return gAnim != Anim::None && (millis() - gAnimStart) < gAnimDur;
}
static float animT() {
  if (gAnimDur == 0) return 1.0f;
  float t = (float)(millis() - gAnimStart) / (float)gAnimDur;
  return t > 1.0f ? 1.0f : t;
}
static void setMsg(const char *m, uint32_t ms = 1500) {
  strncpy(gMsg, m, sizeof(gMsg) - 1); gMsg[sizeof(gMsg) - 1] = '\0';
  gMsgUntil = millis() + ms;
}

// ===========================================================================
// Time helper — current RTC epoch + hour, taken under one short lock.
// ===========================================================================
struct TimeNow { uint32_t epoch; uint8_t hour; bool ok; };
static TimeNow timeNow() {
  uint8_t h, m, s, dy, mo; uint16_t yr; bool ok;
  { ModelLock lk;
    h = model.hour; m = model.minute; s = model.second;
    dy = model.day; mo = model.month; yr = model.year; ok = model.rtcOk; }
  TimeNow t; t.ok = ok; t.hour = h;
  t.epoch = ok ? rtcEpochSec(yr, mo, dy, h, m, s) : 0;
  return t;
}

// ===========================================================================
// Small drawing primitives
// ===========================================================================
static void textC(Arduino_GFX *g, const char *s, int16_t cx, int16_t y,
                  uint8_t size, uint16_t color) {
  g->setTextSize(size);
  g->setTextColor(color);                       // transparent over cleared bg
  int16_t w = (int16_t)strlen(s) * 6 * size;
  g->setCursor(cx - w / 2, y);
  g->print(s);
}

// A curved mouth: c>0 = smile (corners up), c<0 = frown, c=0 = flat line.
static void drawMouth(Arduino_GFX *g, int16_t mx, int16_t my, int16_t w,
                      float c, uint16_t color) {
  for (int16_t x = -w; x <= w; x++) {
    int16_t y = my - (int16_t)(c * (float)(x * x) / (float)(w * w));
    g->fillRect(mx + x, y, 1, 2, color);
  }
}

// Open eye: white with a pupil that can glance via (dx,dy).
static void drawEyeOpen(Arduino_GFX *g, int16_t ex, int16_t ey, int16_t er,
                        int16_t dx, int16_t dy) {
  g->fillCircle(ex, ey, er, EYE_W);
  g->drawCircle(ex, ey, er, BODY_DK);
  g->fillCircle(ex + dx, ey + dy, er > 5 ? er - 3 : 2, PUPIL);
  if (er > 5) g->fillCircle(ex + dx - 1, ey + dy - 1, 1, EYE_W);  // glint
}
// Closed/sleepy eye: a gentle downward arc.
static void drawEyeClosed(Arduino_GFX *g, int16_t ex, int16_t ey, int16_t er) {
  for (int16_t x = -er; x <= er; x++) {
    int16_t y = ey + (int16_t)(2.0f * (1.0f - (float)(x * x) / (float)(er * er)));
    g->fillRect(ex + x, y, 1, 2, BODY_DK);
  }
}
// Happy "^" eye.
static void drawEyeHappy(Arduino_GFX *g, int16_t ex, int16_t ey, int16_t er) {
  g->drawLine(ex - er, ey + 2, ex, ey - 3, BODY_DK);
  g->drawLine(ex,      ey - 3, ex + er, ey + 2, BODY_DK);
  g->drawLine(ex - er, ey + 3, ex, ey - 2, BODY_DK);
  g->drawLine(ex,      ey - 2, ex + er, ey + 3, BODY_DK);
}

// ===========================================================================
// The creature
// ===========================================================================
static void drawZs(Arduino_GFX *g, int16_t x, int16_t y, uint32_t phase) {
  // Three rising, fading "z"s above the head.
  uint16_t col = rgb(180, 190, 210);
  for (int i = 0; i < 3; i++) {
    float p = ((phase / 110) % 90 + i * 30) / 90.0f;       // 0..1 each
    int16_t zy = y - (int16_t)(p * 26);
    int16_t zx = x + i * 9 + (int16_t)(3 * sinf(p * 6.28f));
    uint8_t sz = (i == 2) ? 2 : 1;
    g->setTextSize(sz);
    g->setTextColor(blend(col, BLACK, p * 0.7f));
    g->setCursor(zx, zy);
    g->print('z');
  }
}

static void drawEgg(Arduino_GFX *g, int16_t cx, int16_t cy, uint32_t phase) {
  int16_t wob = (int16_t)(3.0f * sinf(phase / 260.0f));     // gentle rock
  cx += wob;
  // Egg = small circle (top) fused with a larger circle (bottom).
  g->fillCircle(cx, cy + 12, 34, BELLY);
  g->fillCircle(cx, cy - 20, 26, BELLY);
  g->fillRect(cx - 26, cy - 20, 52, 34, BELLY);
  // Speckles.
  uint16_t sp = rgb(214, 150, 90);
  g->fillCircle(cx - 12, cy - 6, 3, sp);
  g->fillCircle(cx + 14, cy + 4, 4, sp);
  g->fillCircle(cx - 6, cy + 20, 3, sp);
  g->fillCircle(cx + 8, cy - 18, 2, sp);
  // Zig-zag crack.
  uint16_t ck = darken(BELLY, 0.45f);
  int16_t zx = cx - 22, zy = cy + 2;
  for (int i = 0; i < 6; i++) {
    int16_t nx = zx + 8, ny = zy + ((i & 1) ? 6 : -6);
    g->drawLine(zx, zy, nx, ny, ck);
    zx = nx; zy = ny;
  }
}

// Draw the living creature at (cx,cy). `s` carries stage + mood + sleep state.
static void drawCreature(Arduino_GFX *g, int16_t cx, int16_t cy,
                         const PetSnapshot &s, uint32_t phase) {
  if (s.stage == PetStage::Egg) { drawEgg(g, cx, cy, phase); return; }

  int16_t r;
  bool feet = false, antenna = false;
  switch (s.stage) {
    case PetStage::Baby:  r = 34; break;
    case PetStage::Child: r = 44; feet = true; break;
    case PetStage::Adult: r = 52; feet = true; antenna = true; break;
    default:              r = 40; break;
  }

  // Body colour follows health/sleep — sickly green when ill, dimmed at night.
  uint16_t body  = BODY;
  if (s.mood == PetMood::Sick) body = blend(BODY, SICKCOL, 0.5f);
  if (s.asleep) body = darken(body, 0.35f);
  uint16_t bodyDk = darken(body, 0.35f);
  uint16_t belly  = s.asleep ? darken(BELLY, 0.35f) : BELLY;

  // Animation offsets.
  float ph = phase / 1000.0f;
  int16_t bob = (int16_t)(2.5f * sinf(ph * 3.0f));
  int16_t xo  = 0;
  if (s.asleep)                         bob = (int16_t)(2.0f * sinf(ph * 1.1f));
  else if (s.mood == PetMood::Happy)    bob = -(int16_t)fabsf(6.0f * sinf(ph * 6.0f));
  if (animActive() && gAnim == Anim::Play)
    bob = -(int16_t)fabsf(11.0f * sinf(ph * 13.0f));
  if (animActive() && gAnim == Anim::Refuse)
    xo  = (int16_t)(7.0f * sinf((float)(millis() - gAnimStart) / 22.0f));
  cx += xo; cy += bob;

  // Antenna (drawn behind body).
  if (antenna) {
    g->drawLine(cx, cy - r, cx, cy - r - 13, bodyDk);
    g->drawLine(cx + 1, cy - r, cx + 1, cy - r - 13, bodyDk);
    g->fillCircle(cx, cy - r - 15, 4, blend(body, EYE_W, 0.3f));
  }
  // Feet.
  if (feet) {
    g->fillCircle(cx - (int16_t)(r * 0.55f), cy + r - 4, 9, bodyDk);
    g->fillCircle(cx + (int16_t)(r * 0.55f), cy + r - 4, 9, bodyDk);
  }
  // Body + belly + outline.
  g->fillCircle(cx, cy, r, body);
  g->drawCircle(cx, cy, r, bodyDk);
  g->drawCircle(cx, cy, r - 1, bodyDk);
  g->fillCircle(cx, cy + (int16_t)(r * 0.32f), (int16_t)(r * 0.55f), belly);

  // Mess: a few grubby spots + a buzzing fly when filthy.
  if (s.mess > 45 && !s.asleep) {
    uint16_t dirt = rgb(110, 80, 50);
    g->fillCircle(cx - r / 2, cy + r / 3, 4, dirt);
    g->fillCircle(cx + r / 3, cy + r / 2, 3, dirt);
    if (s.mess > 70) {
      int16_t fx = cx + r + 6 + (int16_t)(4 * sinf(ph * 9.0f));
      int16_t fy = cy - r / 2 + (int16_t)(5 * cosf(ph * 11.0f));
      g->fillCircle(fx, fy, 2, BLACK);
      g->drawLine(fx - 3, fy - 2, fx, fy, DARKGREY);
      g->drawLine(fx + 3, fy - 2, fx, fy, DARKGREY);
    }
  }

  // --- Face ---
  int16_t er = (s.stage == PetStage::Baby) ? 9 : (s.stage == PetStage::Adult ? 8 : 8);
  int16_t ex = (int16_t)(r * 0.42f);
  int16_t ey = cy - (int16_t)(r * 0.12f);
  int16_t my = cy + (int16_t)(r * 0.40f);          // mouth line
  bool blink = ((phase % 4000) < 150);

  bool eating = animActive() && gAnim == Anim::Eat;

  if (s.asleep) {
    drawEyeClosed(g, cx - ex, ey, er);
    drawEyeClosed(g, cx + ex, ey, er);
    drawMouth(g, cx, my, 6, 0.0f, bodyDk);
    drawZs(g, cx + r - 4, cy - r, phase);
  } else if (s.mood == PetMood::Sick) {
    int16_t dy = 2;
    drawEyeOpen(g, cx - ex, ey, er, 0, dy);
    drawEyeOpen(g, cx + ex, ey, er, 0, dy);
    drawMouth(g, cx, my + 2, 8, -2.0f, bodyDk);    // queasy frown
    // sweat drop
    g->fillCircle(cx + ex + er, ey - er, 3, rgb(120, 180, 230));
  } else if (s.mood == PetMood::Sad) {
    int16_t dy = 3;
    drawEyeOpen(g, cx - ex, ey + 1, er, -1, dy);
    drawEyeOpen(g, cx + ex, ey + 1, er, 1, dy);
    drawMouth(g, cx, my + 3, 9, -3.0f, bodyDk);    // frown
    g->fillCircle(cx - ex - er + 1, ey + er, 2, rgb(120, 180, 230));  // tear
  } else if (eating) {
    // Chewing: an open mouth that opens/closes; a morsel shrinks toward it.
    drawEyeOpen(g, cx - ex, ey, er, 0, 1);
    drawEyeOpen(g, cx + ex, ey, er, 0, 1);
    float chew = 0.5f + 0.5f * sinf((float)(millis() - gAnimStart) / 70.0f);
    int16_t mh = 3 + (int16_t)(6 * chew);
    g->fillCircle(cx, my + 2, mh, rgb(120, 40, 30));
    float k = 1.0f - animT();
    int16_t morselY = cy - r - 18 + (int16_t)((my + 2 - (cy - r - 18)) * (1.0f - k));
    int16_t mr = (int16_t)(7 * k);
    if (mr > 1) { g->fillCircle(cx + 22, morselY, mr, rgb(220, 60, 50));
                  g->drawLine(cx + 22, morselY - mr, cx + 24, morselY - mr - 4, rgb(80, 160, 70)); }
  } else if (s.mood == PetMood::Happy ||
             (animActive() && gAnim == Anim::Play)) {
    drawEyeHappy(g, cx - ex, ey, er);
    drawEyeHappy(g, cx + ex, ey, er);
    drawMouth(g, cx, my, 11, 4.0f, bodyDk);        // big smile
    g->fillCircle(cx - ex - 2, my - 2, 4, CHEEK);  // blush
    g->fillCircle(cx + ex + 2, my - 2, 4, CHEEK);
  } else {  // Neutral
    if (blink) { drawEyeClosed(g, cx - ex, ey, er); drawEyeClosed(g, cx + ex, ey, er); }
    else {
      int16_t gx = (int16_t)(2.0f * sinf(ph * 1.7f));        // idle glance
      drawEyeOpen(g, cx - ex, ey, er, gx, 0);
      drawEyeOpen(g, cx + ex, ey, er, gx, 0);
    }
    drawMouth(g, cx, my, 8, 1.5f, bodyDk);         // soft smile
  }
}

// Sparkles for an evolution / treat celebration.
static void drawSparkles(Arduino_GFX *g, int16_t cx, int16_t cy, float t) {
  int16_t rad = (int16_t)(20 + t * 70);
  uint16_t col = blend(rgb(255, 230, 120), BLACK, t);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785f + t * 2.0f;
    int16_t x = cx + (int16_t)(rad * cosf(a));
    int16_t y = cy + (int16_t)(rad * sinf(a));
    int16_t s = 4 - (int16_t)(t * 3);
    if (s < 1) s = 1;
    g->fillRect(x - s, y, 2 * s, 1, col);
    g->fillRect(x, y - s, 1, 2 * s, col);
  }
}

// ===========================================================================
// HUD + action bar
// ===========================================================================
static void drawHud(Arduino_GFX *g, const PetSnapshot &s) {
  const char *labels[4] = { "Fed", "Joy", "Eng", "Hp" };
  uint8_t vals[4] = { (uint8_t)(100 - s.hunger), s.happiness, s.energy, s.health };
  for (int i = 0; i < 4; i++) {
    int16_t x0 = i * BTN_W;
    textC(g, labels[i], x0 + BTN_W / 2, HUD_Y, 1, rgb(190, 190, 190));
    int16_t bx = x0 + 8, by = HUD_Y + 12, bw = BTN_W - 16, bh = 7;
    g->drawRect(bx, by, bw, bh, rgb(70, 70, 70));
    int16_t fw = (int16_t)((bw - 2) * vals[i] / 100);
    if (fw > 0) g->fillRect(bx + 1, by + 1, fw, bh - 2, statColor(vals[i]));
  }
}

// Action-button icons (~drawn within a 60-wide cell centred at cx, around cy).
static void drawIcon(Arduino_GFX *g, PetAction a, int16_t cx, int16_t cy, uint16_t fg) {
  switch (a) {
    case PetAction::Feed: {           // apple
      g->fillCircle(cx - 3, cy, 7, rgb(220, 60, 50));
      g->fillCircle(cx + 3, cy, 7, rgb(220, 60, 50));
      g->drawLine(cx, cy - 7, cx + 2, cy - 12, rgb(120, 80, 40));
      g->fillTriangle(cx + 2, cy - 12, cx + 8, cy - 13, cx + 4, cy - 8, rgb(80, 170, 70));
      break;
    }
    case PetAction::Play: {           // bouncing ball
      g->fillCircle(cx, cy, 8, rgb(80, 150, 230));
      g->fillCircle(cx - 3, cy - 3, 2, EYE_W);
      g->drawLine(cx - 8, cy, cx + 8, cy, rgb(20, 60, 120));
      g->drawLine(cx, cy - 8, cx, cy + 8, rgb(20, 60, 120));
      break;
    }
    case PetAction::Clean: {          // water drop + bubbles
      g->fillCircle(cx, cy + 2, 6, rgb(90, 180, 230));
      g->fillTriangle(cx - 5, cy, cx + 5, cy, cx, cy - 9, rgb(90, 180, 230));
      g->fillCircle(cx - 2, cy, 2, EYE_W);
      g->drawCircle(cx + 8, cy - 5, 2, rgb(160, 210, 240));
      break;
    }
    case PetAction::Sleep: {          // crescent moon
      g->fillCircle(cx, cy, 8, rgb(235, 220, 120));
      g->fillCircle(cx + 4, cy - 3, 7, fg);    // carve crescent with btn bg
      textC(g, "z", cx + 7, cy - 10, 1, rgb(235, 220, 120));
      break;
    }
  }
}

static void drawActionBar(Arduino_GFX *g, const ThemeColors &t) {
  uint16_t panel = blend(t.bg, t.line, 0.25f);
  for (int i = 0; i < 4; i++) {
    PetAction a = (PetAction)i;
    int16_t x = i * BTN_W;
    bool sel = (i == gSel);
    uint16_t bg = sel ? t.accent : panel;
    g->fillRoundRect(x + 3, BAR_Y + 2, BTN_W - 6, BAR_H - 4, 8, bg);
    if (sel) g->drawRoundRect(x + 3, BAR_Y + 2, BTN_W - 6, BAR_H - 4, 8, t.fg);
    uint16_t fg = sel ? contrastFor(t.accent) : t.fg;
    drawIcon(g, a, x + BTN_W / 2, BAR_Y + 20, bg);
    textC(g, petActionName(a), x + BTN_W / 2, BAR_Y + 38, 1, fg);
  }
}

// ===========================================================================
// View
// ===========================================================================
void PetView::onEnter() {
  TimeNow tn = timeNow();
  petInit(tn.epoch, tn.ok);          // load or create on first ever entry
  { ModelLock lk; gBright = model.brightness; }
  gDimmed = false;
  gAnim = Anim::None;
  gMsg[0] = '\0'; gMsgUntil = 0;
  gPressActive = false;
  // Greet on entry: if a need is already critical, nag once.
  if (petNagDue()) { hapticBuzz(70, 70); hapticBuzz(70, 70); }
}

void PetView::onExit() {
  petSave();
  if (gDimmed) { backlightSet(gBright); gDimmed = false; }
}

void PetView::render() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) {                         // PSRAM exhausted — degrade gracefully
    if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg);
               textC(gfx, "pet (no mem)", W / 2, H / 2, 2, t.fg); }
    return;
  }

  TimeNow tn = timeNow();
  petTick(tn.epoch, tn.hour, tn.ok);
  PetSnapshot s = petGet();

  // One-shot celebrations raised by pet.cpp.
  if (petTakeEvolved()) { startAnim(Anim::Evolve, 2200); setMsg("evolved!", 2200);
                          hapticBuzz(150, 90); hapticBuzz(150, 90); hapticBuzz(150, 120); }
  if (petTakeTreat())   { startAnim(Anim::Treat, 1600); setMsg("treat! +", 1600);
                          hapticBuzz(120, 70); hapticBuzz(120, 70); }
  // Periodic "needs attention" nag.
  if (petNagDue()) { hapticBuzz(70, 70); hapticBuzz(70, 70); }

  // Dim the backlight while the pet sleeps; restore when it wakes.
  if (s.asleep && !gDimmed) {
    uint8_t dim = gBright / 3; if (dim < 16) dim = 16;
    backlightSet(dim); gDimmed = true;
  } else if (!s.asleep && gDimmed) {
    backlightSet(gBright); gDimmed = false;
  }

  ThemeColors t = theme();
  uint16_t bg = s.asleep ? darken(t.bg, 0.25f) : t.bg;
  cv->fillScreen(bg);

  uint32_t phase = millis();
  drawHud(cv, s);
  drawCreature(cv, PET_CX, PET_CY, s, phase);
  if (animActive() && (gAnim == Anim::Evolve || gAnim == Anim::Treat))
    drawSparkles(cv, PET_CX, PET_CY, animT());

  // Name + stage + age.
  char sub[36];
  snprintf(sub, sizeof(sub), "%s  %s  Day %u",
           petName(), petStageName(s.stage), s.ageDays);
  textC(cv, sub, W / 2, STAGE_Y, 1, blend(t.fg, t.bg, 0.35f));

  // Status line: transient message wins, else flavour text.
  const char *status = (millis() < gMsgUntil) ? gMsg : petStatusLine();
  uint16_t statusCol = (millis() < gMsgUntil) ? t.accent : t.fg;
  textC(cv, status, W / 2, STATUS_Y, 2, statusCol);

  drawActionBar(cv, t);
  cv->flush();
}

// Perform an action and react with animation + haptics + a status message.
static void performAction(PetAction a) {
  gSel = (uint8_t)a;
  TimeNow tn = timeNow();
  PetReact r = petDo(a, tn.epoch, tn.hour);
  switch (r) {
    case PetReact::Ok:
      switch (a) {
        case PetAction::Feed:  startAnim(Anim::Eat, 1800);  setMsg("yum!");
                               hapticBuzz(130, 90); break;
        case PetAction::Play:  startAnim(Anim::Play, 1500); setMsg("wheee!");
                               hapticBuzz(120, 60); hapticBuzz(120, 60); break;
        case PetAction::Clean: startAnim(Anim::Treat, 900); setMsg("all clean!");
                               hapticBuzz(100, 70); break;
        default: break;
      }
      break;
    case PetReact::Refused:
      startAnim(Anim::Refuse, 600);
      setMsg(a == PetAction::Feed ? "not hungry" : "too tired");
      hapticBuzz(50, 180);                       // slow, low "no"
      break;
    case PetReact::Slept: setMsg("nap time");      hapticBuzz(60, 150); break;
    case PetReact::Woke:  setMsg("good morning!"); hapticBuzz(90, 60);  break;
  }
}

void PetView::onEvent(const Event &e) {
  // Back: hardware button short-press or a right-swipe (mapped to ButtonShort).
  if (e.type == EventType::ButtonShort) { petSave(); switchTo(Screen::AppList); return; }

  // Shake → activity / mini-play.
  if (e.type == EventType::ImuMotion) {
    TimeNow tn = timeNow();
    petShake(tn.epoch);
    PetSnapshot s = petGet();
    if (!s.asleep) { startAnim(Anim::Play, 900); hapticBuzz(90, 50); }
    return;
  }

  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp)   { gSel = (gSel + 3) & 3; hapticBuzz(40, 40); }
    if (e.gesture == Gesture::SwipeDown) { gSel = (gSel + 1) & 3; hapticBuzz(40, 40); }
    if (e.gesture == Gesture::DoubleTap) { performAction((PetAction)gSel); }
    return;
  }

  if (e.type == EventType::Touch) {
    gPressActive = true; gPressMoved = false;
    gPressX = e.x; gPressY = e.y; gPressMs = millis();
    return;
  }
  if (e.type == EventType::TouchHold && gPressActive) {
    int dx = (int)e.x - gPressX, dy = (int)e.y - gPressY;
    if (dx * dx + dy * dy > 18 * 18) gPressMoved = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    bool quick = (millis() - gPressMs) < 600;
    if (gPressActive && !gPressMoved && quick) {
      if (gPressY >= BAR_Y) {                          // action bar
        int idx = gPressX / BTN_W;
        if (idx >= 0 && idx < 4) performAction((PetAction)idx);
      } else if (gPressY > 70 && gPressY < BAR_Y) {    // tapped the creature
        TimeNow tn = timeNow();
        petPet(tn.epoch);
        PetSnapshot s = petGet();
        if (!s.asleep) { hapticBuzz(60, 40); setMsg("<3", 700); }
      }
    }
    gPressActive = false; gPressMoved = false;
    return;
  }
}
