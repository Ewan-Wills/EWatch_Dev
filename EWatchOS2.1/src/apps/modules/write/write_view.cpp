// WriteView: handwriting input. Draw a letter or digit anywhere below the
// text; when the finger stays up for gapMs (450 ms) the strokes are
// recognised (write_cnn: CNN on the rasterised ink) with English context
// (write_lm: letter trigram while writing, dictionary correction when the
// word ends) and the character is typed into the shared text (write_text.h).
// Gestures: one straight stroke left->right = space, right->left = delete.
// Buttons: shift (tap: next letter upper case, double-tap: caps lock) and
// delete (tap: one char, hold: repeat). Side button = back.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "write_view.h"
#include "write_cnn.h"
#include "write_text.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "ui_style.h"
#include "FreeSans12pt7b.h"

// ---------------------------------------------------------------- layout
static const int16_t W = 240, H = 280;
static const int16_t TXT_X = 8, TXT_Y = 8, TXT_W = 224, TXT_H = 50;
static const int16_t INK_Y = TXT_Y + TXT_H + 4;          // ink anywhere below the text
static const int16_t BTN_R = 21;                         // button radius
static const int16_t SHIFT_X = 30, DEL_X = W - 30, BTN_Y = H - 30;
static const int16_t INK_R = 3;                          // brush radius (px)
static const int16_t kMoveSlop = 10;                     // a press that moves this far is ink
static const float   kReject = 0.12f;                    // CNN top probability below: not typed
static const uint32_t kRepeatDelay = 420, kRepeatEvery = 110, kDoubleTap = 380;
static const uint32_t kIdleCorrectMs = 2500;             // pause after a letter: correct the word

// ---------------------------------------------------------------- shared text
static char          sText[kWriteTextMax + 1];
static size_t        sLen = 0;
static uint32_t      sRev = 0;
static portMUX_TYPE  sTextMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      sLastInputMs = 0;

size_t writeGetText(char *out, size_t cap) {
  if (!out || !cap) return 0;
  portENTER_CRITICAL(&sTextMux);
  size_t n = sLen < cap - 1 ? sLen : cap - 1;
  memcpy(out, sText, n);
  portEXIT_CRITICAL(&sTextMux);
  out[n] = 0;
  return n;
}
void writeSetText(const char *s) {
  size_t n = s ? strlen(s) : 0;
  if (n > kWriteTextMax) n = kWriteTextMax;
  portENTER_CRITICAL(&sTextMux);
  memcpy(sText, s ? s : "", n);
  sText[n] = 0; sLen = n; sRev++;
  portEXIT_CRITICAL(&sTextMux);
}
void writeClearText() { writeSetText(""); }
uint32_t writeTextRevision() { return sRev; }
bool writeKeepAwake() { return millis() - sLastInputMs < 60000; }

// Console (tests): `write text` -> "WTEXT:<text>|" ('\n' as '~', '|' marks the
// end so a cut-off line is detectable).
bool writeConsole(const char *line) {
  if (strcmp(line, "write text") != 0) return false;
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  for (size_t i = 0; i < n; i++) if (t[i] == '\n') t[i] = '~';
  Serial.printf("WTEXT:%s|\n", t);
  return true;
}

static void textAppend(char c) {
  portENTER_CRITICAL(&sTextMux);
  if (sLen < kWriteTextMax) { sText[sLen++] = c; sText[sLen] = 0; sRev++; }
  portEXIT_CRITICAL(&sTextMux);
}
static char textPop() {
  char c = 0;
  portENTER_CRITICAL(&sTextMux);
  if (sLen > 0) { c = sText[--sLen]; sText[sLen] = 0; sRev++; }
  portEXIT_CRITICAL(&sTextMux);
  return c;
}
static char textLast() {
  portENTER_CRITICAL(&sTextMux);
  char c = sLen ? sText[sLen - 1] : 0;
  portEXIT_CRITICAL(&sTextMux);
  return c;
}
static void logText() {
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  const char *tail = n > 47 ? t + (n - 47) : t;
  for (size_t i = 0; i < n; i++) if (t[i] == '\n') t[i] = '|';
  EWLOGI("WRITE", "text=%s", tail);
}
static void saveText() {
  char t[kWriteTextMax + 1];
  writeGetText(t, sizeof t);
  Preferences p;
  if (p.begin("ewwrite", false)) { p.putString("text", t); p.end(); }
}

