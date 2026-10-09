// stocks — see stocks.h for the overview.
//
// Serial protocol (watch <- PC), newline-terminated ASCII, one frame per line:
//
//   >>TICK <symbol> <price> <changePct> <nPoints> <p0> <p1> ... <p(n-1)>
//       Upsert one ticker. <symbol> is 1..11 chars, no spaces. <price> and
//       <changePct> are floats. <nPoints> is 0..STK_MAX_SPARK; the sparkline
//       points follow as floats (oldest first). A repeated symbol updates the
//       existing entry in place; a new symbol is appended (up to STK_MAX_TICK).
//   >>CLEAR
//       Drop all tickers (PC sends this when its ticker list changes).
//
// Lines that do not start with ">>" are ignored (boot log / noise).
//
// Serial ownership: the OS ewlog console task (ewcon) also reads Serial. While
// the Stocks screen is up it pauses that console (ewlogConsolePause) and the
// RX task reads; while it is not, the RX task stays off Serial entirely so the
// console (or another app that paused it) gets an unsplit stream. Frames sent
// while the app is closed are therefore dropped; the PC re-sends every poll.
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "stocks.h"
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock
#include "ewlog.h"     // ewlogConsolePause

#include "FreeSansBold24pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSans12pt7b.h"

// ---------------------------------------------------------------------------
// Shared ticker store. Written by the RX task, read by the render task; a
// dedicated mutex (NOT the model mutex) guards it so a slow draw never stalls
// serial parsing. The renderer discovers changes via g_dataRev.
// ---------------------------------------------------------------------------
static const int STK_MAX_TICK  = 16;   // max distinct symbols held
static const int STK_MAX_SPARK = 32;   // max sparkline points per symbol

struct StockTicker {
  char  symbol[12];
  float price;
  float changePct;
  uint8_t n;                     // valid sparkline points
  float spark[STK_MAX_SPARK];
};

static StockTicker      g_tick[STK_MAX_TICK];
static volatile int     g_count   = 0;
static volatile uint32_t g_dataRev = 0;   // bumped on every store mutation
static SemaphoreHandle_t g_mutex  = nullptr;
static TaskHandle_t      g_rxTask = nullptr;
static volatile bool     g_viewActive = false;  // Stocks screen owns Serial

// Wake the render task after a store change.
static void nudgeRenderer() {
  ModelLock lk;
  model.revision++;
}

// Upsert a ticker. Caller must NOT hold g_mutex (we take it here).
static void storeUpsert(const char *sym, float price, float pct,
                        const float *pts, int np) {
  if (np < 0) np = 0;
  if (np > STK_MAX_SPARK) np = STK_MAX_SPARK;
  if (!g_mutex) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  int idx = -1;
  for (int i = 0; i < g_count; i++) {
    if (strncmp(g_tick[i].symbol, sym, sizeof(g_tick[i].symbol)) == 0) { idx = i; break; }
  }
  if (idx < 0 && g_count < STK_MAX_TICK) { idx = g_count; g_count++; }
  if (idx >= 0) {
    StockTicker &t = g_tick[idx];
    strncpy(t.symbol, sym, sizeof(t.symbol) - 1);
    t.symbol[sizeof(t.symbol) - 1] = '\0';
    t.price     = price;
    t.changePct = pct;
    t.n = (uint8_t)np;
    for (int i = 0; i < np; i++) t.spark[i] = pts[i];
  }
  xSemaphoreGive(g_mutex);
  g_dataRev++;
  nudgeRenderer();
}

static void storeClear() {
  if (!g_mutex) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_count = 0;
  xSemaphoreGive(g_mutex);
  g_dataRev++;
  nudgeRenderer();
}

// Copy one ticker out under the lock. Returns false if idx is out of range.
static bool storeGet(int idx, StockTicker &out, int &countOut) {
  if (!g_mutex) { countOut = 0; return false; }
  bool ok = false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  countOut = g_count;
  if (idx >= 0 && idx < g_count) { out = g_tick[idx]; ok = true; }
  xSemaphoreGive(g_mutex);
  return ok;
}

