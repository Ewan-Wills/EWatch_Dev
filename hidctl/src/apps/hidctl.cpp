// hidctl — Notes editor + mouse-cursor overlay, driven through a transport-
// agnostic input seam. See hidctl.h for the feasibility story and the seam.
//
// What runs here:
//   * A Notes text buffer (persisted to LittleFS at /notes.txt) with a caret,
//     word-wrapped and drawn in a mono font.
//   * A mouse cursor overlay driven by relative mouse deltas; a left click at
//     the cursor is routed through the SAME handleTap() path as a finger tap,
//     so the mouse can press the on-screen [Clear] / [Save] buttons and Back.
//   * A serial-feeder transport: a private FreeRTOS task reads newline-framed
//     ASCII commands from USB-CDC and calls the hidFeed*() seam. A future
//     BLE-HID central would call the same functions and this file is unchanged.
//
// Serial protocol (PC -> watch), one command per line, '\n'-terminated ASCII.
// Any unrecognised line is ignored, so boot logs / stray text are harmless.
//   KC <n>        key char: n = decimal ASCII 32..126           (e.g. "KC 65")
//   KS <NAME>     special: ENTER BACK TAB ESC LEFT RIGHT UP DOWN HOME END
//   MM <dx> <dy>  relative mouse move, signed decimals           (e.g. "MM 4 -3")
//   MB <0|1>      left button: 1 = press (click), 0 = release
// The watch prints ">> HIDCTL ready" once on entry as a PC-side handshake cue.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "hidctl.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "ewlog.h"

#include "FreeMono12pt7b.h"

// ---------------------------------------------------------------------------
// Layout constants (screen is 240 wide x 280 tall)
// ---------------------------------------------------------------------------
static const int16_t SCR_W = 240, SCR_H = 280;
static const int16_t TEXT_X = 6,  TEXT_Y = 54;          // top-left of text area
static const int16_t TEXT_W = SCR_W - 12;               // 228 px usable width
static const int16_t LINE_H = 20;                       // mono 12pt line pitch
static const int16_t TEXT_BOTTOM = 232;                 // above the button bar
static const int16_t GLYPH_W = 12;                      // FreeMono12pt advance
static const int   COLS = TEXT_W / GLYPH_W;             // ~19 chars per line
static const int   VIS_ROWS = (TEXT_BOTTOM - TEXT_Y) / LINE_H;

// Button bar
static const int16_t BTN_Y = 240, BTN_H = 34;
static const int16_t CLR_X = 20,  CLR_W = 90;
static const int16_t SAVE_X = 130, SAVE_W = 90;

// ---------------------------------------------------------------------------
// Notes buffer
// ---------------------------------------------------------------------------
static const size_t NOTE_CAP = 1024;        // chars (+1 for NUL)
static char     gText[NOTE_CAP + 1] = {0};
static size_t   gLen   = 0;                  // current length
static size_t   gCaret = 0;                  // caret index into gText
static bool     gDirty = false;              // unsaved changes
static const char *kNotePath = "/notes.txt";

// ---------------------------------------------------------------------------
// Mouse cursor overlay
// ---------------------------------------------------------------------------
static int16_t gCurX = SCR_W / 2, gCurY = SCR_H / 2;
static bool    gBtnDown = false;

// ---------------------------------------------------------------------------
// Input seam plumbing: a small ring of decoded input items filled by a
// transport task and drained on the render task in render().
// ---------------------------------------------------------------------------
namespace {
enum class Kind : uint8_t { KeyChar, KeySpecial, MouseMove, MouseBtn };
struct InItem {
  Kind    kind;
  char    ch;         // KeyChar
  uint8_t sk;         // KeySpecial (hid::SpecialKey)
  int16_t dx, dy;     // MouseMove
  bool    down;       // MouseBtn
};
}  // namespace

static QueueHandle_t gInQ   = nullptr;       // ring of InItem
static TaskHandle_t  gFeedTask = nullptr;
static volatile bool gActive = false;        // true while the Notes screen is open

// Nudge the render task to repaint (render() is called when model.revision
// changes). Cheap; coalesced by the render loop.
static void nudgeRender() {
  ModelLock lk;
  model.revision++;
}

