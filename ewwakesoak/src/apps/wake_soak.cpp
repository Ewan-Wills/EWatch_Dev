// Wake soak test — see wake_soak.h.
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

#include "wake_soak.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "wake_gest.h"

static const int16_t W = 240, H = 280;
static const char   *kPath = "/wakesoak.csv";
static const uint32_t kPromptMs = 5000, kHoldMs = 1000, kGraceMs = 400;

static bool     sEnabled = false;
static bool     sLabel = false;            // label mode (see wake_soak.h)
static bool     sPending = false;          // a prompt is waiting for an answer
static SoakWake sRec;
static char     sSrc[24];
static uint32_t sWakeMs = 0;               // millis() at the wake
static uint32_t sCount = 0;                // records in the file (n column)
static bool     sCounted = false;          // sCount loaded from the file this boot
static volatile bool sTestGoto = false;    // `soak test` from the console task

// Prompt geometry.
static const int16_t YES_X = 20, YES_Y = 110, YES_W = 200, YES_H = 80;
static const int16_t NO_X = 60, NO_Y = 206, NO_W = 120, NO_H = 46;
// Stats geometry.
static const int16_t TOG_X = 20, TOG_Y = 52, TOG_W = 200, TOG_H = 40;

void soakBegin() {
  Preferences p;
  if (p.begin("soak", true)) { sEnabled = p.getBool("on", false); sLabel = p.getBool("label", false); p.end(); }
  EWLOGI("SOAK", "begin on=%d label=%d", (int)sEnabled, (int)sLabel);
}
bool soakEnabled()      { return sEnabled; }
bool soakLabelMode()    { return sEnabled && sLabel; }
bool soakPromptActive() { return sPending; }
// Console-injected prompt: the switch must happen on the render task.
bool soakTakeGoto() { bool g = sTestGoto; sTestGoto = false; return g; }

static void setEnabled(bool on) {
  sEnabled = on;
  Preferences p;
  if (p.begin("soak", false)) { p.putBool("on", on); p.end(); }
  EWLOGI("SOAK", "enabled=%d", (int)on);
}

static void appendRecord(const SoakWake &w, const char *answer, uint32_t respMs) {
  if (!sCounted) {                          // carry n on across reboots
    sCounted = true; sCount = 0;
    File r = LittleFS.open(kPath, FILE_READ);
    if (r) { while (r.available()) if (r.read() == '\n') sCount++; r.close(); }
    if (sCount) sCount--;                   // header line
  }
  char rtc[12];
  { ModelLock lk; snprintf(rtc, sizeof rtc, "%02u:%02u:%02u", model.hour, model.minute, model.second); }
  char line[160];
  snprintf(line, sizeof line, "%lu,%lu,%s,%s,%lu,%u,%u,%u,%u,%u,%d,%d,%d,%s,%lu,%lu",
           (unsigned long)++sCount, (unsigned long)(millis() / 1000), rtc, w.src,
           (unsigned long)w.offMs, w.gesture, w.pts, w.x, w.y, w.heldMs,
           w.ax, w.ay, w.az, answer, (unsigned long)respMs, (unsigned long)w.traceId);
  gestAnswer(w.traceId, answer);
  File f = LittleFS.open(kPath, FILE_APPEND);
  if (f) {
    if (f.size() == 0) f.println("n,uptime_s,rtc,src,off_ms,gesture,pts,x,y,held_ms,ax,ay,az,answer,resp_ms,trace_id");
    f.println(line);
    f.close();
  } else {
    EWLOGW("SOAK", "csv open failed");
  }
  EWLOGI("SOAK", "%s", line);
}

void soakOnWake(const SoakWake &w, bool askUser) {
  if (!sEnabled) return;
  if (!askUser) { appendRecord(w, "auto_yes", 0); return; }
  if (sPending) appendRecord(sRec, "superseded", millis() - sWakeMs);
  sRec = w;
  strlcpy(sSrc, w.src, sizeof sSrc);
  sRec.src = sSrc;
  sWakeMs = millis();
  sPending = true;
  switchTo(Screen::WakeSoak);
}

void soakLogRejected(const SoakWake &w) {
  if (sEnabled) appendRecord(w, "rejected", 0);
}

