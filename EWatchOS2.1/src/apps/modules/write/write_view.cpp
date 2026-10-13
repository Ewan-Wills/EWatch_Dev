// WriteView: handwriting input. Draw a letter or digit anywhere below the
// text. At each finger-up the strokes are classified (write_cnn); the letter
// is typed 200 ms later if it is a confident shape that is never the start of
// another letter, else after gapMs (400 ms) without a new stroke. English
// context (write_lm): letter trigram while writing, dictionary correction
// when the word ends (highlighted; one delete undoes it).
// A small dot = '.', a small single stroke = ',' (size relative to the
// writer's recent letters); both end the word and add a space.
// Gestures: one straight stroke left->right = space, right->left = delete.
// Buttons: shift (tap: next letter upper case, double-tap: caps lock;
// sentences start upper case by themselves), delete (tap: one char, hold:
// repeat) and, while the watch is linked to eWatch Cloud, send (centre).
// Side button = back.
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
#include "cloud_msg.h"
#include "FreeSans12pt7b.h"

// ---------------------------------------------------------------- layout
static const int16_t W = 240, H = 280;
static const int16_t TXT_X = 8, TXT_Y = 8, TXT_W = 224, TXT_H = 50;
static const int16_t INK_Y = TXT_Y + TXT_H + 4;          // ink anywhere below the text
static const int16_t BTN_R = 21;                         // button radius
static const int16_t SHIFT_X = 30, DEL_X = W - 30, SEND_X = W / 2, BTN_Y = H - 30;
static const int16_t INK_R = 3;                          // brush radius (px)
static const int16_t kMoveSlop = 10;                     // a press that moves this far is ink
static const float   kReject = 0.12f;                    // CNN top probability below: not typed
static const uint32_t kRepeatDelay = 420, kRepeatEvery = 110, kDoubleTap = 380;
static const uint32_t kIdleCorrectMs = 2500;             // pause after a letter: correct the word
// Early commit (ml-write/prefix.py on UJIpenchars2): at p >= 0.85 these classes
// came from an unfinished multi-stroke letter <= 2 % of the time; i, l, t, o,
// c, j, 1, 7... are often the first stroke of something else, so they wait gapMs.
// Held-out writers: 37 % of letters typed early, 0.3 % of unfinished ones cut.
static const char    kEarlySafe[] = "ABDEGHKMNPQWXY34568";
static const float   kEarlyProb = 0.85f;
static const uint32_t kEarlyMs = 200;
// Punctuation by size relative to the running letter size, and for ',' by
// shape (UJIpenchars2: '.' <= 0.27 of the writer's letters, ',' median 0.46,
// letters >= 0.43). Measured on all 60 writers: '.' 118/120, ',' 76/120, and
// 11 of 7440 letters taken for punctuation (small o/s/a).
static const float   kDotFrac = 0.22f, kCommaFrac = 0.42f;
static const float   kCommaTallFrac = 0.55f, kCommaAspect = 1.5f;   // a taller, downward stroke
// A bigger comma is told from a letter by context: a short downward stroke
// after a complete word, where the letter would make a non-word ("helloi").
static const float   kCommaCtxFrac = 0.8f, kCommaCtxAspect = 1.2f;
static const int16_t kDotPx = 12;
static const uint32_t kFadeStepMs = 45, kCorrHiMs = 800;

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
// end so a cut-off line is detectable). `write set <text>` replaces the text
// (use it before `goto Write`: an open Write view doesn't redraw for it).
bool writeConsole(const char *line) {
  if (strncmp(line, "write set ", 10) == 0) { writeSetText(line + 10); Serial.println("WTEXT set"); return true; }
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

// ---------------------------------------------------------------- CNN worker
// The CNN (~70 ms) runs on core 0 so the render task keeps drawing ink at
// 60 fps while it works (a second stroke started right after finger-up froze
// for 70 ms). One job at a time; wcnn's buffers are guarded by sCnnMutex.
static TaskHandle_t       sCnnTask = nullptr;
static SemaphoreHandle_t  sCnnMutex = nullptr;
static wrec::RawPt       *sJobPts = nullptr;
static volatile int       sJobN = 0;
static volatile uint32_t  sJobSeq = 0, sDoneSeq = 0;
static volatile bool      sJobOk = false;
static float              sJobProb[wcnn::kClasses];
static uint32_t           sTAnalyse, sTDraw, sTCommit;

static bool cnnRun(const wrec::RawPt *p, int n, float *prob) {
  static float img[wcnn::kG * wcnn::kG];
  xSemaphoreTake(sCnnMutex, portMAX_DELAY);
  uint32_t t0 = micros();
  bool ok = wcnn::rasterise(p, n, img) && wcnn::infer(img, prob);
  sTAnalyse = micros() - t0;
  xSemaphoreGive(sCnnMutex);
  return ok;
}

static void cnnTask(void *) {
  float prob[wcnn::kClasses];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t seq = sJobSeq;
    bool ok = cnnRun(sJobPts, sJobN, prob);
    memcpy(sJobProb, prob, sizeof prob);
    sJobOk = ok;
    sDoneSeq = seq;
  }
}