// ---------------------------------------------------------------------------
// Frame parser. Tokenises one ">>" line in place and applies it to the store.
// ---------------------------------------------------------------------------
static void parseLine(char *line) {
  if (line[0] != '>' || line[1] != '>') return;   // not telemetry — ignore
  char *p = line + 2;
  char *cmd = strtok(p, " \t\r\n");
  if (!cmd) return;

  if (strcmp(cmd, "CLEAR") == 0) { storeClear(); return; }

  if (strcmp(cmd, "TICK") == 0) {
    char *sym = strtok(nullptr, " \t\r\n");
    char *sp  = strtok(nullptr, " \t\r\n");
    char *sc  = strtok(nullptr, " \t\r\n");
    char *sn  = strtok(nullptr, " \t\r\n");
    if (!sym || !sp || !sc || !sn) return;
    float price = (float)atof(sp);
    float pct   = (float)atof(sc);
    int   np    = atoi(sn);
    if (np < 0) np = 0;
    if (np > STK_MAX_SPARK) np = STK_MAX_SPARK;
    static float pts[STK_MAX_SPARK];
    int got = 0;
    for (int i = 0; i < np; i++) {
      char *tok = strtok(nullptr, " \t\r\n");
      if (!tok) break;
      pts[got++] = (float)atof(tok);
    }
    storeUpsert(sym, price, pct, pts, got);
    return;
  }
  // Unknown command — ignore.
}

// ---------------------------------------------------------------------------
// RX task. Assembles newline-terminated lines from the CDC serial stream and
// hands each one to parseLine(). Bounded line buffer; overlong lines are
// truncated at the newline so the parser can never run away.
// ---------------------------------------------------------------------------
static void stocksRxTask(void *) {
  static const size_t LINEMAX = 64 + STK_MAX_SPARK * 12;  // sym+nums headroom
  static char buf[LINEMAX];
  size_t len = 0;
  for (;;) {
    // Only read while our screen is up and the console is paused; otherwise
    // the bytes belong to ewcon (or whichever app paused it).
    if (!g_viewActive || !ewlogConsolePaused()) {
      len = 0;   // drop any half line
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c < 0) break;
      if (c == '\n') {
        buf[len] = '\0';
        if (len > 0) parseLine(buf);
        len = 0;
      } else if (c != '\r') {
        if (len < LINEMAX - 1) buf[len++] = (char)c;
        // else: drop until newline (line too long) — keeps the parser safe.
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15));   // ~66 Hz poll; plenty for text frames
  }
}

static void ensureStoreAndTask() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  if (!g_rxTask && g_mutex) {
    // Core 0 alongside taskIO; small stack — parsing is shallow. Not added to
    // the Task WDT (it is an optional service, not a platform task).
    xTaskCreatePinnedToCore(stocksRxTask, "stocksRx", 4096, nullptr, 2,
                            &g_rxTask, 0);
  }
}

// ---------------------------------------------------------------------------
// Drawing helpers. `g` is either the frame canvas (preferred, flicker-free)
// or the panel directly (fallback when PSRAM is exhausted).
// ---------------------------------------------------------------------------
static const int16_t W = 240;
static const int16_t H = 280;

static uint16_t colUp()   { return RGB565(0x24, 0xC0, 0x5A); }  // green
static uint16_t colDown() { return RGB565(0xF0, 0x3A, 0x3A); }  // red
static uint16_t colDim()  { return RGB565(0x70, 0x74, 0x80); }  // grey

// Center a string horizontally with the currently-selected font, draw at
// baseline `y`. Returns the text width.
static int16_t drawCentered(Arduino_GFX *g, const char *s, int16_t y, uint16_t fg) {
  int16_t x1, y1; uint16_t tw, th;
  g->getTextBounds(s, 0, y, &x1, &y1, &tw, &th);
  int16_t x = (W - (int16_t)tw) / 2 - x1;
  g->setTextColor(fg);
  g->setCursor(x, y);
  g->print(s);
  return (int16_t)tw;
}