// ---------------------------------------------------------------------------
// Stats (parsed from the CSV; small file)
// ---------------------------------------------------------------------------
struct SrcStat { char src[22]; uint16_t total, yes, no, timeout, rejected, autoYes; };
static int readStats(SrcStat *st, int max) {
  int n = 0;
  File f = LittleFS.open(kPath, FILE_READ);
  if (!f) return 0;
  char line[170];
  while (f.available()) {
    size_t len = f.readBytesUntil('\n', line, sizeof line - 1);
    line[len] = 0;
    if (!len || line[0] == 'n') continue;
    // src = 4th field, answer = 14th
    char *fld[16]; int nf = 0; char *p = line;
    while (nf < 16 && p) { fld[nf++] = p; p = strchr(p, ','); if (p) *p++ = 0; }
    if (nf < 15) continue;
    const char *src = fld[3], *ans = fld[13];
    int i = 0;
    while (i < n && strcmp(st[i].src, src)) i++;
    if (i == n) { if (n >= max) continue; memset(&st[n], 0, sizeof(SrcStat)); strlcpy(st[n].src, src, sizeof st[n].src); n++; }
    st[i].total++;
    if (!strcmp(ans, "yes")) st[i].yes++;
    else if (!strcmp(ans, "no")) st[i].no++;
    else if (!strcmp(ans, "timeout")) st[i].timeout++;
    else if (!strcmp(ans, "rejected")) st[i].rejected++;
    else if (!strcmp(ans, "auto_yes")) st[i].autoYes++;
  }
  f.close();
  return n;
}

bool soakHandleLine(char *line) {
  if (strncmp(line, "soak", 4) != 0 || (line[4] && line[4] != ' ')) return false;
  const char *a = line[4] ? line + 5 : "";
  if (!strcmp(a, "on"))  { setEnabled(true);  Serial.println("soak on"); return true; }
  if (!strcmp(a, "off")) { setEnabled(false); Serial.println("soak off"); return true; }
  if (!strcmp(a, "test")) {                    // inject a fake touch wake (prompt UI check)
    if (!sEnabled) { Serial.println("soak test: soak is off"); return true; }
    SoakWake r = { "test_touch", 1234, 5, 1, 120, 140, 400, 0, 0, 0 };
    sRec = r; strlcpy(sSrc, r.src, sizeof sSrc); sRec.src = sSrc;
    sWakeMs = millis(); sPending = true;
    sTestGoto = true;                            // render task switches (see soakTakeGoto)
    Serial.println("soak test: prompt");
    return true;
  }
  if (!strcmp(a, "clear")) { LittleFS.remove(kPath); sCount = 0; gestClear(); Serial.println("soak cleared"); return true; }
  if (!strcmp(a, "label on") || !strcmp(a, "label off")) {
    sLabel = a[7] == 'n';
    Preferences p;
    if (p.begin("soak", false)) { p.putBool("label", sLabel); p.end(); }
    Serial.printf("soak label=%d\n", (int)sLabel);
    return true;
  }
  if (gestHandleLine(a)) return true;
  // Machine pull (tools/soak_pull.py): `soak get FROM COUNT` prints
  //   SK <idx> <fnv1a32 hex> <line>   for file lines FROM..FROM+COUNT-1
  //   SK end <total lines>
  // The USB serial can drop bytes, so the host checks each line's hash and
  // re-asks for the ones that arrived damaged.
  if (!strncmp(a, "get", 3)) {
    unsigned long from = 0, cnt = 1000000;
    sscanf(a + 3, "%lu %lu", &from, &cnt);
    File f = LittleFS.open(kPath, FILE_READ);
    uint32_t idx = 0;
    char ln[192], out[220];
    Serial.setTxTimeoutMs(1000);
    while (f && f.available()) {
      size_t n = f.readBytesUntil('\n', ln, sizeof ln - 1);
      ln[n] = 0;
      if (n && ln[n - 1] == '\r') ln[--n] = 0;
      if (!n) continue;
      if (idx >= from && idx < from + cnt) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n; i++) { h ^= (uint8_t)ln[i]; h *= 16777619u; }
        int m = snprintf(out, sizeof out, "SK %lu %08lx %s\n", (unsigned long)idx, (unsigned long)h, ln);
        Serial.write((const uint8_t *)out, m);
        if ((idx & 7) == 7) vTaskDelay(pdMS_TO_TICKS(5));
      }
      idx++;
    }
    if (f) f.close();
    Serial.printf("SK end %lu\n", (unsigned long)idx);
    Serial.flush();
    Serial.setTxTimeoutMs(100);
    return true;
  }
  if (!strcmp(a, "dump")) {
    File f = LittleFS.open(kPath, FILE_READ);
    if (!f) { Serial.println("soak: no data"); return true; }
    // Line by line, one write each, with a 1 s TX timeout: HWCDC drops the
    // rest of a write after 100 ms of full ring (see ewsim idfQuiet), which
    // cut and glued rows together in the dump.
    Serial.setTxTimeoutMs(1000);
    uint32_t rows = 0;
    char ln[192];
    while (f.available()) {
      size_t n = f.readBytesUntil('\n', ln, sizeof ln - 2);
      ln[n] = 0;
      if (n && ln[n - 1] == '\r') ln[--n] = 0;
      if (!n) continue;
      ln[n++] = '\n';
      Serial.write((const uint8_t *)ln, n);
      rows++;
      if ((rows & 7) == 0) vTaskDelay(pdMS_TO_TICKS(5));
    }
    f.close();
    Serial.printf("soak: end lines=%lu\n", (unsigned long)rows);
    Serial.flush();
    Serial.setTxTimeoutMs(100);
    return true;
  }
  // stats (default)
  SrcStat st[10];
  int n = readStats(st, 10);
  char gs[96]; gestStats(gs, sizeof gs);
  Serial.printf("soak on=%d label=%d sources=%d %s\n", (int)sEnabled, (int)sLabel, n, gs);
  for (int i = 0; i < n; i++)
    Serial.printf("  %-22s total=%u yes=%u no=%u timeout=%u rejected=%u auto=%u\n", st[i].src,
                  st[i].total, st[i].yes, st[i].no, st[i].timeout, st[i].rejected, st[i].autoYes);
  return true;
}