static bool cnnIdle() { return sDoneSeq == sJobSeq; }

// ---------------------------------------------------------------- view
static void *psram(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

static size_t textLen() {
  portENTER_CRITICAL(&sTextMux);
  size_t n = sLen;
  portEXIT_CRITICAL(&sTextMux);
  return n;
}

static bool sentenceStart() {
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  while (n > 0 && t[n - 1] == ' ') n--;
  return n == 0 || t[n - 1] == '.' || t[n - 1] == '\n';
}

void WriteView::sentenceCase() {
  if (caseMode == Case::Caps) return;
  if (sentenceStart()) caseMode = Case::Shift;
}

void WriteView::onEnter() {
  if (!pts) pts = (wrec::RawPt *)psram(sizeof(wrec::RawPt) * kMaxPts);
  if (!fadePts) fadePts = (wrec::RawPt *)psram(sizeof(wrec::RawPt) * kMaxPts);
  if (!sJobPts) sJobPts = (wrec::RawPt *)psram(sizeof(wrec::RawPt) * kMaxPts);
  if (!sCnnMutex) sCnnMutex = xSemaphoreCreateMutex();
  if (!sCnnTask && sJobPts && sCnnMutex)
    xTaskCreatePinnedToCore(cnnTask, "wcnn", 4096, nullptr, 2, &sCnnTask, 0);
  uint32_t t0 = micros();
  bool ok = wcnn::init();
  if (!loaded) {
    loaded = true;
    Preferences p;
    if (p.begin("ewwrite", true)) {
      String t = p.getString("text", "");
      gapMs = p.getUShort("gapMs", 350);
      p.end();
      writeSetText(t.c_str());
    }
    if (gapMs < 200 || gapMs > 3000) gapMs = 350;
  }
  EWLOGI("WRITE", "enter ok=%d gap=%u init_us=%lu", ok ? 1 : 0, gapMs, (unsigned long)(micros() - t0));
  nPts = nStrokes = inkShown = nFade = fadeStep = 0;
  inStroke = cacheOk = autoSpace = false;
  press = Btn::None;
  corrPos = -1; corrHiUntil = 0;
  wordLen = 0; wordOk = false;            // the word before the cursor isn't ours
  { char c = textLast(); if (c == 0 || c == ' ' || c == '\n') wordOk = true; }
  sentenceCase();
  canSend = cloudLinkState() == CloudLink::Linked;
  CloudContact r;
  if (canSend && cloudReplyTo(&r)) snprintf(replyName, sizeof replyName, "%s", r.name);
  else replyName[0] = 0;
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
  return (nStrokes > 0 || fadeStep) ? 16 : 50;           // pending: commit on time
}

void WriteView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    if (replyName[0]) { cloudSetReplyTo(nullptr); replyName[0] = 0; }
    switchTo(Screen::AppList);
    return;
  }
  if (e.type == EventType::Touch || e.type == EventType::TouchHold ||
      e.type == EventType::TouchUp) sLastInputMs = millis();

  switch (e.type) {
    case EventType::Touch:
      if (e.y < INK_Y) break;                                   // text line: nothing yet
      // A touch on a button is a press unless it moves (then it's ink, e.g. a
      // big letter's stroke starting down there). With ink still pending, a tap
      // on delete cancels that ink and a tap on shift types it first.
      press = inBtn(e.x, e.y, SHIFT_X) ? Btn::Shift : inBtn(e.x, e.y, DEL_X) ? Btn::Del
            : canSend && inBtn(e.x, e.y, SEND_X) ? Btn::Send : Btn::None;
      if (press != Btn::None) {
        pressX = e.x; pressY = e.y; pressMs = repeatMs = millis(); repeating = false;
        dirtyText = true;                                            // pressed look
      } else {
        beginStroke(e.x, e.y);
      }
      break;
    case EventType::TouchHold:
      if (press != Btn::None) {
        if (abs((int)e.x - pressX) + abs((int)e.y - pressY) > kMoveSlop && !repeating) {
          press = Btn::None;                                    // it was ink after all
          dirtyText = true;
          beginStroke(pressX, pressY);
          addPoint(e.x, e.y);
        }
      } else if (inStroke) {
        addPoint(e.x, e.y);
      }
      break;
    case EventType::TouchUp:
      if (press == Btn::Del && nStrokes > 0 && !inStroke) {
        startFade();
        nPts = nStrokes = inkShown = 0;
        cacheOk = false; wantJob = false; myJob = 0;
        EWLOGI("WRITE", "edit=cancel");
        hapticBuzz(80, 15);
        press = Btn::None; dirtyText = true;
        break;
      }
      if (press == Btn::Shift && nStrokes > 0 && !inStroke) commit();
      if (press == Btn::Shift) {
        uint32_t now = millis();
        if (now - lastShiftTapMs < kDoubleTap) caseMode = Case::Caps;
        else caseMode = caseMode == Case::Lower ? Case::Shift : Case::Lower;
        lastShiftTapMs = now;
        EWLOGI("WRITE", "case=%s", caseMode == Case::Caps ? "caps" : caseMode == Case::Shift ? "shift" : "lower");
        hapticBuzz(70, 12);
      } else if (press == Btn::Del) {
        if (!repeating) backspace(false);
      } else if (press == Btn::Send) {
        if (nStrokes > 0 && !inStroke) commit();
        press = Btn::None; dirtyText = true;
        send();
        break;
      } else if (inStroke) {
        addPoint(e.x, e.y);
        endStroke();
      }
      if (press != Btn::None) { press = Btn::None; dirtyText = true; }
      break;
    default:
      break;                                                    // chip gestures: ink is the input
  }
}