// Small back chevron, top-left (the firmware also sends ButtonShort on a
// right-swipe / SW2, so this is just a visual affordance).
static void drawBackChevron(Arduino_GFX *g, uint16_t c) {
  g->drawLine(14, 18, 6, 26, c);
  g->drawLine(6, 26, 14, 34, c);
  g->drawLine(15, 18, 7, 26, c);
  g->drawLine(7, 26, 15, 34, c);
}

// Sparkline polyline inside [x,y,w,h]. Autoscales to min..max of the points.
static void drawSparkline(Arduino_GFX *g, int16_t x, int16_t y, int16_t w, int16_t h,
                          const float *pts, int n, uint16_t c) {
  if (n < 2) {
    g->setFont(&FreeSans12pt7b);
    drawCentered(g, "no history", y + h / 2, colDim());
    g->setFont(nullptr);
    return;
  }
  float mn = pts[0], mx = pts[0];
  for (int i = 1; i < n; i++) { if (pts[i] < mn) mn = pts[i]; if (pts[i] > mx) mx = pts[i]; }
  float span = mx - mn;
  if (span < 1e-6f) span = 1.0f;   // flat line — avoid divide-by-zero

  // Faint baseline at the first (oldest) value.
  int16_t by = y + h - 1 - (int16_t)((pts[0] - mn) / span * (h - 2));
  for (int16_t bx = x; bx < x + w; bx += 6) g->drawPixel(bx, by, colDim());

  int16_t px = x, py = y + h - 1 - (int16_t)((pts[0] - mn) / span * (h - 2));
  for (int i = 1; i < n; i++) {
    int16_t cx = x + (int16_t)((int32_t)i * (w - 1) / (n - 1));
    int16_t cy = y + h - 1 - (int16_t)((pts[i] - mn) / span * (h - 2));
    g->drawLine(px, py, cx, cy, c);
    g->drawLine(px, py + 1, cx, cy + 1, c);   // 2px thick for visibility
    px = cx; py = cy;
  }
  // Marker dot at the latest point.
  g->fillCircle(px, py, 2, c);
}

// Page dots at the bottom: one per ticker, filled for the current one.
static void drawPageDots(Arduino_GFX *g, int cur, int count, uint16_t on, uint16_t off) {
  if (count <= 1) return;
  int shown = count > 12 ? 12 : count;   // cap so they fit the width
  int gap = 14;
  int totalW = (shown - 1) * gap;
  int sx = (W - totalW) / 2;
  for (int i = 0; i < shown; i++) {
    uint16_t c = (i == cur) ? on : off;
    g->fillCircle(sx + i * gap, H - 12, (i == cur) ? 3 : 2, c);
  }
}

// Compose the whole screen for ticker `cur` into target `g`.
static void drawScreen(Arduino_GFX *g, int cur, const StockTicker *t, int count) {
  ThemeColors th = theme();
  g->fillScreen(th.bg);
  drawBackChevron(g, th.fg);

  if (count == 0 || !t) {
    g->setFont(&FreeSansBold12pt7b);
    drawCentered(g, "STOCKS", 70, th.accent == th.bg ? th.fg : th.fg);
    g->setFont(&FreeSans12pt7b);
    drawCentered(g, "Waiting for PC...", 150, colDim());
    g->setFont(nullptr);
    g->setTextColor(colDim());
    g->setTextSize(1);
    drawCentered(g, "run pc/stocks_feed.py", 180, colDim());
    g->setFont(nullptr);
    return;
  }

  bool up = t->changePct >= 0.0f;
  uint16_t chg = up ? colUp() : colDown();

  // Symbol (hero).
  g->setFont(&FreeSansBold24pt7b);
  drawCentered(g, t->symbol, 62, th.fg);

  // Price.
  char pbuf[24];
  snprintf(pbuf, sizeof(pbuf), "%.2f", t->price);
  g->setFont(&FreeSansBold24pt7b);
  drawCentered(g, pbuf, 112, th.fg);

  // Change %  (triangle + signed percent), centered as a group.
  char cbuf[24];
  snprintf(cbuf, sizeof(cbuf), "%+.2f%%", t->changePct);
  g->setFont(&FreeSansBold12pt7b);
  int16_t x1, y1; uint16_t tw, thh;
  g->getTextBounds(cbuf, 0, 145, &x1, &y1, &tw, &thh);
  int16_t triW = 16;
  int16_t groupW = triW + (int16_t)tw;
  int16_t gx = (W - groupW) / 2;
  int16_t ty = 134;   // triangle top region
  if (up) {
    g->fillTriangle(gx, ty + 14, gx + 12, ty + 14, gx + 6, ty + 2, chg);
  } else {
    g->fillTriangle(gx, ty + 2, gx + 12, ty + 2, gx + 6, ty + 14, chg);
  }
  g->setTextColor(chg);
  g->setCursor(gx + triW - x1, 149);
  g->print(cbuf);
  g->setFont(nullptr);

  // Sparkline.
  drawSparkline(g, 16, 168, W - 32, 80, t->spark, t->n, chg);

  // Footer: index / count.
  g->setFont(nullptr);
  g->setTextSize(1);
  char fbuf[16];
  snprintf(fbuf, sizeof(fbuf), "%d / %d", cur + 1, count);
  g->setTextColor(colDim());
  drawCentered(g, fbuf, H - 28, colDim());

  drawPageDots(g, cur, count, th.accent == th.bg ? th.fg : th.fg, colDim());
}

