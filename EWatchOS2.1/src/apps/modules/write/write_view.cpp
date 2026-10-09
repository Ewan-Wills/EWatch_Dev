// WriteView: handwriting input. Draw a letter or digit in the big area; when
// the finger stays up for gapMs (600 ms) the strokes are recognised ($P, see
// write_recog.cpp) and the character is appended to the shared text
// (write_text.h). Edit gestures: one straight stroke left->right = space,
// right->left = backspace, top-right->bottom-left = enter.
// Top bar: the text line, 3 candidate chips (tap = correct the last letter
// and learn that drawing), Aa (case), CLR (tap: clear text; long-press:
// also forget learned letters). Side button = back.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "write_view.h"
#include "write_text.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "ui_style.h"

// ---------------------------------------------------------------- layout
static const int16_t W = 240, H = 280;
static const int16_t TXT_X = 14, TXT_Y = 10, TXT_W = 212, TXT_H = 40;
static const int16_t CHIP_Y = 56, CHIP_H = 34;
static const int16_t CHIP_X[3] = { 12, 58, 104 };
static const int16_t CHIP_W = 42;
static const int16_t CASE_X = 152, CASE_W = 36;
static const int16_t CLR_X = 192, CLR_W = 36;
static const int16_t DRAW_Y = 96;                  // ink area: y >= DRAW_Y
static const char   *kTplPath = "/write_tpl.bin";
static const float   kRejectScore = 0.20f;         // below: not typed (see NOTES.md)

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

// Text edits from the view (render task).
static void textAppend(char c) {
  portENTER_CRITICAL(&sTextMux);
  if (sLen < kWriteTextMax) { sText[sLen++] = c; sText[sLen] = 0; sRev++; }
  portEXIT_CRITICAL(&sTextMux);
}
static void textBackspace() {
  portENTER_CRITICAL(&sTextMux);
  if (sLen > 0) { sText[--sLen] = 0; sRev++; }
  portEXIT_CRITICAL(&sTextMux);
}
static void textReplaceAt(int pos, char c) {
  portENTER_CRITICAL(&sTextMux);
  if (pos >= 0 && (size_t)pos < sLen) { sText[pos] = c; sRev++; }
  portEXIT_CRITICAL(&sTextMux);
}
static void logText() {                 // the last 47 chars, '\n' shown as '|'
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

// ---------------------------------------------------------------- helpers
static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Letter <-> digit look-alikes. When the top two candidates are such a pair
// and nearly tied, the previous typed character decides (digit after digit).
static char twinOf(char c) {
  switch (c) {
    case 'O': return '0'; case '0': return 'O';
    case 'I': return '1'; case '1': return 'I';
    case 'S': return '5'; case '5': return 'S';
    case 'Z': return '2'; case '2': return 'Z';
    case 'B': return '8'; case '8': return 'B';
    case 'G': return '6'; case '6': return 'G';
    default:  return 0;
  }
}

static void thickLine(Arduino_GFX *d, int x0, int y0, int x1, int y1, uint16_t c) {
  static const int8_t off[][2] = { {0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,-1},{1,-1},{-1,1} };
  for (auto &o : off) d->drawLine(x0 + o[0], y0 + o[1], x1 + o[0], y1 + o[1], c);
}

// ---------------------------------------------------------------- view
void WriteView::onEnter() {
  if (!pts) {
    pts = (wrec::RawPt *)heap_caps_malloc(sizeof(wrec::RawPt) * kMaxPts,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pts) pts = (wrec::RawPt *)malloc(sizeof(wrec::RawPt) * kMaxPts);
  }
  uint32_t t0 = micros();
  bool ok = wrec::init();
  if (!loaded) {
    loaded = true;
    Preferences p;
    if (p.begin("ewwrite", true)) {
      String t = p.getString("text", "");
      gapMs = p.getUShort("gapMs", 600);
      upper = p.getBool("upper", true);
      p.end();
      writeSetText(t.c_str());
    }
    if (gapMs < 250 || gapMs > 3000) gapMs = 600;
    loadUserTemplates();
  }
  EWLOGI("WRITE", "enter ok=%d tpl=%d user=%d gap=%u init_us=%lu", ok ? 1 : 0,
         wrec::builtinCount(), wrec::userCount(), gapMs, (unsigned long)(micros() - t0));
  nPts = nStrokes = 0;
  inStroke = uiTouch = false;
  nCands = 0; candPos = -1; status[0] = 0;
  sLastInputMs = millis();
  dirty = true;
}

void WriteView::onExit() {
  saveText();
  Preferences p;
  if (p.begin("ewwrite", false)) { p.putBool("upper", upper); p.end(); }
}

uint16_t WriteView::desiredFrameMs() const {
  return (inStroke || nStrokes > 0) ? 33 : 0;
}

void WriteView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch || e.type == EventType::TouchHold ||
      e.type == EventType::TouchUp) sLastInputMs = millis();

  switch (e.type) {
    case EventType::Touch:
      if (e.y < DRAW_Y - 2) { uiTouch = true; tapTopBar(e.x, e.y); }
      else { uiTouch = false; beginStroke(e.x, e.y); }
      break;
    case EventType::TouchHold:
      if (inStroke) addPoint(e.x, e.y);
      break;
    case EventType::TouchUp:
      if (inStroke) { addPoint(e.x, e.y); endStroke(); }
      uiTouch = false;
      break;
    case EventType::Gesture:
      // Long-press on CLR: forget learned letters. Every other chip gesture
      // (swipes while drawing, taps) is ignored: the ink is the input.
      if (e.gesture == Gesture::LongPress && inRect(e.x, e.y, CLR_X, CHIP_Y, CLR_W, CHIP_H)) {
        wrec::clearUser();
        LittleFS.remove(kTplPath);
        EWLOGI("WRITE", "forget user=0");
        snprintf(status, sizeof status, "forgot learned letters");
        hapticBuzz(160, 120);
        dirty = true;
      }
      break;
    default:
      break;
  }
}