// ---------------------------------------------------------------- send
void WriteView::send() {
  if (sendWait) { hapticBuzz(140, 30); return; }               // one at a time
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\n')) n--;
  if (n == 0) { EWLOGI("WRITE", "send: empty"); hapticBuzz(140, 30); return; }
  CloudContact r;
  if (cloudReplyTo(&r)) { sendTo(r.id, r.name); return; }
  EWLOGI("WRITE", "send: pick contact");
  cloudPickContact(onPicked, this, (uint8_t)Screen::Write);
}

// Runs on the render task, before the picker switches back to Write.
void WriteView::onPicked(const CloudContact *c, void *ctx) {
  if (!c) { EWLOGI("WRITE", "send: cancelled"); return; }
  ((WriteView *)ctx)->sendTo(c->id, c->name);
}

void WriteView::sendTo(const char *id, const char *name) {
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\n')) n--;
  t[n] = 0;
  bool ok = n > 0 && cloudSendText(id, t);
  EWLOGI("WRITE", "send: to=%s name=%s chars=%u queued=%d", id, name, (unsigned)n, ok ? 1 : 0);
  if (!ok) { hapticBuzz(140, 50); return; }
  sendWait = true;
  sendAtMs = cloudSendStateMs();
  hapticBuzz(70, 15);
}

// Sent: clear the text (and the reply target). Failed: keep it to try again.
void WriteView::pollSend() {
  if (!sendWait || cloudSendStateMs() == sendAtMs) return;
  CloudSendState st = cloudSendState(nullptr, 0);
  if (st == CloudSendState::Sending || st == CloudSendState::Idle) return;
  sendWait = false;
  EWLOGI("WRITE", "send: %s", st == CloudSendState::Sent ? "sent" : "failed");
  if (st != CloudSendState::Sent) return;
  writeSetText("");
  saveText();
  if (replyName[0]) { cloudSetReplyTo(nullptr); replyName[0] = 0; }
  wordLen = 0; wordOk = true; corrPos = -1; corrHiUntil = 0; autoSpace = false;
  caseMode = Case::Shift;
  full = true;
}

void WriteView::beginStroke(int16_t x, int16_t y) {
  if (nStrokes >= kMaxStrokes || !pts) { inStroke = false; return; }
  if (nPts == 0 && textLast() == 0) full = true;                // clear the hint first
  if (y < INK_Y) y = INK_Y;
  inStroke = true;
  cacheOk = false; wantJob = false; myJob = 0;                  // the glyph is changing
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
  if (nPts - first < 2 || (maxX - minX < 6 && maxY - minY < 6)) {
    // A dot (tap): one point. Alone it is a full stop; after a stroke, an i/j dot.
    pts[first].x = (maxX + minX) / 2; pts[first].y = (maxY + minY) / 2;
    nPts = first + 1;
  }
  nStrokes++;
  if (inkShown > nPts) inkShown = nPts;
  cacheOk = false; myJob = 0;
  wantJob = true;                                   // classify it (worker, core 0)
  lastUpMs = millis();
}