// ---------------------------------------------------------------------------
// View lifecycle.
// ---------------------------------------------------------------------------
void StocksView::onEnter() {
  ensureStoreAndTask();
  ewlogConsolePause(true);   // the RX task owns USB serial while we are up
  g_viewActive = true;
  firstFrame  = true;
  lastDataRev = 0xFFFFFFFF;
  lastCur     = -1;
  lastCount   = -1;
  // Clamp index into range if tickers arrived/left while we were away.
  int c; StockTicker tmp;
  storeGet(0, tmp, c);
  if (c > 0 && cur >= c) cur = c - 1;
  if (cur < 0) cur = 0;
  if (gfx) { ThemeColors th = theme(); gfx->fillScreen(th.bg); }
}

void StocksView::onExit() {
  // Leave the RX task running (it idles until the next visit); the frame
  // canvas is shared and reclaimed by whoever enters next. Hand Serial back to
  // the ewlog console.
  g_viewActive = false;
  ewlogConsolePause(false);
}

void StocksView::render() {
  if (!gfx) return;

  int count; StockTicker t;
  bool have = storeGet(cur, t, count);
  if (count > 0 && cur >= count) { cur = count - 1; have = storeGet(cur, t, count); }

  uint32_t rev = g_dataRev;
  bool changed = firstFrame || rev != lastDataRev || cur != lastCur || count != lastCount;
  if (!changed) return;
  firstFrame  = false;
  lastDataRev = rev;
  lastCur     = cur;
  lastCount   = count;

  Arduino_Canvas *cv = frameCanvas();
  if (cv) {
    drawScreen(cv, cur, have ? &t : nullptr, count);
    cv->flush();
  } else {
    // PSRAM exhausted: draw straight to the panel (may flicker slightly).
    drawScreen(gfx, cur, have ? &t : nullptr, count);
  }
}

void StocksView::onEvent(const Event &e) {
  // Back: SW2 and right-swipe both arrive as ButtonShort.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
    int count; StockTicker tmp; storeGet(0, tmp, count);
    if (count > 1) {
      if (e.x < W / 2) { cur = (cur - 1 + count) % count; }
      else             { cur = (cur + 1) % count; }
      hapticBuzz(60, 25);
      ModelLock lk; model.revision++;
    }
    return;
  }

  if (e.type == EventType::Gesture) {
    int count; StockTicker tmp; storeGet(0, tmp, count);
    if (count <= 1) return;
    bool moved = false;
    switch (e.gesture) {
      case Gesture::SwipeLeft:              // next
      case Gesture::SwipeUp:
        cur = (cur + 1) % count; moved = true; break;
      case Gesture::SwipeDown:              // previous
        cur = (cur - 1 + count) % count; moved = true; break;
      default: break;
    }
    if (moved) {
      hapticBuzz(60, 25);
      ModelLock lk; model.revision++;
    }
    return;
  }
}