static void pushItem(const InItem &it) {
  if (!gActive || !gInQ) return;
  // Non-blocking send; if the ring is full we drop the event (fine for a
  // demo — a flood of mouse moves just loses a few samples).
  if (xQueueSend(gInQ, &it, 0) == pdPASS) nudgeRender();
}

// ---- public seam (declared in hidctl.h) ----------------------------------
void hid::hidFeedKeyChar(char c) {
  if (c < 32 || c > 126) return;
  InItem it{}; it.kind = Kind::KeyChar; it.ch = c; pushItem(it);
}
void hid::hidFeedKeySpecial(hid::SpecialKey k) {
  InItem it{}; it.kind = Kind::KeySpecial; it.sk = (uint8_t)k; pushItem(it);
}
void hid::hidFeedMouseMove(int dx, int dy) {
  InItem it{}; it.kind = Kind::MouseMove;
  it.dx = (int16_t)dx; it.dy = (int16_t)dy; pushItem(it);
}
void hid::hidFeedMouseButton(bool down) {
  InItem it{}; it.kind = Kind::MouseBtn; it.down = down; pushItem(it);
}

// ---------------------------------------------------------------------------
// Serial-feeder transport task. Reads '\n'-framed ASCII and calls the seam.
// ---------------------------------------------------------------------------
static bool parseSpecial(const char *name, hid::SpecialKey &out) {
  struct { const char *n; hid::SpecialKey k; } map[] = {
    {"ENTER", hid::SpecialKey::Enter}, {"BACK", hid::SpecialKey::Backspace},
    {"TAB",   hid::SpecialKey::Tab},   {"ESC",  hid::SpecialKey::Esc},
    {"LEFT",  hid::SpecialKey::Left},  {"RIGHT",hid::SpecialKey::Right},
    {"UP",    hid::SpecialKey::Up},    {"DOWN", hid::SpecialKey::Down},
    {"HOME",  hid::SpecialKey::Home},  {"END",  hid::SpecialKey::End},
  };
  for (auto &m : map) if (strcmp(name, m.n) == 0) { out = m.k; return true; }
  return false;
}

static void parseLine(char *line) {
  // Tokenise in place.
  if (line[0] == '\0') return;
  if (line[0]=='K' && line[1]=='C') {                 // KC <n>
    int n = atoi(line + 2);
    hid::hidFeedKeyChar((char)n);
  } else if (line[0]=='K' && line[1]=='S') {          // KS <NAME>
    const char *p = line + 2;
    while (*p==' ') p++;
    hid::SpecialKey k;
    if (parseSpecial(p, k)) hid::hidFeedKeySpecial(k);
  } else if (line[0]=='M' && line[1]=='M') {          // MM <dx> <dy>
    int dx = 0, dy = 0;
    if (sscanf(line + 2, "%d %d", &dx, &dy) == 2) hid::hidFeedMouseMove(dx, dy);
  } else if (line[0]=='M' && line[1]=='B') {          // MB <0|1>
    int b = atoi(line + 2);
    hid::hidFeedMouseButton(b != 0);
  }
  // else: unknown line -> ignore (boot/log noise is harmless)
}

