// src/apps/llmterm.cpp — see llmterm.h for the protocol + design overview.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "llmterm.h"
#include "display.h"   // gfx, frameCanvas()
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock
#include "view.h"      // switchTo, theme, drawTitleBar, drawBackButton, tappedBack

// ---------------------------------------------------------------------------
// Layout / tuning
// ---------------------------------------------------------------------------
static constexpr int16_t SCRN_W = 240;
static constexpr int16_t SCRN_H = 280;

// Canned prompts (MVP input). On-watch free-text entry is deliberately omitted
// — tap a row to send it. Keep each comfortably under ~38 chars so the menu
// rows read cleanly.
static const char *const kPrompts[] = {
  "Tell me a short joke",
  "Why is the sky blue?",
  "Write a haiku about watches",
  "Give me a fun fact",
  "Explain recursion simply",
};
static constexpr int kPromptCount = sizeof(kPrompts) / sizeof(kPrompts[0]);

// Terminal body uses the built-in 6x8 bitmap font scaled x2 (12x20 cell inc.
// line spacing), giving a crisp fixed-pitch "terminal" grid.
static constexpr int16_t kTxtSize = 2;
static constexpr int16_t kCharW   = 6 * kTxtSize;        // 12 px advance
static constexpr int16_t kCharH   = 8 * kTxtSize;        // 16 px glyph
static constexpr int16_t kLineH   = kCharH + 4;          // 20 px row pitch
static constexpr int16_t kBodyTop = 28;                  // below the header
static constexpr int16_t kBodyX   = 6;
static constexpr int16_t kCols    = (SCRN_W - 2 * kBodyX) / kCharW;        // 19
static constexpr int16_t kRows    = (SCRN_H - kBodyTop - 6) / kLineH;      // 12

// Menu row geometry.
static constexpr int16_t kRowTop = 56;
static constexpr int16_t kRowH   = 40;
static constexpr int16_t kRowGap = 4;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void LlmTermView::onEnter() {
  mode = Mode::Menu;
  menuDirty = true;
  resetChat();
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
}

void LlmTermView::onExit() {
  // Stop treating a half-finished stream as live; nothing else to release
  // (the frame canvas is shared and owned by the display driver).
  streaming = false;
}

void LlmTermView::resetChat() {
  respLen = 0; resp[0] = 0;
  streaming = false; sawAnyTok = false; errState = false;
  lineLen = 0; line[0] = 0;
  promptText = nullptr;
}

// ---------------------------------------------------------------------------
// Serial protocol
// ---------------------------------------------------------------------------
void LlmTermView::sendPrompt(const char *text) {
  resetChat();
  promptText = text;
  streaming  = true;
  // Flush any stale RX bytes so a previous run's tail can't leak in.
  while (Serial.available()) (void)Serial.read();
  Serial.print("PROMPT ");
  Serial.print(text);
  Serial.print('\n');
  // Kick the render loop so it starts pumping immediately.
  { ModelLock lk; model.revision++; }
}

void LlmTermView::pumpSerial() {
  // Drain whatever is buffered; keep it bounded so we never starve the WDT.
  int budget = 1024;
  while (budget-- > 0 && Serial.available() > 0) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = 0;
      handleLine(line);
      lineLen = 0;
      continue;
    }
    if (lineLen < kLineCap - 1) line[lineLen++] = c;
    // If a line overflows kLineCap we simply stop storing until the next '\n';
    // protocol lines are far shorter than this in practice.
  }
}

void LlmTermView::handleLine(const char *s) {
  if (strncmp(s, ">>TOK ", 6) == 0) {
    sawAnyTok = true;
    appendTokenEscaped(s + 6);
  } else if (strncmp(s, ">>END", 5) == 0) {
    streaming = false;
  } else if (strncmp(s, ">>ERR ", 6) == 0) {
    errState  = false;  // keep partial text visible, append the error inline
    if (respLen == 0) { /* fresh */ }
    const char *msg = s + 6;
    // Prefix a newline so the error sits on its own line.
    if (respLen < kRespCap) resp[respLen++] = '\n';
    while (*msg && respLen < kRespCap) resp[respLen++] = *msg++;
    resp[respLen] = 0;
    errState  = true;
    streaming = false;
  }
  // Any other line (>>READY, boot log, blank) is ignored by design.
}