// ---------------------------------------------------------------------------
// View: prompt while an answer is pending, otherwise stats + on/off
// ---------------------------------------------------------------------------
void WakeSoakView::onEnter() {
  firstDraw = true; enterMs = millis(); holding = false; shownProg = -1; shownSecs = -1;
}

uint16_t WakeSoakView::desiredFrameMs() const { return sPending ? 50 : 0; }

void WakeSoakView::finish(const char *answer) {
  appendRecord(sRec, answer, millis() - sWakeMs);
  sPending = false;
  hapticBuzz(strcmp(answer, "yes") == 0 ? 120 : 60, strcmp(answer, "yes") == 0 ? 80 : 40);
  switchTo(Screen::Watch);
  // No answer = assume it was accidental: straight back to sleep.
  if (strcmp(answer, "yes") != 0) powerRequestSleep();
}

void WakeSoakView::drawPrompt() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2); gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(30, 16); gfx->print("Wake check");
  gfx->setTextSize(1); gfx->setTextColor(LIGHTGREY, BLACK);
  char b[48];
  snprintf(b, sizeof b, "woke by: %s", sRec.src);
  gfx->setCursor(20, 50); gfx->print(b);
  gfx->setCursor(20, 66); gfx->print("Was that on purpose?");
  gfx->fillRoundRect(YES_X, YES_Y, YES_W, YES_H, 14, DARKGREEN);
  gfx->setTextSize(2); gfx->setTextColor(WHITE, DARKGREEN);
  gfx->setCursor(YES_X + 34, YES_Y + 22); gfx->print("HOLD = YES");
  gfx->setTextSize(1);
  gfx->setCursor(YES_X + 52, YES_Y + 52); gfx->print("hold for 1 second");
  gfx->fillRoundRect(NO_X, NO_Y, NO_W, NO_H, 12, MAROON);
  gfx->setTextSize(2); gfx->setTextColor(WHITE, MAROON);
  gfx->setCursor(NO_X + 48, NO_Y + 15); gfx->print("NO");
}

