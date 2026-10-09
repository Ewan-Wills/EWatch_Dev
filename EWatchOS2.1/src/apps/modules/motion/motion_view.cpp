// MotionView — the Motion app. Page 0: live token stream, the LM's next-token
// guess, the recognised gesture + confidence, latency and the action. Page 1:
// the gesture -> action map. Header: back | title (tap = switch page) | DRY/LIVE.
// Bottom chips: BG (keep the engine running on every screen) and REC (label
// for data capture). Swipes anywhere and taps in the middle are motion tokens.
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

#include "motion_view.h"
#include "motion_tok.h"
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"

static const int16_t W = 240;
static const int16_t HDR_H = 46;
static const int16_t CHIP_Y = 232, CHIP_H = 40;
static const int16_t MODE_X = 176, MODE_W = 58;

static MotionView sView;
View *motionViewPtr() { return &sView; }

static uint16_t tokColor(int t) {
  if (t >= MT_STILL0 && t < MT_MOVE0) return DARKGREY;
  if (t >= MT_MOVE0 && t < MT_JOLT0)  return ((t - MT_MOVE0) / 6) & 1 ? YELLOW : CYAN;
  if (t >= MT_JOLT0 && t < MT_TOUCH0) return ORANGE;
  if (t >= MT_TOUCH0 && t < MT_LABEL0) return MAGENTA;
  return WHITE;
}

void MotionView::onEnter() {
  firstDraw = true;
  shownVersion = 0xFFFFFFFF;
  motionEngineStart();
}

void MotionView::onExit() { motionEngineStop(); }

void MotionView::drawChrome() {
  drawTitleBar(page == 0 ? "Motion" : "Actions");
  MotionSnap s; motionGetSnap(&s);
  ThemeColors t = theme();
  uint16_t c = s.dry ? DARKGREY : RED;
  gfx->fillRoundRect(MODE_X, 8, MODE_W, 30, 8, c);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, c);
  gfx->setCursor(MODE_X + (s.dry ? 11 : 5), 16);
  gfx->print(s.dry ? "DRY" : "LIVE");
  (void)t;
}

static void chip(int16_t x, int16_t w, const char *label, bool on) {
  ThemeColors t = theme();
  uint16_t bg = on ? t.accent : t.bg;
  gfx->fillRoundRect(x, CHIP_Y, w, CHIP_H, 10, bg);
  gfx->drawRoundRect(x, CHIP_Y, w, CHIP_H, 10, t.line);
  gfx->setTextSize(1);
  gfx->setTextColor(on ? contrastFor(bg) : t.fg, bg);
  int16_t tw = (int16_t)strlen(label) * 6;
  gfx->setCursor(x + (w - tw) / 2, CHIP_Y + 16);
  gfx->print(label);
}

// Region caches: each part of the live page is repainted only when what it
// shows changed (text is drawn with a background colour, padded to a fixed
// width), so a token arriving doesn't blank and redraw the whole screen.
static struct {
  uint8_t  toks[8]; uint8_t ntoks;
  char     lm[48];
  uint32_t resAt; int resLabel; bool resFresh;
  char     st[3][48];
  int      global, cap;
} C;

static void textAt(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint16_t bg, const char *str, char *cache) {
  if (cache && !strcmp(cache, str)) return;
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(x, y);
  gfx->print(str);
  if (cache) strcpy(cache, str);
}