// Classify the glyph as it stands (~70 ms), once per finger-up.


// Hand the glyph to the worker when it is free; pick the result up when ready.
void WriteView::analyse() {
  if (wantJob && cnnIdle() && sCnnTask) {
    memcpy(sJobPts, pts, sizeof(wrec::RawPt) * nPts);
    sJobN = nPts;
    myJob = ++sJobSeq;
    wantJob = false;
    xTaskNotifyGive(sCnnTask);
  }
  if (!cacheOk && myJob && sDoneSeq == myJob && !wantJob) {
    memcpy(cacheProb, sJobProb, sizeof cacheProb);
    cacheOk = sJobOk;
  }
}

void WriteView::typeChar(char c, const float *lp) {
  bool up = caseMode != Case::Lower;
  if (caseMode == Case::Shift) caseMode = Case::Lower;
  textAppend(up ? c : (char)tolower((unsigned char)c));
  if (wordOk && wordLen < wlm::kMaxWord) { memcpy(wordLp[wordLen], lp, sizeof wordLp[0]); wordLen++; }
  else wordOk = false;
  wordChecked = false;
  autoSpace = false;
  lastTypedMs = millis();
}

// Dictionary correction of the word before the cursor (keeping each
// letter's case). Run when the word ends and when writing pauses.
void WriteView::correctWord() {
  wordChecked = true;
  if (!wordOk || wordLen < 2) return;
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  if ((size_t)wordLen > n) return;
  char *w = t + n - wordLen, typed[wlm::kMaxWord + 1], fixed[wlm::kMaxWord + 1];
  for (int i = 0; i < wordLen; i++) typed[i] = (char)toupper((unsigned char)w[i]);
  typed[wordLen] = fixed[wordLen] = 0;
  uint32_t t0 = micros();
  if (!wlm::correct(typed, wordLen, wordLp, fixed)) return;
  memcpy(corrOrig, w, wordLen); corrOrig[wordLen] = 0;
  // Each letter keeps its case; a digit takes the case of the word's letters
  // (lower case when there are none: "701" -> "you").
  bool anyUpper = false, anyLower = false;
  for (int i = 0; i < wordLen; i++) {
    if (isupper((unsigned char)w[i])) anyUpper = true;
    if (islower((unsigned char)w[i])) anyLower = true;
  }
  for (int i = 0; i < wordLen; i++) {
    bool up = isalpha((unsigned char)w[i]) ? isupper((unsigned char)w[i]) : (anyUpper && !anyLower);
    w[i] = up ? fixed[i] : (char)tolower(fixed[i]);
  }
  writeSetText(t);
  corrPos = (int)(n - wordLen); corrLen = wordLen;
  corrTextLen = n;                       // callers that append update this
  corrHiUntil = millis() + kCorrHiMs;
  EWLOGI("WRITE", "correct from=%s to=%s us=%lu", typed, fixed, (unsigned long)(micros() - t0));
  hapticBuzz(120, 22);
  logText();
  dirtyText = true;
}

// One delete right after a correction puts back what was written.
bool WriteView::undoCorrection() {
  if (corrPos < 0) return false;
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  int pos = corrPos;
  corrPos = -1;
  if (n != corrTextLen || (size_t)(pos + corrLen) > n) return false;
  memcpy(t + pos, corrOrig, corrLen);
  writeSetText(t);
  if ((size_t)(pos + corrLen) == n) { wordOk = false; wordChecked = true; }   // still the current word: leave it
  corrHiUntil = 0;
  EWLOGI("WRITE", "undo correction=%s", corrOrig);
  hapticBuzz(100, 18);
  logText();
  dirtyText = true;
  return true;
}

void WriteView::space() {
  if (autoSpace && textLast() == ' ') {   // after . or , the space is already there
    autoSpace = false;
    EWLOGI("WRITE", "edit=space kept=1");
    hapticBuzz(90, 16);
    return;
  }
  corrPos = -1;                                      // an older correction can't be undone past this
  correctWord();
  textAppend(' ');
  if (corrPos >= 0) corrTextLen = textLen();         // undo allowed right after the space
  wordLen = 0; wordOk = true; wordChecked = false; autoSpace = false;
  EWLOGI("WRITE", "edit=space");
  if (corrPos < 0) hapticBuzz(90, 16);              // (a correction buzzed already)
  logText();
  dirtyText = true;
}