void LlmTermView::appendTokenEscaped(const char *chunk) {
  for (const char *p = chunk; *p && respLen < kRespCap; ++p) {
    char c = *p;
    if (c == '\\') {
      char n = *(p + 1);
      if (n == 'n')      { c = '\n'; ++p; }
      else if (n == 't') { c = '\t'; ++p; }
      else if (n == '\\'){ c = '\\'; ++p; }
      // else: a lone backslash — emit it literally.
    }
    if (c == '\t') c = ' ';           // tabs don't map onto the cell grid
    resp[respLen++] = c;
  }
  resp[respLen] = 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void LlmTermView::render() {
  if (!gfx) return;

  if (mode == Mode::Chat) {
    pumpSerial();
    drawChat();
    // Keep the render loop hot while a stream is in flight so tokens keep
    // flowing and the caret blinks. Stops bumping once >>END/>>ERR arrives.
    if (streaming) { ModelLock lk; model.revision++; }
    return;
  }

  // Menu — static, so paint only when flagged dirty (avoids 1 Hz flicker).
  if (menuDirty) { drawMenu(); menuDirty = false; }
}

void LlmTermView::drawMenu() {
  ThemeColors t = theme();
  drawTitleBar("LLM Terminal");     // clears screen, draws back chevron + title
  uint16_t rowBg  = t.line;
  uint16_t rowTxt = contrastFor(rowBg);
  gfx->setFont(nullptr);
  gfx->setTextSize(kTxtSize);
  for (int i = 0; i < kPromptCount; ++i) {
    int16_t y = kRowTop + i * (kRowH + kRowGap);
    gfx->fillRoundRect(kBodyX, y, SCRN_W - 2 * kBodyX, kRowH, 6, rowBg);
    gfx->setTextColor(rowTxt, rowBg);
    // Truncate to the row width (19 cols) so text never overruns the pill.
    char buf[kCols + 1];
    int n = 0;
    for (const char *p = kPrompts[i]; *p && n < kCols; ++p) buf[n++] = *p;
    buf[n] = 0;
    int16_t ty = y + (kRowH - kCharH) / 2;
    gfx->setCursor(kBodyX + 6, ty);
    gfx->print(buf);
  }
}

void LlmTermView::drawChat() {
  // Prefer the double-buffered canvas (flicker-free scrolling); fall back to
  // drawing straight to the panel if PSRAM is exhausted.
  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *g = cv ? (Arduino_GFX *)cv : gfx;
  ThemeColors t = theme();

  g->fillScreen(t.bg);

  // Header: back chevron + a short prompt label.
  uint16_t chTxt = contrastFor(t.accent);
  g->fillRoundRect(2, 2, 44, 22, 5, t.accent);
  g->setFont(nullptr);
  g->setTextColor(chTxt, t.accent);
  g->setTextSize(2);
  g->setCursor(14, 5);
  g->print('<');
  g->setTextColor(t.accent, t.bg);
  g->setTextSize(1);
  g->setCursor(54, 9);
  if (promptText) {
    char hdr[28]; int n = 0;
    for (const char *p = promptText; *p && n < 27; ++p) hdr[n++] = *p;
    hdr[n] = 0;
    g->print(hdr);
  } else {
    g->print("LLM");
  }
  g->drawFastHLine(0, 26, SCRN_W, t.line);

  // Word-wrap the response into a ring of the last kRows lines.
  String ring[kRows];
  int ringCount = 0;
  auto push = [&](const String &ln) {
    if (ringCount < kRows) { ring[ringCount++] = ln; }
    else { for (int r = 1; r < kRows; ++r) ring[r - 1] = ring[r]; ring[kRows - 1] = ln; }
  };

  String cur;
  uint32_t i = 0;
  while (i < respLen) {
    char c = resp[i];
    if (c == '\n') { push(cur); cur = ""; ++i; continue; }
    if (c == ' ') {
      if ((int)cur.length() >= kCols) { push(cur); cur = ""; }
      else cur += ' ';
      ++i; continue;
    }
    // Gather one word.
    uint32_t j = i;
    while (j < respLen && resp[j] != ' ' && resp[j] != '\n') ++j;
    int wlen = (int)(j - i);
    if (wlen > kCols) {                       // hard-split an over-long word
      for (uint32_t k = i; k < j; ++k) {
        if ((int)cur.length() >= kCols) { push(cur); cur = ""; }
        cur += resp[k];
      }
    } else {
      if ((int)cur.length() + wlen > kCols) { push(cur); cur = ""; }
      for (uint32_t k = i; k < j; ++k) cur += resp[k];
    }
    i = j;
  }
  push(cur);                                   // the in-progress final line
  int caretCol = (int)cur.length();
  int caretRow = ringCount - 1;

  // Draw the lines.
  g->setTextSize(kTxtSize);
  g->setTextColor(t.fg, t.bg);
  for (int r = 0; r < ringCount; ++r) {
    g->setCursor(kBodyX, kBodyTop + r * kLineH);
    g->print(ring[r]);
  }

  // "thinking" hint before the first token lands.
  if (streaming && !sawAnyTok) {
    g->setTextColor(t.accent, t.bg);
    g->setCursor(kBodyX, kBodyTop);
    g->print("...thinking");
    caretCol = 11; caretRow = 0;
  }

  // Blinking block caret (solid while streaming; hidden once finished).
  bool caretOn = streaming ? (((millis() / 400) & 1) != 0) : false;
  if (caretOn && caretRow >= 0 && caretRow < kRows && caretCol <= kCols) {
    int16_t cx = kBodyX + caretCol * kCharW;
    int16_t cy = kBodyTop + caretRow * kLineH;
    g->fillRect(cx, cy, kCharW, kCharH, t.fg);
  }

  // A small done/error marker in the corner.
  if (!streaming && respLen > 0) {
    g->setTextSize(1);
    g->setTextColor(errState ? RED : t.accent, t.bg);
    g->setCursor(SCRN_W - 30, 9);
    g->print(errState ? "err" : "end");
  }

  if (cv) cv->flush();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void LlmTermView::onEvent(const Event &e) {
  if (mode == Mode::Chat) {
    if (e.type == EventType::ButtonShort) {       // back → menu
      mode = Mode::Menu; menuDirty = true; streaming = false;
      if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
      { ModelLock lk; model.revision++; }
      return;
    }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      mode = Mode::Menu; menuDirty = true; streaming = false;
      if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
      { ModelLock lk; model.revision++; }
      return;
    }
    if (e.type == EventType::Tick) {              // safety-net serial pump
      pumpSerial();
      { ModelLock lk; model.revision++; }
    }
    return;
  }

  // Menu mode
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
    for (int i = 0; i < kPromptCount; ++i) {
      int16_t y = kRowTop + i * (kRowH + kRowGap);
      if (e.y >= y && e.y < y + kRowH) {
        hapticBuzz(180, 25);
        mode = Mode::Chat;
        if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
        sendPrompt(kPrompts[i]);
        return;
      }
    }
  }
}