static void feederTask(void *) {
  static char buf[64];
  size_t n = 0;
  // Only reads while the Notes screen is open, which is exactly while the
  // ewlog console is paused (onEnter/onExit), so the two never split a line.
  while (gActive) {
    while (gActive && ewlogConsolePaused() && Serial.available() > 0) {
      int c = Serial.read();
      if (c == '\n' || c == '\r') {
        buf[n] = '\0';
        if (n > 0) parseLine(buf);
        n = 0;
      } else if (n < sizeof(buf) - 1) {
        buf[n++] = (char)c;
      } else {
        n = 0;   // overflow: resync on next newline
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  gFeedTask = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// LittleFS persistence
// ---------------------------------------------------------------------------
static bool gFsOk = false;

static void loadNote() {
  gLen = 0; gText[0] = '\0';
  if (!gFsOk) return;
  File f = LittleFS.open(kNotePath, "r");
  if (!f) return;
  size_t n = f.read((uint8_t *)gText, NOTE_CAP);
  if (n > NOTE_CAP) n = NOTE_CAP;
  gText[n] = '\0';
  gLen = n;
  f.close();
}

static bool saveNote() {
  if (!gFsOk) return false;
  File f = LittleFS.open(kNotePath, "w");
  if (!f) return false;
  f.write((const uint8_t *)gText, gLen);
  f.close();
  gDirty = false;
  return true;
}

// ---------------------------------------------------------------------------
// Text editing (the onKey half of the seam)
// ---------------------------------------------------------------------------
static void insertChar(char c) {
  if (gLen >= NOTE_CAP) return;
  // shift tail right
  for (size_t i = gLen; i > gCaret; i--) gText[i] = gText[i - 1];
  gText[gCaret] = c;
  gLen++; gCaret++;
  gText[gLen] = '\0';
  gDirty = true;
}

static void backspace() {
  if (gCaret == 0) return;
  for (size_t i = gCaret - 1; i < gLen - 1; i++) gText[i] = gText[i + 1];
  gLen--; gCaret--;
  gText[gLen] = '\0';
  gDirty = true;
}

static void onKeyChar(char c)                { insertChar(c); }
static void onKeySpecial(hid::SpecialKey k) {
  switch (k) {
    case hid::SpecialKey::Enter:     insertChar('\n'); break;
    case hid::SpecialKey::Tab:       insertChar(' '); insertChar(' '); break;
    case hid::SpecialKey::Backspace: backspace(); break;
    case hid::SpecialKey::Left:      if (gCaret > 0)    gCaret--; break;
    case hid::SpecialKey::Right:     if (gCaret < gLen) gCaret++; break;
    case hid::SpecialKey::Home:      gCaret = 0;    break;
    case hid::SpecialKey::End:       gCaret = gLen; break;
    case hid::SpecialKey::Up:        if (gCaret >= (size_t)COLS) gCaret -= COLS; else gCaret = 0; break;
    case hid::SpecialKey::Down:      gCaret = (gCaret + COLS <= gLen) ? gCaret + COLS : gLen; break;
    case hid::SpecialKey::Esc:       break;
  }
}

// ---------------------------------------------------------------------------
// Word-wrap model: break gText into visual rows. A row break happens at a
// newline OR when COLS chars are reached. We also record, per visual row, the
// text index where it starts, so the caret can be located on screen.
// ---------------------------------------------------------------------------
struct WrapRow { size_t start; uint8_t len; bool hardBreak; };
static const int MAX_ROWS = 64;
static WrapRow gRows[MAX_ROWS];
static int     gRowCount = 0;

static void wrap() {
  gRowCount = 0;
  size_t i = 0;
  if (gLen == 0) { gRows[0] = { 0, 0, true }; gRowCount = 1; return; }
  while (i <= gLen && gRowCount < MAX_ROWS) {
    size_t start = i;
    uint8_t len = 0;
    bool hard = false;
    while (i < gLen && len < COLS) {
      char c = gText[i];
      if (c == '\n') { hard = true; i++; break; }
      len++; i++;
    }
    gRows[gRowCount++] = { start, len, hard };
    if (i == gLen) {
      // trailing newline -> show an empty final row so the caret is visible
      if (gLen > 0 && gText[gLen - 1] == '\n' && gRowCount < MAX_ROWS)
        gRows[gRowCount++] = { gLen, 0, true };
      break;
    }
  }
}

// Find caret's (row,col). Returns row index; col via out param.
static int caretRowCol(int &col) {
  for (int r = 0; r < gRowCount; r++) {
    size_t s = gRows[r].start;
    size_t e = s + gRows[r].len;                 // chars shown on this row
    // caret belongs to row r if it falls within [s, e], but a hard break
    // consumes the '\n' so the end-of-row caret lands on the next row.
    size_t eInclusive = e + (gRows[r].hardBreak ? 1 : 0);
    if (gCaret < eInclusive || r == gRowCount - 1) {
      col = (int)(gCaret - s);
      if (col < 0) col = 0;
      if (col > gRows[r].len) col = gRows[r].len;
      return r;
    }
  }
  col = 0; return 0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static bool     gCaretOn = true;
static uint32_t gLastBlink = 0;

static void drawInto(Arduino_GFX *g) {
  ThemeColors t = theme();
  g->fillScreen(t.bg);

  // Title bar (drawn manually so it works on the canvas too)
  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(t.fg, t.bg);
  g->setCursor(44, 10);
  g->print("Notes");
  g->drawFastHLine(20, 40, 200, t.line);

  // Back chevron (top-left)
  uint16_t backTxt = contrastFor(t.accent);
  g->fillRoundRect(2, 2, 36, 30, 5, t.accent);
  g->setTextColor(backTxt, t.accent);
  g->setTextSize(3);
  g->setCursor(10, 6);
  g->print('<');

  // Char count (top-right)
  g->setFont(nullptr);
  g->setTextSize(1);
  g->setTextColor(t.line, t.bg);
  char cnt[20];
  snprintf(cnt, sizeof(cnt), "%u/%u%s", (unsigned)gLen, (unsigned)NOTE_CAP,
           gDirty ? "*" : "");
  int cntw = (int)strlen(cnt) * 6;
  g->setCursor(SCR_W - cntw - 4, 14);
  g->print(cnt);

  // Text body (mono font), scrolled so the caret row is visible
  wrap();
  int caretCol = 0;
  int caretRow = caretRowCol(caretCol);
  int topRow = 0;
  if (caretRow >= VIS_ROWS) topRow = caretRow - VIS_ROWS + 1;

  g->setFont(&FreeMono12pt7b);
  g->setTextColor(t.fg, t.bg);
  for (int vr = 0; vr < VIS_ROWS; vr++) {
    int r = topRow + vr;
    if (r >= gRowCount) break;
    int16_t y = TEXT_Y + vr * LINE_H + (LINE_H - 4);   // baseline
    int16_t x = TEXT_X;
    for (int c = 0; c < gRows[r].len; c++) {
      char ch = gText[gRows[r].start + c];
      if (ch == '\n') continue;
      g->setCursor(x, y);
      g->print(ch);
      x += GLYPH_W;
    }
  }

  // Caret (blink) — only if its row is visible
  if (gCaretOn && caretRow >= topRow && caretRow < topRow + VIS_ROWS) {
    int vr = caretRow - topRow;
    int16_t cx = TEXT_X + caretCol * GLYPH_W;
    int16_t cy = TEXT_Y + vr * LINE_H + 2;
    g->drawFastVLine(cx, cy, LINE_H - 4, t.accent);
  }

  // Button bar
  g->setFont(nullptr);
  g->setTextSize(2);
  uint16_t bt = contrastFor(t.accent);
  g->fillRoundRect(CLR_X, BTN_Y, CLR_W, BTN_H, 6, t.accent);
  g->setTextColor(bt, t.accent);
  g->setCursor(CLR_X + 14, BTN_Y + 9);
  g->print("Clear");
  g->fillRoundRect(SAVE_X, BTN_Y, SAVE_W, BTN_H, 6, t.accent);
  g->setTextColor(bt, t.accent);
  g->setCursor(SAVE_X + 20, BTN_Y + 9);
  g->print("Save");

  // Mouse cursor overlay (small arrow): a filled triangle + outline.
  int16_t mx = gCurX, my = gCurY;
  uint16_t cur = gBtnDown ? t.accent : t.fg;
  uint16_t edge = contrastFor(cur);
  g->fillTriangle(mx, my, mx, my + 14, mx + 10, my + 10, cur);
  g->drawTriangle(mx, my, mx, my + 14, mx + 10, my + 10, edge);
}

static void repaint() {
  if (!gfx) return;
  Arduino_Canvas *cv = frameCanvas();
  if (cv) { drawInto(cv); cv->flush(); }
  else    { drawInto(gfx); }    // PSRAM exhausted: draw direct (may flicker)
}

// Button hit-test (the file-scope inRect in view.cpp is private to that TU).
static bool inBtn(uint16_t x, uint16_t y, int16_t bx, int16_t bw) {
  return (int16_t)x >= bx && (int16_t)x < bx + bw &&
         (int16_t)y >= BTN_Y && (int16_t)y < BTN_Y + BTN_H;
}

// ---------------------------------------------------------------------------
// View lifecycle
// ---------------------------------------------------------------------------
void HidCtlView::onEnter() {
  // Mount LittleFS (format on first use) and load the saved note.
  gFsOk = LittleFS.begin(/*formatOnFail=*/true);
  loadNote();
  gCaret = gLen;
  gCurX = SCR_W / 2; gCurY = SCR_H / 2; gBtnDown = false;
  gCaretOn = true; gLastBlink = millis();

  if (!gInQ) gInQ = xQueueCreate(64, sizeof(InItem));
  if (gInQ) xQueueReset(gInQ);
  // The ewlog debug console (ewcon) also reads Serial; pause it while this
  // screen owns the port, or it steals bytes from the feeder stream.
  ewlogConsolePause(true);
  gActive = true;
  if (!gFeedTask)
    xTaskCreatePinnedToCore(feederTask, "hidfeed", 3072, nullptr, 2, &gFeedTask, 0);

  Serial.println(">> HIDCTL ready");   // handshake cue for the PC feeder
  repaint();
}

void HidCtlView::onExit() {
  if (gDirty) saveNote();
  gActive = false;                      // feederTask observes this and exits
  // Give the feeder task a moment to notice and self-delete.
  for (int i = 0; i < 20 && gFeedTask; i++) vTaskDelay(pdMS_TO_TICKS(5));
  ewlogConsolePause(false);             // hand Serial back to the console
}

// Set by edits that must force a repaint on the next render() pass (taps,
// Clear/Save). Reset once consumed.
static bool gForceRepaint = true;

void HidCtlView::handleTap(uint16_t x, uint16_t y) {
  // Back chevron
  if (x < 48 && y < 40) { switchTo(Screen::AppList); return; }
  // Clear
  if (inBtn(x, y, CLR_X, CLR_W)) {
    gLen = 0; gCaret = 0; gText[0] = '\0'; gDirty = true;
    hapticBuzz(150, 25); gForceRepaint = true; return;
  }
  // Save
  if (inBtn(x, y, SAVE_X, SAVE_W)) {
    bool ok = saveNote();
    hapticBuzz(ok ? 180 : 80, ok ? 40 : 120); gForceRepaint = true; return;
  }
}

// Drain the transport seam's input ring and apply each item. Returns true if
// anything changed the visible state. May call switchTo() via a mouse click on
// Back — callers must not touch member state afterwards if so.
bool HidCtlView::drainInput() {
  bool changed = false;
  if (!gInQ) return false;
  InItem it;
  while (xQueueReceive(gInQ, &it, 0) == pdPASS) {
    changed = true;
    switch (it.kind) {
      case Kind::KeyChar:    onKeyChar(it.ch); break;
      case Kind::KeySpecial: onKeySpecial((hid::SpecialKey)it.sk); break;
      case Kind::MouseMove:
        gCurX += it.dx; gCurY += it.dy;
        if (gCurX < 0) gCurX = 0; if (gCurX > SCR_W - 2) gCurX = SCR_W - 2;
        if (gCurY < 0) gCurY = 0; if (gCurY > SCR_H - 2) gCurY = SCR_H - 2;
        break;
      case Kind::MouseBtn:
        if (it.down && !gBtnDown) {            // press edge = click at cursor
          gBtnDown = it.down;
          handleTap((uint16_t)gCurX, (uint16_t)gCurY);  // may switchTo()
          if (!gActive) return true;           // we left the screen; bail out
          continue;
        }
        gBtnDown = it.down;
        break;
    }
  }
  return changed;
}

// render() runs on the render task whenever model.revision changes (the I/O
// task bumps it continuously) and at least once per second. Serial input only
// bumps revision — it posts no hardware Event — so this is where typed keys and
// mouse motion are actually applied. We redraw only when something changed or
// the caret blink toggled, to avoid pinning the CPU on a full-canvas flush.
void HidCtlView::render() {
  if (!gfx) return;
  bool changed = drainInput();
  if (!gActive) return;                        // a mouse-click on Back left us

  uint32_t now = millis();
  if (now - gLastBlink >= 500) { gCaretOn = !gCaretOn; gLastBlink = now; changed = true; }

  if (changed || gForceRepaint) { repaint(); gForceRepaint = false; }
}

// While the Notes screen is open, run the render task at ~30 fps so serial
// input is drained promptly and the caret blinks smoothly. OS2.1 no longer
// re-renders on every model.revision bump, so this is what keeps typing live.
uint16_t HidCtlView::desiredFrameMs() const {
  return gActive ? 33 : 0;
}

void HidCtlView::onEvent(const Event &e) {
  // Hardware back (SW2 short / right-swipe)
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  // Finger taps still work, in parallel with the mouse cursor.
  if (e.type == EventType::Touch) { handleTap(e.x, e.y); }
  // Key/mouse input is drained in render(); nothing else to do here.
}