static inline bool inBtn(int x, int y, int cx) {
  int dx = x - cx, dy = y - BTN_Y;
  return dx * dx + dy * dy <= (BTN_R + 6) * (BTN_R + 6);
}

// ---------------------------------------------------------------- ink drawing
// Round brush stamped along the segment (smooth, even width).
static void inkSeg(Arduino_GFX *d, int x0, int y0, int x1, int y1, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)(sqrtf(dx * dx + dy * dy) / 1.5f) + 1;
  for (int i = 0; i <= n; i++) {
    int x = x0 + (int)lroundf(dx * i / n), y = y0 + (int)lroundf(dy * i / n);
    d->fillCircle(x, y, INK_R, c);
  }
}

// Push a rectangle of the frame canvas to the panel (rows are contiguous in
// the canvas, so one call per row).
static void pushRect(Arduino_Canvas *cv, int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
  if (x1 < x0 || y1 < y0) return;
  uint16_t *fb = cv->getFramebuffer();
  for (int y = y0; y <= y1; y++) gfx->draw16bitRGBBitmap(x0, y, fb + y * W + x0, x1 - x0 + 1, 1);
}

// ---------------------------------------------------------------- view
void WriteView::onEnter() {
  if (!pts) {
    pts = (wrec::RawPt *)heap_caps_malloc(sizeof(wrec::RawPt) * kMaxPts,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pts) pts = (wrec::RawPt *)malloc(sizeof(wrec::RawPt) * kMaxPts);
  }
  uint32_t t0 = micros();
  bool ok = wcnn::init();
  if (!loaded) {
    loaded = true;
    Preferences p;
    if (p.begin("ewwrite", true)) {
      String t = p.getString("text", "");
      gapMs = p.getUShort("gapMs", 450);
      p.end();
      writeSetText(t.c_str());
    }
    if (gapMs < 200 || gapMs > 3000) gapMs = 450;
  }
  EWLOGI("WRITE", "enter ok=%d gap=%u init_us=%lu", ok ? 1 : 0, gapMs, (unsigned long)(micros() - t0));
  nPts = nStrokes = inkShown = 0;
  inStroke = false;
  press = Btn::None;
  wordLen = 0; wordOk = false;            // the word before the cursor isn't ours
  { char c = textLast(); if (c == 0 || c == ' ' || c == '\n') wordOk = true; }
  sLastInputMs = millis();
  full = true;
}

void WriteView::onExit() { saveText(); }

// Never 0: Write holds 240 MHz while open. Each 80<->240 MHz switch stalled
// the USB console for seconds (measured: silence right after "cpu_mhz=240"
// on touch down), and the glyph must be recognised fast anyway. Idle frames
// cost nothing (render() returns when there's no new ink).
uint16_t WriteView::desiredFrameMs() const {
  if (inStroke || press != Btn::None) return 16;
  return nStrokes > 0 ? 33 : 50;
}