// '.' or ',': ends the word (corrected), no space before it, one after it.
// Inside a number ("3.5") it is just the character.
void WriteView::punctuate(char c) {
  corrPos = -1;
  correctWord();                                      // may turn "701" into "you" first
  char t[kWriteTextMax + 1];
  size_t n = writeGetText(t, sizeof t);
  size_t s = n;
  while (s > 0 && t[s - 1] != ' ' && t[s - 1] != '\n') s--;
  bool number = s < n;
  for (size_t i = s; i < n; i++) if (!isdigit((unsigned char)t[i])) number = false;
  if (number) {
    textAppend(c);
    wordOk = false;
  } else {
    if (textLast() == ' ') textPop();                 // "word ," -> "word,"
    textAppend(c);
    textAppend(' ');
    if (corrPos >= 0) corrTextLen = textLen();        // undo still allowed right after
    wordLen = 0; wordOk = true; wordChecked = false;
    autoSpace = true;
    if (c == '.') sentenceCase();
  }
  EWLOGI("WRITE", "punct=%c", c);
  hapticBuzz(75, 12);
  logText();
  dirtyText = true;
}

void WriteView::backspace(bool word) {
  if (!word && undoCorrection()) return;
  corrPos = -1;
  if (textLen() == 0) return;                       // nothing to delete (hold on empty)
  do {
    char c = textPop();
    if (!c) break;
    if (c == ' ' || c == '\n') { wordLen = 0; wordOk = false; if (!word) break; continue; }
    if (wordLen > 0) wordLen--; else wordOk = false;
    if (word) { char p = textLast(); if (!p || p == ' ' || p == '\n') break; }
  } while (word);
  { char p = textLast(); if (!p || p == ' ' || p == '\n') { wordLen = 0; wordOk = true; } }
  autoSpace = false;
  sentenceCase();
  EWLOGI("WRITE", "edit=backspace");
  hapticBuzz(80, 15);
  logText();
  dirtyText = true;
}