void WriteView::beginStroke(int16_t x, int16_t y) {
  if (nStrokes >= kMaxStrokes || !pts) { inStroke = false; return; }
  inStroke = true;
  if (nPts < kMaxPts) pts[nPts++] = { x, y, (uint8_t)nStrokes };
  dirty = true;
}

void WriteView::addPoint(int16_t x, int16_t y) {
  if (!inStroke || nPts >= kMaxPts) return;
  if (y < DRAW_Y) y = DRAW_Y;                      // ink stays in the area
  const wrec::RawPt &p = pts[nPts - 1];
  if (p.stroke == nStrokes && abs(p.x - x) + abs(p.y - y) < 2) return;   // no motion
  pts[nPts++] = { x, y, (uint8_t)nStrokes };
  dirty = true;
}

void WriteView::endStroke() {
  inStroke = false;
  // A dot (tap) is not a stroke: drop it.
  int first = nPts;
  while (first > 0 && pts[first - 1].stroke == nStrokes) first--;
  int16_t minX = 999, maxX = -999, minY = 999, maxY = -999;
  for (int i = first; i < nPts; i++) {
    if (pts[i].x < minX) minX = pts[i].x; if (pts[i].x > maxX) maxX = pts[i].x;
    if (pts[i].y < minY) minY = pts[i].y; if (pts[i].y > maxY) maxY = pts[i].y;
  }
  if (nPts - first < 2 || (maxX - minX < 6 && maxY - minY < 6)) nPts = first;
  else nStrokes++;
  lastUpMs = millis();
  dirty = true;
}

void WriteView::recognise() {
  uint32_t t0 = micros();
  int n = nPts, strokes = nStrokes;
  wrec::Edit ed = wrec::editGesture(pts, n);
  nPts = nStrokes = 0;
  dirty = true;
  if (ed != wrec::Edit::None) {
    if (ed == wrec::Edit::Space) textAppend(' ');
    else if (ed == wrec::Edit::Enter) textAppend('\n');
    else textBackspace();
    uint32_t us = micros() - t0;
    EWLOGI("WRITE", "edit=%s ms=%u us=%lu", wrec::editName(ed), (unsigned)(us / 1000), (unsigned long)us);
    snprintf(status, sizeof status, "%s", wrec::editName(ed));
    nCands = 0; candPos = -1;
    hapticBuzz(90, 25);
    logText();
    return;
  }
  bool okN = wrec::normalize(pts, n, lastCloud);
  int nc = okN ? wrec::classify(lastCloud, cands, 3) : 0;
  uint32_t us = micros() - t0;

  // Letter/digit twin decided by the previous character.
  bool ctx = false;
  if (nc >= 2 && twinOf(cands[0].ch) == cands[1].ch && cands[0].score - cands[1].score < 0.06f) {
    char prev = 0;
    portENTER_CRITICAL(&sTextMux);
    if (sLen) prev = sText[sLen - 1];
    portEXIT_CRITICAL(&sTextMux);
    bool prevDigit = prev >= '0' && prev <= '9';
    bool prevAlpha = isalpha((unsigned char)prev);
    bool topDigit = cands[0].ch >= '0' && cands[0].ch <= '9';
    if ((prevDigit && !topDigit) || (prevAlpha && topDigit)) {
      wrec::Cand t = cands[0]; cands[0] = cands[1]; cands[1] = t;
      ctx = true;
    }
  }

  char cl[40] = "";
  for (int i = 0; i < nc; i++) {
    char b[12];
    snprintf(b, sizeof b, "%s%c:%.2f", i ? "," : "", cands[i].ch, cands[i].score);
    strncat(cl, b, sizeof cl - strlen(cl) - 1);
  }
  if (nc == 0 || cands[0].score < kRejectScore) {
    EWLOGI("WRITE", "reject score=%.2f ms=%u cands=%s strokes=%d pts=%d",
           nc ? cands[0].score : 0.f, (unsigned)(us / 1000), cl, strokes, n);
    snprintf(status, sizeof status, "? not sure - draw again");
    nCands = 0; candPos = -1;
    hapticBuzz(140, 60);
    return;
  }
  nCands = nc;
  candSel = 0;
  char c = cands[0].ch;
  textAppend(upper ? c : (char)tolower((unsigned char)c));
  candPos = (int)sLen - 1;
  EWLOGI("WRITE", "char=%c score=%.2f ms=%u cands=%s us=%lu strokes=%d pts=%d%s%s",
         c, cands[0].score, (unsigned)(us / 1000), cl, (unsigned long)us, strokes, n,
         ctx ? " ctx=1" : "", cands[0].user ? " user=1" : "");
  int pct = (int)(cands[0].score * 100 + 0.5f);
  if (us >= 1000) snprintf(status, sizeof status, "%c  %d%%  %lu ms", c, pct, (unsigned long)((us + 500) / 1000));
  else            snprintf(status, sizeof status, "%c  %d%%", c, pct);
  hapticBuzz(90, 20);
  logText();
}