void WriteView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch || e.type == EventType::TouchHold ||
      e.type == EventType::TouchUp) sLastInputMs = millis();

  switch (e.type) {
    case EventType::Touch:
      if (e.y < INK_Y) break;                                   // text line: nothing yet
      // Mid-letter the buttons are canvas too (a stroke of a big letter often
      // starts down there; it toggled shift in testing).
      press = nStrokes > 0 ? Btn::None
            : inBtn(e.x, e.y, SHIFT_X) ? Btn::Shift : inBtn(e.x, e.y, DEL_X) ? Btn::Del : Btn::None;
      if (press != Btn::None) {
        pressX = e.x; pressY = e.y; pressMs = repeatMs = millis(); repeating = false;
        full = true;                                            // pressed look
      } else {
        beginStroke(e.x, e.y);
      }
      break;
    case EventType::TouchHold:
      if (press != Btn::None) {
        if (abs((int)e.x - pressX) + abs((int)e.y - pressY) > kMoveSlop && !repeating) {
          press = Btn::None;                                    // it was ink after all
          full = true;
          beginStroke(pressX, pressY);
          addPoint(e.x, e.y);
        }
      } else if (inStroke) {
        addPoint(e.x, e.y);
      }
      break;
    case EventType::TouchUp:
      if (press == Btn::Shift) {
        uint32_t now = millis();
        if (now - lastShiftTapMs < kDoubleTap) caseMode = Case::Caps;
        else caseMode = caseMode == Case::Lower ? Case::Shift : Case::Lower;
        lastShiftTapMs = now;
        EWLOGI("WRITE", "case=%s", caseMode == Case::Caps ? "caps" : caseMode == Case::Shift ? "shift" : "lower");
        hapticBuzz(70, 12);
      } else if (press == Btn::Del) {
        if (!repeating) backspace(false);
      } else if (inStroke) {
        addPoint(e.x, e.y);
        endStroke();
      }
      if (press != Btn::None) { press = Btn::None; full = true; }
      break;
    default:
      break;                                                    // chip gestures: ink is the input
  }
}

void WriteView::beginStroke(int16_t x, int16_t y) {
  if (nStrokes >= kMaxStrokes || !pts) { inStroke = false; return; }
  if (nPts == 0 && textLast() == 0) full = true;                // clear the hint first
  if (y < INK_Y) y = INK_Y;
  inStroke = true;
  if (nPts < kMaxPts) pts[nPts++] = { x, y, (uint8_t)nStrokes };
}

void WriteView::addPoint(int16_t x, int16_t y) {
  if (!inStroke || nPts >= kMaxPts) return;
  if (y < INK_Y) y = INK_Y;
  const wrec::RawPt &p = pts[nPts - 1];
  if (p.stroke == nStrokes && abs(p.x - x) + abs(p.y - y) < 2) return;   // no motion
  pts[nPts++] = { x, y, (uint8_t)nStrokes };
}

void WriteView::endStroke() {
  inStroke = false;
  int first = nPts;
  while (first > 0 && pts[first - 1].stroke == nStrokes) first--;
  int16_t minX = 999, maxX = -999, minY = 999, maxY = -999;
  for (int i = first; i < nPts; i++) {
    if (pts[i].x < minX) minX = pts[i].x; if (pts[i].x > maxX) maxX = pts[i].x;
    if (pts[i].y < minY) minY = pts[i].y; if (pts[i].y > maxY) maxY = pts[i].y;
  }
  bool dot = nPts - first < 2 || (maxX - minX < 6 && maxY - minY < 6);
  if (dot && nStrokes == 0) { nPts = first; full = true; }     // a lone tap is not a glyph
  else if (dot) {                                               // i / j dot: one point
    pts[first].x = (maxX + minX) / 2; pts[first].y = (maxY + minY) / 2;
    nPts = first + 1; nStrokes++;
  } else nStrokes++;
  if (inkShown > nPts) inkShown = nPts;
  lastUpMs = millis();
}

void WriteView::typeChar(char c, const float *lp) {
  bool up = caseMode != Case::Lower;
  if (caseMode == Case::Shift) caseMode = Case::Lower;
  textAppend(up ? c : (char)tolower((unsigned char)c));
  if (wordOk && wordLen < wlm::kMaxWord) { memcpy(wordLp[wordLen], lp, sizeof wordLp[0]); wordLen++; }
  else wordOk = false;
  wordChecked = false;
  lastTypedMs = millis();
}