// The glyph is done: edit gesture, punctuation, or a character.
void WriteView::commit() {
  uint32_t t0 = micros();
  uint32_t upAgo = millis() - lastUpMs;
  int n = nPts, strokes = nStrokes;
  int16_t minX = 999, maxX = -999, minY = 999, maxY = -999;
  for (int i = 0; i < n; i++) {
    if (pts[i].x < minX) minX = pts[i].x; if (pts[i].x > maxX) maxX = pts[i].x;
    if (pts[i].y < minY) minY = pts[i].y; if (pts[i].y > maxY) maxY = pts[i].y;
  }
  int16_t gw = maxX - minX, gh = maxY - minY;
  int16_t size = gw > gh ? gw : gh;
  bool downward = n > 1 && pts[n - 1].y > pts[0].y;
  wrec::Edit ed = wrec::editGesture(pts, n);
  bool early = millis() - lastUpMs < gapMs;
  bool cached = cacheOk;
  startFade();
  nPts = nStrokes = inkShown = 0;
  cacheOk = false; wantJob = false; myJob = 0;
  dirtyText = true;

  if (ed == wrec::Edit::Space) { space(); return; }
  if (ed == wrec::Edit::Backspace) { backspace(false); return; }
  float dotMax = kDotFrac * letterSize;
  if (dotMax < kDotPx) dotMax = kDotPx;
  if (size <= dotMax) { punctuate('.'); return; }
  if (strokes == 1 && (size <= kCommaFrac * letterSize ||
      (size <= kCommaTallFrac * letterSize && gh >= kCommaAspect * gw && downward))) {
    punctuate(',');
    return;
  }

  static float img[wcnn::kG * wcnn::kG];
  float prob[wcnn::kClasses], lp[wcnn::kClasses];
  (void)img;
  if (cached) memcpy(prob, cacheProb, sizeof prob);
  else if (!cnnRun(fadePts, nFade, prob)) return;          // worker late: do it here
  int top = 0;
  for (int i = 0; i < wcnn::kClasses; i++) {
    lp[i] = logf(prob[i] > 1e-6f ? prob[i] : 1e-6f);
    if (prob[i] > prob[top]) top = i;
  }
  uint32_t us = micros() - t0;
  if (prob[top] < kReject) {
    EWLOGI("WRITE", "reject score=%.2f strokes=%d pts=%d", prob[top], strokes, n);
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
  if (strokes == 1 && wl >= 1 && downward && size <= kCommaCtxFrac * letterSize && gh >= kCommaCtxAspect * gw) {
    char next[wlm::kMaxWord + 2];
    memcpy(next, word, wl); next[wl] = c;
    float fCur, fNext;
    int sCur = wlm::wordStatus(word, wl, &fCur), sNext = wlm::wordStatus(next, wl + 1, &fNext);
    // "ok" + i: "oki" is listed but far rarer than "ok" -> still a comma.
    bool nextUnlikely = !(sNext & wlm::kPrefix) && (!(sNext & wlm::kWord) || fCur - fNext > 3.0f);
    if ((sCur & wlm::kWord) && nextUnlikely) {
      EWLOGI("WRITE", "comma_ctx letter=%c score=%.2f", c, prob[ci]);
      punctuate(',');
      return;
    }
  }
  corrPos = -1;
  typeChar(c, lp);
  letterSize = 0.8f * letterSize + 0.2f * size;
  int second = top == 0 ? 1 : 0;
  for (int i = 0; i < wcnn::kClasses; i++) if (i != top && prob[i] > prob[second]) second = i;
  word[wl] = 0;
  EWLOGI("WRITE", "char=%c score=%.2f early=%d cands=%c:%.2f,%c:%.2f lm=%d ctx=%s us=%lu strokes=%d pts=%d size=%d",
         c, prob[ci], early ? 1 : 0, wcnn::classChar(top), prob[top], wcnn::classChar(second),
         prob[second], ci != top ? 1 : 0, wl ? word : "-", (unsigned long)us, strokes, n, size);
  hapticBuzz(55, 8);
  logText();
  sTCommit = micros() - t0;
  EWLOGD("WRITE", "timing up_to_commit=%lu analyse_us=%lu commit_us=%lu last_draw_us=%lu",
         (unsigned long)upAgo, (unsigned long)sTAnalyse, (unsigned long)sTCommit, (unsigned long)sTDraw);
}

// ---------------------------------------------------------------- fading ink
static void drawInk(Arduino_GFX *d, const wrec::RawPt *p, int n, uint16_t col);
static const uint16_t kFadeCol[3] = { aura::kTextDim, aura::kLine, aura::kBg };

void WriteView::startFade() {
  nFade = 0; fadeStep = 0;
  if (!fadePts || nPts == 0) return;
  memcpy(fadePts, pts, sizeof(wrec::RawPt) * nPts);
  nFade = nPts;
  int16_t x0 = 999, y0 = 999, x1 = -999, y1 = -999;
  for (int i = 0; i < nFade; i++) {
    if (fadePts[i].x < x0) x0 = fadePts[i].x; if (fadePts[i].x > x1) x1 = fadePts[i].x;
    if (fadePts[i].y < y0) y0 = fadePts[i].y; if (fadePts[i].y > y1) y1 = fadePts[i].y;
  }
  fadeBox[0] = x0 - INK_R - 1; fadeBox[1] = y0 - INK_R - 1;
  fadeBox[2] = x1 + INK_R + 1; fadeBox[3] = y1 + INK_R + 1;
  fadeStep = 1;                     // first fade colour now (drawFull also uses it)
  fadeMs = millis();
  Arduino_Canvas *cv = frameCanvas();
  if (cv) {
    drawInk(cv, fadePts, nFade, kFadeCol[0]);
    pushRect(cv, fadeBox[0], fadeBox[1], fadeBox[2], fadeBox[3]);
  }
}

static void drawInk(Arduino_GFX *d, const wrec::RawPt *p, int n, uint16_t col) {
  for (int i = 0; i < n; i++) {
    bool joined = i > 0 && p[i].stroke == p[i - 1].stroke;
    inkSeg(d, joined ? p[i - 1].x : p[i].x, joined ? p[i - 1].y : p[i].y, p[i].x, p[i].y, col);
  }
}

// Repaint the old glyph a shade darker (only its rectangle goes to the panel);
// the last step is a full redraw, which also restores anything it overlapped.
void WriteView::stepFade(uint32_t now) {
  if (!fadeStep || now - fadeMs < kFadeStepMs) return;
  fadeMs = now;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) { fadeStep = 0; full = true; return; }
  if (++fadeStep >= 3) {
    // Gone: clear its box (below the text line), restore what it covered.
    fadeStep = 0;
    int16_t y0 = fadeBox[1] < INK_Y ? INK_Y : fadeBox[1];
    cv->fillRect(fadeBox[0], y0, fadeBox[2] - fadeBox[0] + 1, fadeBox[3] - y0 + 1, aura::kBg);
    drawButtonsOn(cv);
    drawInk(cv, pts, nPts, aura::kText);                    // a new glyph may already be under way
    pushRect(cv, fadeBox[0], y0, fadeBox[2], fadeBox[3]);
    return;
  }
  drawInk(cv, fadePts, nFade, kFadeCol[fadeStep - 1]);
  drawInk(cv, pts, nPts, aura::kText);                      // new ink stays on top
  pushRect(cv, fadeBox[0], fadeBox[1], fadeBox[2], fadeBox[3]);
}