void MotionView::drawLive(const MotionSnap &s) {
  ThemeColors t = theme();
  char b[64], nb[16];

  // ---- token stream: 2 rows x 4, newest bottom-right (fixed 4-char cells)
  if (s.ntoks != C.ntoks || memcmp(s.toks, C.toks, sizeof C.toks)) {
    for (int slot = 0; slot < 8; slot++) {
      int i = slot - (8 - s.ntoks);
      int16_t x = 6 + (slot % 4) * 58, y = HDR_H + 6 + (slot / 4) * 22;
      snprintf(b, sizeof b, "%-4s", i >= 0 ? mt_tok_name(s.toks[i], nb, sizeof nb) : "");
      textAt(x, y, 2, i == s.ntoks - 1 ? WHITE : (i >= 0 ? tokColor(s.toks[i]) : t.bg), t.bg, b, nullptr);
    }
    memcpy(C.toks, s.toks, sizeof C.toks); C.ntoks = s.ntoks;
  }

  // ---- LM state line
  if (!s.ready) snprintf(b, sizeof b, "%-38s", "loading model...");
  else {
    char seg[12] = "";
    if (s.segActive) snprintf(seg, sizeof seg, "%2u tok", s.segLen);
    snprintf(b, sizeof b, "LM next: %-5s %-13s %-8s", s.predNext >= 0 ? mt_tok_name(s.predNext, nb, sizeof nb) : "-",
             s.segActive ? "segment open" : (s.running ? "listening" : "stopped"), seg);
  }
  textAt(6, 98, 1, t.line, t.bg, b, C.lm);

  // ---- recognised gesture (repainted per recognition, and when it goes stale)
  bool fresh = s.lastLabel >= 0 && millis() - s.lastAtMs < 4000;
  if (s.lastAtMs != C.resAt || s.lastLabel != C.resLabel || fresh != C.resFresh) {
    gfx->fillRect(0, 110, W, 64, t.bg);
    const char *name = s.lastLabel >= 0 ? ml_label_name(s.lastLabel) : "move me";
    uint8_t sz = strlen(name) > 9 ? 2 : 3;
    textAt(uiCenterX(name, sz), 114, sz, s.lastLabel > 0 ? (fresh ? GREEN : t.fg) : t.line, t.bg, name, nullptr);
    if (s.lastLabel >= 0) {
      int16_t bw = (int16_t)(s.lastP * 160);
      gfx->drawRect(20, 144, 162, 10, t.line);
      gfx->fillRect(21, 145, bw, 8, s.lastP >= s.thr ? GREEN : ORANGE);
      snprintf(b, sizeof b, "%.2f", s.lastP);
      textAt(188, 145, 1, t.fg, t.bg, b, nullptr);
      snprintf(b, sizeof b, "%s %s-> %s", s.lastTouch ? "touch" : "imu",
               (s.lastLabel == 0 && s.lastBest > 0) ? "(low p) " : "", s.lastAction);
      textAt(6, 162, 1, fresh ? YELLOW : t.line, t.bg, b, nullptr);
    }
    C.resAt = s.lastAtMs; C.resLabel = s.lastLabel; C.resFresh = fresh;
  }

  // ---- latency / throughput (measured on the device with esp_timer)
#if defined(EWSIM_HOST)
  const char *tag = " sim";     // host simulator: CPU time is free, so ~0 ms here
#else
  const char *tag = "";
#endif
  snprintf(b, sizeof b, "infer %.2f ms/tok avg %.2f%-4s", s.lastFwdMs, s.avgFwdMs, tag);
  snprintf(b + strlen(b), sizeof b - strlen(b), "%*s", (int)(38 - strlen(b)) > 0 ? (int)(38 - strlen(b)) : 0, "");
  textAt(6, 186, 1, t.fg, t.bg, b, C.st[0]);
  snprintf(b, sizeof b, "classify %.2f ms  LM %3.0f tok/s      ", s.lastClassifyMs, s.tokPerSec);
  textAt(6, 198, 1, t.fg, t.bg, b, C.st[1]);
  snprintf(b, sizeof b, "segments %lu  gestures %lu%-10s", (unsigned long)s.segments,
           (unsigned long)s.recognised, s.injectLeft ? "  [inject]" : "");
  textAt(6, 210, 1, t.fg, t.bg, b, C.st[2]);

  // ---- chips
  if (C.global != (int)s.global) { chip(6, 110, s.global ? "BG: on" : "BG: off", s.global); C.global = s.global; }
  if (C.cap != s.capLabel) {
    snprintf(b, sizeof b, "REC: %s", s.capLabel >= 0 ? ml_label_name(s.capLabel) : "off");
    chip(124, 110, b, s.capLabel >= 0);
    C.cap = s.capLabel;
  }
}

void MotionView::drawMap() {
  ThemeColors t = theme();
  gfx->setTextSize(1);
  for (int i = 1; i < MT_NLABELS; i++) {
    int16_t y = HDR_H + 8 + (i - 1) * 15;
    gfx->setTextColor(i >= ML_SWIPE_UU ? MAGENTA : CYAN, t.bg);
    gfx->setCursor(8, y);
    gfx->print(ml_label_name(i));
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(104, y);
    gfx->print(motionActionText(i));
  }
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(8, HDR_H + 10 + 14 * 15);
  gfx->print("magenta = swipe/tap phrases");
}

void MotionView::render() {
  if (!gfx) return;
  MotionSnap s; motionGetSnap(&s);
  if (firstDraw) {
    memset(&C, 0, sizeof C);
    C.ntoks = 0xFF; C.resAt = 0xFFFFFFFF; C.resLabel = -2; C.global = -1; C.cap = -2;
    drawChrome();
    if (page == 1) drawMap();
    firstDraw = false;
    shownVersion = s.version - 1;
  }
  if (page != 0) return;
  uint32_t sec = millis() / 1000;          // refresh "fresh" colouring + rate once a second
  if (s.version != shownVersion || sec != shownSecond) {
    drawLive(s);
    shownVersion = s.version;
    shownSecond = sec;
  }
}

void MotionView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
    if (e.y < HDR_H) {
      if (e.x >= MODE_X) {
        MotionSnap s; motionGetSnap(&s);
        motionSetDry(!s.dry);
      } else {
        page ^= 1;
      }
      hapticBuzz(24, 25);
      firstDraw = true;
      return;
    }
    if (page == 0 && e.y >= CHIP_Y) {
      MotionSnap s; motionGetSnap(&s);
      if (e.x < 120) motionSetGlobal(!s.global);
      else motionSetCapture(s.capLabel + 1 >= MT_NLABELS ? -1 : s.capLabel + 1);
      hapticBuzz(24, 25);
      return;
    }
    return;
  }
  if (e.type == EventType::Gesture) {
    bool isTap = e.gesture == Gesture::SingleTap || e.gesture == Gesture::DoubleTap ||
                 e.gesture == Gesture::LongPress;
    if (isTap && (e.y < HDR_H || e.y >= CHIP_Y)) return;   // that was a button press
    motionFeedTouch(e.gesture);
  }
}