void WriteView::tapTopBar(uint16_t x, uint16_t y) {
  for (int i = 0; i < 3; i++) {
    if (inRect(x, y, CHIP_X[i], CHIP_Y, CHIP_W, CHIP_H)) { applyCandidate(i); return; }
  }
  if (inRect(x, y, CASE_X, CHIP_Y, CASE_W, CHIP_H)) {
    upper = !upper;
    EWLOGI("WRITE", "case=%s", upper ? "upper" : "lower");
    hapticBuzz(90, 20);
    dirty = true;
    return;
  }
  if (inRect(x, y, CLR_X, CHIP_Y, CLR_W, CHIP_H)) {
    writeClearText();
    nCands = 0; candPos = -1; nPts = nStrokes = 0;
    status[0] = 0;
    EWLOGI("WRITE", "clear");
    saveText();
    hapticBuzz(120, 40);
    dirty = true;
  }
}

void WriteView::applyCandidate(int i) {
  if (i >= nCands || candPos < 0 || i == candSel) return;
  char from = cands[candSel].ch, to = cands[i].ch;
  textReplaceAt(candPos, upper ? to : (char)tolower((unsigned char)to));
  candSel = i;
  EWLOGI("WRITE", "fix from=%c to=%c", from, to);
  // Learn: this drawing is what `to` looks like for this writer.
  wrec::addUser(to, lastCloud);
  saveUserTemplates();
  EWLOGI("WRITE", "learn char=%c user=%d", to, wrec::userCount());
  snprintf(status, sizeof status, "%c -> %c (learned)", from, to);
  hapticBuzz(110, 30);
  logText();
  dirty = true;
}