void WakeSoakView::drawStats() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Wake Soak");
  uint16_t fill = sEnabled ? t.accent : t.line;
  gfx->fillRoundRect(TOG_X, TOG_Y, TOG_W, TOG_H, 10, fill);
  gfx->setTextSize(2); gfx->setTextColor(contrastFor(fill), fill);
  gfx->setCursor(TOG_X + 24, TOG_Y + 12); gfx->print(sEnabled ? "Soak: ON" : "Soak: OFF");
  SrcStat st[10];
  int n = readStats(st, 10);
  gfx->setTextSize(1); gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(8, 100); gfx->print("source         n  yes  no  t/o rej");
  int16_t y = 114;
  for (int i = 0; i < n && y < 250; i++, y += 14) {
    char b[64];
    snprintf(b, sizeof b, "%-13.13s %3u %4u %3u %4u %3u", st[i].src, st[i].total,
             st[i].yes + st[i].autoYes, st[i].no, st[i].timeout, st[i].rejected);
    gfx->setTextColor(t.fg, t.bg); gfx->setCursor(8, y); gfx->print(b);
  }
  if (n == 0) { gfx->setTextColor(t.line, t.bg); gfx->setCursor(8, 114); gfx->print("no wakes logged yet"); }
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(8, 258); gfx->print("long-press the toggle to clear the log");
}

void WakeSoakView::render() {
  if (!gfx) return;
  if (sPending) {
    if (firstDraw) { drawPrompt(); firstDraw = false; }
    uint32_t now = millis();
    if (now - sWakeMs >= kPromptMs) { finish("timeout"); return; }
    int secs = (int)((kPromptMs - (now - sWakeMs)) / 1000) + 1;
    int prog = holding ? (int)((now - holdStartMs) * 100 / kHoldMs) : 0;
    if (prog >= 100) { finish("yes"); return; }
    if (secs != shownSecs) {
      shownSecs = secs;
      gfx->fillRect(200, 16, 40, 20, BLACK);
      gfx->setTextSize(2); gfx->setTextColor(YELLOW, BLACK);
      gfx->setCursor(206, 16); gfx->print(secs);
    }
    if (prog != shownProg) {
      shownProg = prog;
      gfx->fillRect(YES_X + 10, YES_Y + YES_H - 10, YES_W - 20, 5, DARKGREEN);
      gfx->fillRect(YES_X + 10, YES_Y + YES_H - 10, (YES_W - 20) * prog / 100, 5, WHITE);
    }
    return;
  }
  if (firstDraw) { drawStats(); firstDraw = false; }
}

static bool hit(const Event &e, int16_t x, int16_t y, int16_t w, int16_t h) {
  return e.x >= x && e.x < x + w && e.y >= y && e.y < y + h;
}

void WakeSoakView::onEvent(const Event &e) {
  if (sPending) {
    // Ignore the wake touch itself (and its trailing frames).
    if (millis() - enterMs < kGraceMs) { if (e.type == EventType::TouchUp) holding = false; return; }
    if (e.type == EventType::ButtonShort) { finish("no"); return; }
    if (e.type == EventType::Touch) {
      if (hit(e, YES_X, YES_Y, YES_W, YES_H)) { holding = true; holdStartMs = millis(); }
      else if (hit(e, NO_X, NO_Y, NO_W, NO_H)) finish("no");
    } else if (e.type == EventType::TouchHold) {
      if (holding && !hit(e, YES_X - 20, YES_Y - 20, YES_W + 40, YES_H + 40)) holding = false;
    } else if (e.type == EventType::TouchUp) {
      holding = false;
    }
    return;
  }
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Gesture && e.gesture == Gesture::SwipeRight)) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Gesture && e.gesture == Gesture::LongPress && hit(e, TOG_X, TOG_Y, TOG_W, TOG_H)) {
    LittleFS.remove(kPath); sCount = 0; gestClear(); hapticBuzz(150, 120); firstDraw = true; return;
  }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
  // Toggle on the tap gesture (not Touch), so a long-press can clear instead.
  if (e.type == EventType::Gesture && e.gesture == Gesture::SingleTap && hit(e, TOG_X, TOG_Y, TOG_W, TOG_H)) {
    setEnabled(!sEnabled); hapticBuzz(60, 30); firstDraw = true;
  }
}