// Dictionary correction of the word before the cursor (keeping each
// letter's case). Run when the word ends (space) and when writing pauses.
void WriteView::correctWord() {
  wordChecked = true;
  if (wordOk && wordLen >= 2) {
    char t[kWriteTextMax + 1];
    size_t n = writeGetText(t, sizeof t);
    if ((size_t)wordLen <= n) {
      char *w = t + n - wordLen, typed[wlm::kMaxWord + 1], fixed[wlm::kMaxWord + 1];
      for (int i = 0; i < wordLen; i++) typed[i] = (char)toupper((unsigned char)w[i]);
      typed[wordLen] = fixed[wordLen] = 0;
      uint32_t t0 = micros();
      if (wlm::correct(typed, wordLen, wordLp, fixed)) {
        for (int i = 0; i < wordLen; i++) w[i] = islower((unsigned char)w[i]) ? (char)tolower(fixed[i]) : fixed[i];
        writeSetText(t);
        EWLOGI("WRITE", "correct from=%s to=%s us=%lu", typed, fixed, (unsigned long)(micros() - t0));
        logText();
        full = true;
      }
    }
  }
}

void WriteView::space() {
  correctWord();
  textAppend(' ');
  wordLen = 0; wordOk = true; wordChecked = false;
}

void WriteView::backspace(bool word) {
  do {
    char c = textPop();
    if (!c) break;
    if (c == ' ' || c == '\n') { wordLen = 0; wordOk = false; if (!word) break; continue; }
    if (wordLen > 0) wordLen--; else wordOk = false;
    if (word) { char p = textLast(); if (!p || p == ' ' || p == '\n') break; }
  } while (word);
  { char p = textLast(); if (!p || p == ' ' || p == '\n') { wordLen = 0; wordOk = true; } }
  EWLOGI("WRITE", "edit=backspace");
  hapticBuzz(80, 15);
  logText();
  full = true;
}

void WriteView::recognise() {
  uint32_t t0 = micros();
  int n = nPts, strokes = nStrokes;
  wrec::Edit ed = wrec::editGesture(pts, n);
  nPts = nStrokes = inkShown = 0;
  full = true;
  if (ed == wrec::Edit::Space || ed == wrec::Edit::Backspace) {
    if (ed == wrec::Edit::Space) { space(); EWLOGI("WRITE", "edit=space"); hapticBuzz(90, 20); logText(); }
    else backspace(false);
    return;
  }
  static float img[wcnn::kG * wcnn::kG];
  float prob[wcnn::kClasses], lp[wcnn::kClasses];
  if (!wcnn::rasterise(pts, n, img) || !wcnn::infer(img, prob)) return;
  int top = 0;
  for (int i = 0; i < wcnn::kClasses; i++) {
    lp[i] = logf(prob[i] > 1e-6f ? prob[i] : 1e-6f);
    if (prob[i] > prob[top]) top = i;
  }
  uint32_t us = micros() - t0;
  if (prob[top] < kReject) {
    EWLOGI("WRITE", "reject score=%.2f ms=%u strokes=%d pts=%d", prob[top], (unsigned)(us / 1000), strokes, n);
    hapticBuzz(140, 50);
    return;
  }
  // Context: the current word so far (upper case) picks among close calls.
  char word[wlm::kMaxWord + 1];
  int wl = 0;
  {
    char t[kWriteTextMax + 1];
    size_t tn = writeGetText(t, sizeof t);
    size_t s = tn;
    while (s > 0 && t[s - 1] != ' ' && t[s - 1] != '\n' && tn - s < (size_t)wlm::kMaxWord) s--;
    for (size_t i = s; i < tn; i++) word[wl++] = (char)toupper((unsigned char)t[i]);
  }
  int ci = wlm::decode(lp, word, wl);
  char c = wcnn::classChar(ci);
  typeChar(c, lp);
  int second = top == 0 ? 1 : 0;
  for (int i = 0; i < wcnn::kClasses; i++) if (i != top && prob[i] > prob[second]) second = i;
  word[wl] = 0;
  EWLOGI("WRITE", "char=%c score=%.2f ms=%u cands=%c:%.2f,%c:%.2f lm=%d ctx=%s us=%lu strokes=%d pts=%d",
         c, prob[ci], (unsigned)(us / 1000), wcnn::classChar(top), prob[top], wcnn::classChar(second),
         prob[second], ci != top ? 1 : 0, wl ? word : "-", (unsigned long)us, strokes, n);
  EWLOGD("WRITE", "stage_us mhz=%u raster=%lu c1=%lu c2=%lu c3=%lu fc=%lu", (unsigned)getCpuFrequencyMhz(),
         (unsigned long)wcnn::gStageUs[0], (unsigned long)wcnn::gStageUs[1], (unsigned long)wcnn::gStageUs[2],
         (unsigned long)wcnn::gStageUs[3], (unsigned long)wcnn::gStageUs[4]);
  hapticBuzz(60, 10);
  logText();
}