// Temporary file buffer (4.7 KB) from PSRAM: internal RAM is kept for BLE.
static const size_t kTplFileMax = 5 + wrec::kMaxUser * (1 + 2 * wrec::kN);
static uint8_t *tplBuf() {
  uint8_t *b = (uint8_t *)heap_caps_malloc(kTplFileMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return b ? b : (uint8_t *)malloc(kTplFileMax);
}

void WriteView::saveUserTemplates() {
  uint8_t *buf = tplBuf();
  if (!buf) return;
  size_t n = wrec::userSerialize(buf, kTplFileMax);
  File f = LittleFS.open(kTplPath, "w");
  if (f) { f.write(buf, n); f.close(); }
  else EWLOGW("WRITE", "save_tpl=fail");
  free(buf);
}

void WriteView::loadUserTemplates() {
  if (!LittleFS.exists(kTplPath)) return;
  File f = LittleFS.open(kTplPath, "r");
  if (!f) return;
  uint8_t *buf = tplBuf();
  if (!buf) { f.close(); return; }
  size_t n = f.read(buf, kTplFileMax);
  f.close();
  if (!wrec::userDeserialize(buf, n)) EWLOGW("WRITE", "load_tpl=bad len=%u", (unsigned)n);
  free(buf);
}

void WriteView::render() {
  if (!gfx) return;
  // Glyph finished? (finger up for gapMs)
  if (!inStroke && nStrokes > 0 && millis() - lastUpMs >= gapMs) recognise();
  if (!dirty) return;
  dirty = false;

  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *d = cv ? (Arduino_GFX *)cv : gfx;
  d->fillScreen(aura::kBg);

  // ---- text line ----
  d->fillRoundRect(TXT_X, TXT_Y, TXT_W, TXT_H, 10, aura::kCard);
  char t[kWriteTextMax + 1];
  size_t len = writeGetText(t, sizeof t);
  const char *line = t;
  bool more = false;
  for (size_t i = 0; i < len; i++) if (t[i] == '\n') { line = t + i + 1; more = true; }
  int16_t x0 = TXT_X + 10;
  if (more) {                                        // return arrow: earlier lines exist
    d->drawFastVLine(x0 + 8, TXT_Y + 13, 9, aura::kTextDim);
    d->drawFastHLine(x0, TXT_Y + 21, 9, aura::kTextDim);
    d->drawLine(x0, TXT_Y + 21, x0 + 3, TXT_Y + 18, aura::kTextDim);
    d->drawLine(x0, TXT_Y + 21, x0 + 3, TXT_Y + 24, aura::kTextDim);
    x0 += 14;
  }
  int maxChars = (TXT_X + TXT_W - 10 - x0 - 4) / 12;
  int ll = (int)strlen(line);
  const char *shown = ll > maxChars ? line + (ll - maxChars) : line;
  d->setTextSize(2);
  d->setTextColor(aura::kText);
  d->setCursor(x0, TXT_Y + 13);
  d->print(shown);
  int16_t cx = x0 + (int16_t)strlen(shown) * 12 + 1;
  d->fillRect(cx, TXT_Y + 10, 2, 20, aura::kBlue);   // cursor
  if (len == 0) {
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    d->setCursor(x0 + 8, TXT_Y + 17);
    d->print("your text appears here");
  }

  // ---- candidate chips ----
  for (int i = 0; i < 3; i++) {
    bool has = i < nCands;
    bool sel = has && i == candSel;
    uint16_t bg = sel ? aura::kBlue : aura::kCard;
    d->fillRoundRect(CHIP_X[i], CHIP_Y, CHIP_W, CHIP_H, 9, bg);
    if (!has) { d->drawRoundRect(CHIP_X[i], CHIP_Y, CHIP_W, CHIP_H, 9, aura::kLine); continue; }
    char c = upper ? cands[i].ch : (char)tolower((unsigned char)cands[i].ch);
    d->setTextSize(3);
    d->setTextColor(sel ? aura::kText : aura::kText);
    d->setCursor(CHIP_X[i] + (CHIP_W - 18) / 2 + 1, CHIP_Y + 6);
    d->print(c);
  }
  // Aa + CLR
  d->fillRoundRect(CASE_X, CHIP_Y, CASE_W, CHIP_H, 9, aura::kCard);
  d->setTextSize(2);
  d->setTextColor(upper ? aura::kText : aura::kYellow);
  d->setCursor(CASE_X + 7, CHIP_Y + 10);
  d->print(upper ? "AB" : "ab");
  d->fillRoundRect(CLR_X, CHIP_Y, CLR_W, CHIP_H, 9, 0x5000 | 0x0008);   // dark red
  d->setTextSize(1);
  d->setTextColor(aura::kText);
  d->setCursor(CLR_X + 9, CHIP_Y + 13);
  d->print("CLR");

  // ---- drawing area ----
  d->drawFastHLine(16, DRAW_Y + 1, W - 32, aura::kLine);
  if (status[0]) {
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    d->setCursor(20, DRAW_Y + 8);
    d->print(status);
  }
  if (nPts == 0 && !inStroke) {
    d->setTextSize(2);
    d->setTextColor(aura::kLine);
    const char *h1 = "draw a letter";
    d->setCursor((W - (int16_t)strlen(h1) * 12) / 2, 170);
    d->print(h1);
    d->setTextSize(1);
    d->setTextColor(aura::kTextDim);
    const char *h2 = "-> space   <- delete   / enter";
    d->setCursor((W - (int16_t)strlen(h2) * 6) / 2, 246);
    d->print(h2);
  }
  // ink: finished strokes white, the glyph being written stays white too;
  // the current (still-down) stroke is accent blue.
  for (int i = 1; i < nPts; i++) {
    if (pts[i].stroke != pts[i - 1].stroke) continue;
    bool live = inStroke && pts[i].stroke == nStrokes;
    thickLine(d, pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, live ? aura::kBlue : aura::kText);
  }
  if (nPts == 1 && inStroke) d->fillCircle(pts[0].x, pts[0].y, 2, aura::kBlue);

  if (cv) cv->flush();
}