// New ink since the last frame: stamp it on the canvas and push only the
// rectangle it covers (a full-screen push per touch sample starved other tasks).
void WriteView::inkIncremental() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || inkShown >= nPts) return;
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (int i = inkShown; i < nPts; i++) {
    bool joined = i > 0 && pts[i].stroke == pts[i - 1].stroke;
    int ax = joined ? pts[i - 1].x : pts[i].x, ay = joined ? pts[i - 1].y : pts[i].y;
    inkSeg(cv, ax, ay, pts[i].x, pts[i].y, aura::kText);
    int lx = ax < pts[i].x ? ax : pts[i].x, hx = ax > pts[i].x ? ax : pts[i].x;
    int ly = ay < pts[i].y ? ay : pts[i].y, hy = ay > pts[i].y ? ay : pts[i].y;
    if (lx < x0) x0 = lx; if (hx > x1) x1 = hx;
    if (ly < y0) y0 = ly; if (hy > y1) y1 = hy;
  }
  inkShown = nPts;
  pushRect(cv, x0 - INK_R - 1, y0 - INK_R - 1, x1 + INK_R + 1, y1 + INK_R + 1);
}

// Text line: the tail that fits. The word being written is blue, a
// just-corrected word green.
void WriteView::drawTextOn(Arduino_GFX *d) {
  d->fillRect(0, 0, W, INK_Y, aura::kBg);
  d->fillRoundRect(TXT_X, TXT_Y, TXT_W, TXT_H, 14, aura::kCard);
  char t[kWriteTextMax + 1];
  size_t len = writeGetText(t, sizeof t);
  size_t lineStart = 0;
  for (size_t i = 0; i < len; i++) if (t[i] == '\n') lineStart = i + 1;
  d->setFont(&FreeSans12pt7b);
  d->setTextSize(1);
  const int16_t maxW = TXT_W - 30, baseY = TXT_Y + 33;
  size_t from = lineStart;
  int16_t bx, by; uint16_t bw, bh;
  while (from < len) {
    d->getTextBounds(t + from, 0, baseY, &bx, &by, &bw, &bh);
    if ((int)bw <= maxW) break;
    from++;
  }
  int16_t x = TXT_X + 14;
  if (lineStart == len) {
    d->setTextColor(aura::kTextDim);
    d->setCursor(x + 10, baseY);
    d->print("Write here");
    x = TXT_X + 10;
  } else {
    size_t wordFrom = len >= (size_t)wordLen ? len - wordLen : len;
    bool hi = corrPos >= 0 && corrHiUntil && millis() < corrHiUntil;
    d->setCursor(x, baseY);
    for (size_t i = from; i < len; i++) {
      uint16_t col = aura::kText;
      if (wordLen > 0 && i >= wordFrom) col = aura::kBlue;
      if (hi && (int)i >= corrPos && (int)i < corrPos + corrLen) col = aura::kGreen;
      d->setTextColor(col);
      d->print(t[i]);
    }
    x = d->getCursorX();
  }
  d->fillRoundRect(x + 2, TXT_Y + 13, 3, 25, 1, aura::kBlue);   // cursor
  d->setFont(nullptr);
}