// New ink since the last frame: stamp it on the canvas and push only the
// rectangle it covers (a full-screen push per touch sample starved other tasks).
void WriteView::inkIncremental() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || inkShown >= nPts) return;
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  int from = inkShown > 0 ? inkShown : 0;
  for (int i = from; i < nPts; i++) {
    bool joined = i > 0 && pts[i].stroke == pts[i - 1].stroke && i - 1 >= 0;
    int ax = joined ? pts[i - 1].x : pts[i].x, ay = joined ? pts[i - 1].y : pts[i].y;
    bool live = inStroke && pts[i].stroke == nStrokes;
    inkSeg(cv, ax, ay, pts[i].x, pts[i].y, live ? aura::kText : aura::kText);
    int lx = ax < pts[i].x ? ax : pts[i].x, hx = ax > pts[i].x ? ax : pts[i].x;
    int ly = ay < pts[i].y ? ay : pts[i].y, hy = ay > pts[i].y ? ay : pts[i].y;
    if (lx < x0) x0 = lx; if (hx > x1) x1 = hx;
    if (ly < y0) y0 = ly; if (hy > y1) y1 = hy;
  }
  inkShown = nPts;
  pushRect(cv, x0 - INK_R - 1, y0 - INK_R - 1, x1 + INK_R + 1, y1 + INK_R + 1);
}

void WriteView::drawFull() {
  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *d = cv ? (Arduino_GFX *)cv : gfx;
  d->fillScreen(aura::kBg);

  // ---- text line: the tail that fits, the word being written in accent blue
  d->fillRoundRect(TXT_X, TXT_Y, TXT_W, TXT_H, 14, aura::kCard);
  char t[kWriteTextMax + 1];
  size_t len = writeGetText(t, sizeof t);
  const char *line = t;
  for (size_t i = 0; i < len; i++) if (t[i] == '\n') line = t + i + 1;
  d->setFont(&FreeSans12pt7b);
  d->setTextSize(1);
  const int16_t maxW = TXT_W - 30, baseY = TXT_Y + 33;
  const char *shown = line;
  int16_t bx, by; uint16_t bw, bh;
  while (*shown) {
    d->getTextBounds(shown, 0, baseY, &bx, &by, &bw, &bh);
    if ((int)bw <= maxW) break;
    shown++;
  }
  int16_t x = TXT_X + 14;
  if (!*line) {
    d->setTextColor(aura::kTextDim);
    d->setCursor(x + 10, baseY);
    d->print("Write here");
    x = TXT_X + 10;
  } else {
    // split: committed part (white) + current word (blue)
    const char *ws = shown + strlen(shown);
    while (ws > shown && ws[-1] != ' ') ws--;
    char head[kWriteTextMax + 1];
    size_t hn = ws - shown;
    memcpy(head, shown, hn); head[hn] = 0;
    d->setTextColor(aura::kText);
    d->setCursor(x, baseY);
    d->print(head);
    // trailing spaces aren't counted by getTextBounds: advance by the cursor
    x = d->getCursorX();
    d->setTextColor(wordLen > 0 ? aura::kBlue : aura::kText);
    d->print(ws);
    x = d->getCursorX();
  }
  d->fillRoundRect(x + 2, TXT_Y + 13, 3, 25, 1, aura::kBlue);   // cursor
  d->setFont(nullptr);

  // ---- buttons
  auto btn = [&](int cx, bool pressed, bool on) {
    uint16_t bg = pressed ? aura::kCardHi : (on ? aura::kText : aura::kCard);
    d->fillCircle(cx, BTN_Y, BTN_R, bg);
    return on ? aura::kBg : aura::kText;
  };
  // shift: an up arrow; filled when the next letter is upper case, underlined for caps lock
  uint16_t fg = btn(SHIFT_X, press == Btn::Shift, caseMode != Case::Lower);
  d->fillTriangle(SHIFT_X, BTN_Y - 11, SHIFT_X - 10, BTN_Y + 1, SHIFT_X + 10, BTN_Y + 1, fg);
  d->fillRect(SHIFT_X - 4, BTN_Y + 1, 9, 8, fg);
  if (caseMode == Case::Caps) d->fillRect(SHIFT_X - 7, BTN_Y + 12, 15, 3, fg);
  // delete: a left-pointing tag with an x
  fg = btn(DEL_X, press == Btn::Del, false);
  d->fillTriangle(DEL_X - 13, BTN_Y, DEL_X - 5, BTN_Y - 9, DEL_X - 5, BTN_Y + 9, fg);
  d->fillRoundRect(DEL_X - 6, BTN_Y - 9, 19, 19, 3, fg);
  uint16_t bgc = press == Btn::Del ? aura::kCardHi : aura::kCard;
  for (int o = -1; o <= 1; o++) {
    d->drawLine(DEL_X - 1 + o, BTN_Y - 4, DEL_X + 7 + o, BTN_Y + 4, bgc);
    d->drawLine(DEL_X - 1 + o, BTN_Y + 4, DEL_X + 7 + o, BTN_Y - 4, bgc);
  }

  // ---- hint while nothing is written yet
  if (len == 0 && nPts == 0) {
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    const char *h = "write one letter at a time";
    d->setCursor((W - (int16_t)strlen(h) * 6) / 2, 150);
    d->print(h);
    d->setTextColor(aura::kLine);
    const char *h2 = "swipe -> space   <- delete";
    d->setCursor((W - (int16_t)strlen(h2) * 6) / 2, 168);
    d->print(h2);
  }
  // ---- ink of the glyph in progress
  for (int i = 0; i < nPts; i++) {
    bool joined = i > 0 && pts[i].stroke == pts[i - 1].stroke;
    inkSeg(d, joined ? pts[i - 1].x : pts[i].x, joined ? pts[i - 1].y : pts[i].y, pts[i].x, pts[i].y, aura::kText);
  }
  inkShown = nPts;
  if (cv) cv->flush();
}

void WriteView::render() {
  if (!gfx) return;
  uint32_t now = millis();
  // Hold delete: repeat (whole words after the first few).
  if (press == Btn::Del && now - pressMs >= kRepeatDelay && now - repeatMs >= kRepeatEvery) {
    repeatMs = now;
    backspace(repeating && now - pressMs > kRepeatDelay + 6 * kRepeatEvery);
    repeating = true;
  }
  // Glyph finished? (finger up for gapMs)
  if (!inStroke && nStrokes > 0 && now - lastUpMs >= gapMs) recognise();
  // Writing paused: correct the word so far (the last word of a message gets
  // no space after it).
  // (millis() again: recognise() above may have just set lastTypedMs after `now`,
  // and the unsigned difference would wrap and fire at once.)
  if (!wordChecked && wordLen >= 2 && !inStroke && nStrokes == 0 && millis() - lastTypedMs >= kIdleCorrectMs)
    correctWord();
  if (full) { full = false; drawFull(); }
  else inkIncremental();
}