void WriteView::drawButtonsOn(Arduino_GFX *d) {
  auto btn = [&](int cx, bool pressed, bool on) {
    d->fillCircle(cx, BTN_Y, BTN_R + 1, aura::kBg);
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
  if (!canSend) return;
  // send: a paper plane on blue; "to Name" above it for a reply
  d->fillCircle(SEND_X, BTN_Y, BTN_R + 1, aura::kBg);
  d->fillCircle(SEND_X, BTN_Y, BTN_R, press == Btn::Send ? aura::kCardHi : aura::kBlue);
  d->fillTriangle(SEND_X - 11, BTN_Y - 9, SEND_X + 12, BTN_Y, SEND_X - 11, BTN_Y + 9, aura::kText);
  d->fillTriangle(SEND_X - 11, BTN_Y - 9, SEND_X - 5, BTN_Y, SEND_X - 11, BTN_Y + 9,
                  press == Btn::Send ? aura::kCardHi : aura::kBlue);
  d->drawLine(SEND_X - 5, BTN_Y, SEND_X + 4, BTN_Y, press == Btn::Send ? aura::kCardHi : aura::kBlue);
  if (replyName[0]) {
    char lab[32];
    snprintf(lab, sizeof lab, "to %s", replyName);
    int16_t tw = (int16_t)strlen(lab) * 6;
    d->fillRect(SEND_X - 60, BTN_Y - BTN_R - 14, 120, 10, aura::kBg);
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    d->setCursor(SEND_X - tw / 2, BTN_Y - BTN_R - 13);
    d->print(lab);
  }
}

// After a letter/space/delete: only the text line and the buttons change.
// (A full-screen flush took 61 ms per letter.)
void WriteView::drawTextAndButtons() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) { full = true; return; }
  drawTextOn(cv);
  drawButtonsOn(cv);
  pushRect(cv, 0, 0, W - 1, INK_Y - 1);
  pushRect(cv, SHIFT_X - BTN_R - 1, BTN_Y - BTN_R - 1, SHIFT_X + BTN_R + 1, BTN_Y + BTN_R + 1);
  pushRect(cv, DEL_X - BTN_R - 1, BTN_Y - BTN_R - 1, DEL_X + BTN_R + 1, BTN_Y + BTN_R + 1);
  if (canSend) pushRect(cv, SEND_X - 60, BTN_Y - BTN_R - 14, SEND_X + 60, BTN_Y + BTN_R + 1);
}

void WriteView::drawFull() {
  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *d = cv ? (Arduino_GFX *)cv : gfx;
  d->fillScreen(aura::kBg);
  drawTextOn(d);
  drawButtonsOn(d);
  char t[2];
  size_t len = writeGetText(t, sizeof t);
  // ---- hint while nothing is written yet
  if (len == 0 && nPts == 0 && !fadeStep) {
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    const char *h = "write one letter at a time";
    d->setCursor((W - (int16_t)strlen(h) * 6) / 2, 144);
    d->print(h);
    d->setTextColor(aura::kLine);
    const char *h2 = "swipe -> space   <- delete";
    d->setCursor((W - (int16_t)strlen(h2) * 6) / 2, 162);
    d->print(h2);
    const char *h3 = "small dot .   small tick ,";
    d->setCursor((W - (int16_t)strlen(h3) * 6) / 2, 176);
    d->print(h3);
  }
  // ---- the last glyph fading, then the glyph in progress
  if (fadeStep) drawInk(d, fadePts, nFade, kFadeCol[fadeStep - 1]);
  drawInk(d, pts, nPts, aura::kText);
  inkShown = nPts;
  dirtyText = false;
  if (cv) cv->flush();
}

void WriteView::render() {
  if (!gfx) return;
  pollSend();
  uint32_t now = millis();
  // Hold delete: repeat (whole words after the first few).
  if (press == Btn::Del && now - pressMs >= kRepeatDelay && now - repeatMs >= kRepeatEvery) {
    repeatMs = now;
    backspace(repeating && now - pressMs > kRepeatDelay + 6 * kRepeatEvery);
    repeating = true;
  }
  // Glyph pending: classify it once, then commit early (confident shape that
  // never starts another letter) or after gapMs.
  if (!inStroke && nStrokes > 0) {
    analyse();
    uint32_t wait = gapMs;
    if (cacheOk) {
      int top = 0;
      for (int i = 1; i < kClasses; i++) if (cacheProb[i] > cacheProb[top]) top = i;
      if (cacheProb[top] >= kEarlyProb && strchr(kEarlySafe, wcnn::classChar(top))) wait = kEarlyMs;
    }
    if (millis() - lastUpMs >= wait) commit();
  }
  // Writing paused: correct the word so far (the last word of a message gets
  // no space after it). (millis() again: commit() may have just set
  // lastTypedMs after `now`, and the unsigned difference would wrap.)
  if (!wordChecked && wordLen >= 2 && !inStroke && nStrokes == 0 && millis() - lastTypedMs >= kIdleCorrectMs) {
    correctWord();
  }
  if (corrHiUntil && millis() >= corrHiUntil) { corrHiUntil = 0; dirtyText = true; }
  uint32_t td = micros();
  if (full) { full = false; drawFull(); sTDraw = micros() - td; }
  else {
    if (dirtyText) { dirtyText = false; drawTextAndButtons(); sTDraw = micros() - td; }
    stepFade(millis());
    inkIncremental();
  }
}
