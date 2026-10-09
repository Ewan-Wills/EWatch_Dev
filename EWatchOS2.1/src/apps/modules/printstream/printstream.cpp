// printstream — background network service (Moonraker polling + MJPEG camera +
// confirmed control commands). See printstream.h for the architecture note.
//
// Everything in this file is compiled out when WiFi is gated off; the public
// API degrades to harmless stubs at the bottom so the app still links and the
// view can show a "needs WiFi" message.
#include "printstream.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>
#include "model.h"

// Printer connection details: prefer the git-ignored local header, fall back to
// the tracked example so the firmware always compiles.
#if __has_include("printer.local.h")
#  include "printer.local.h"
#else
#  include "printer.example.h"
#endif

// ---- tunables ----
static const int      HTTP_CONNECT_MS = 4000;
static const int      HTTP_READ_MS    = 5000;
static const uint32_t CAM_STALL_MS    = 6000;    // give up on a silent stream
static const size_t   CAM_MAX         = 300 * 1024; // frame buffer cap (PSRAM)

// ---- shared state ----
static SemaphoreHandle_t psMutex;     // guards `snap`
static SemaphoreHandle_t camMutex;    // guards camBuf/camLen/camGen
static QueueHandle_t     cmdQueue;    // PsCmd messages

static PsSnapshot snap;               // the one snapshot the view reads

static uint8_t  *camBuf = nullptr;
static size_t    camLen = 0;
static uint32_t  camGen = 0;

static volatile bool camWanted = false;   // view on the camera page?
static volatile bool resyncNow = false;

struct Lock { SemaphoreHandle_t m; bool ok;
  explicit Lock(SemaphoreHandle_t s):m(s){ ok = m && xSemaphoreTake(m, portMAX_DELAY)==pdTRUE; }
  ~Lock(){ if(ok) xSemaphoreGive(m); } };

// ---- URL helpers ----
static String moonrakerUrl(const char *path) {
  return String("http://") + PRINTER_HOST + ":" + String((int)MOONRAKER_PORT) + path;
}
static String camUrl(const char *path) {
  return String("http://") + CAM_HOST + ":" + String((int)CAM_PORT) + path;
}

static void prepHttp(HTTPClient &h) {
  h.setConnectTimeout(HTTP_CONNECT_MS);
  h.setTimeout(HTTP_READ_MS);
  h.setReuse(false);
}

static bool wifiReady() {
  bool ok; { ModelLock lk; ok = model.wifiConnected; }
  return ok;
}

// basename of a gcode path -> dst
static void baseName(const char *in, char *dst, size_t cap) {
  if (!in) { dst[0] = '\0'; return; }
  const char *slash = strrchr(in, '/');
  const char *b = slash ? slash + 1 : in;
  strncpy(dst, b, cap - 1); dst[cap - 1] = '\0';
}

static PsState parseState(const char *s) {
  if (!s) return PsState::Unknown;
  if (!strcmp(s, "standby"))   return PsState::Standby;
  if (!strcmp(s, "printing"))  return PsState::Printing;
  if (!strcmp(s, "paused"))    return PsState::Paused;
  if (!strcmp(s, "complete"))  return PsState::Complete;
  if (!strcmp(s, "cancelled")) return PsState::Cancelled;
  if (!strcmp(s, "error"))     return PsState::Error;
  return PsState::Unknown;
}

// ===================================================================
// Moonraker poll
// ===================================================================
// GET /printer/objects/query?print_stats&heater_bed&extruder&display_status
// The reply is small (~600 B) so we read it whole and parse it directly.
static int pollMoonraker() {
  esp_task_wdt_reset();
  WiFiClient client;
  HTTPClient http; prepHttp(http);
  String url = moonrakerUrl("/printer/objects/query"
                            "?print_stats&heater_bed&extruder&display_status");
  if (!http.begin(client, url)) return -1;
  int code = http.GET();
  if (code != 200) { http.end(); return code; }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return -2;
  JsonObject st = doc["result"]["status"];
  if (st.isNull()) return -3;

  JsonObject ps  = st["print_stats"];
  JsonObject bed = st["heater_bed"];
  JsonObject ext = st["extruder"];
  JsonObject ds  = st["display_status"];

  const char *fn    = ps["filename"] | "";
  const char *state = ps["state"]    | "";
  float printDur    = ps["print_duration"] | 0.0f;
  float prog        = ds["progress"] | 0.0f;          // 0..1
  float nT = ext["temperature"] | 0.0f, nTgt = ext["target"] | 0.0f;
  float bT = bed["temperature"] | 0.0f, bTgt = bed["target"] | 0.0f;

  int pct = (int)(prog * 100.0f + 0.5f);
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;

  { Lock lk(psMutex);
    baseName(fn, snap.filename, sizeof(snap.filename));
    snap.state          = parseState(state);
    snap.progressPct    = (uint8_t)pct;
    snap.nozzleTemp     = nT;  snap.nozzleTarget = nTgt;
    snap.bedTemp        = bT;  snap.bedTarget    = bTgt;
    snap.printDurationS = (uint32_t)printDur;
    snap.link           = PsLink::Online;
    snap.lastHttpStatus = 200; }
  return 200;
}

// ===================================================================
// control commands (run on psNetTask AFTER the view's confirm step)
// ===================================================================
static void runCommand(PsCmd cmd) {
  const char *path = nullptr;
  switch (cmd) {
    case PsCmd::Pause:  path = "/printer/print/pause";  break;
    case PsCmd::Resume: path = "/printer/print/resume"; break;
    case PsCmd::Home:   path = "/printer/gcode/script?script=G28"; break;
    case PsCmd::Resync: resyncNow = true; return;
  }
  if (!path) return;
  esp_task_wdt_reset();
  WiFiClient client;
  HTTPClient http; prepHttp(http);
  String url = moonrakerUrl(path);
  if (!http.begin(client, url)) { Serial.println("PS: cmd begin failed"); return; }
  http.addHeader("Content-Length", "0");
  int code = http.POST((uint8_t *)nullptr, 0);
  http.end();
  Serial.printf("PS: cmd %d -> HTTP %d\n", (int)cmd, code);
  resyncNow = true;   // reflect the new state fast
}

// ===================================================================
// camera: swap a freshly-read JPEG into the shared buffer
// ===================================================================
static void installFrame(uint8_t *buf, size_t len) {
  Lock lk(camMutex);
  if (camBuf) free(camBuf);
  camBuf = buf; camLen = len; camGen++;
}

static void setCam(PsCamState c) { Lock lk(psMutex); snap.cam = c; }

// Read exactly `len` bytes from a stream into buf with a stall guard.
static bool readExact(WiFiClient *s, uint8_t *buf, size_t len) {
  size_t got = 0; uint32_t last = millis();
  while (got < len) {
    int avail = s->available();
    if (avail > 0) {
      size_t want = len - got; if ((int)want > avail) want = avail;
      int r = s->readBytes(buf + got, want);
      if (r <= 0) return false;
      got += r; last = millis();
    } else {
      if (millis() - last > CAM_STALL_MS) return false;
      if (!s->connected() && s->available() == 0) return false;
      vTaskDelay(pdMS_TO_TICKS(3));
    }
    esp_task_wdt_reset();
  }
  return true;
}

// Read one header line (up to CRLF) into buf. Returns false on stall/close.
static bool readHeaderLine(WiFiClient *s, char *buf, size_t cap) {
  size_t n = 0; uint32_t last = millis();
  for (;;) {
    int c = s->read();
    if (c < 0) {
      if (millis() - last > CAM_STALL_MS) return false;
      if (!s->connected() && s->available() == 0) return false;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    last = millis();
    if (c == '\n') { // strip trailing \r
      if (n && buf[n - 1] == '\r') n--;
      buf[n] = '\0';
      return true;
    }
    if (n < cap - 1) buf[n++] = (char)c;
  }
}

// Pump the MJPEG multipart stream, installing each complete JPEG frame.
// Returns when the camera is no longer wanted or the stream breaks. `true` if
// at least one frame was decoded (so the caller doesn't fall back to snapshot).
static bool streamMjpeg() {
  WiFiClient client;
  HTTPClient http; prepHttp(http);
  if (!http.begin(client, camUrl(CAM_STREAM_PATH))) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("PS: cam stream HTTP %d\n", code); http.end(); return false; }

  String ctype = http.header("Content-Type");
  WiFiClient *s = http.getStreamPtr();
  bool gotAny = false;
  char line[160];

  // Walk frames: skip to the part headers, read Content-Length, read payload.
  while (camWanted && wifiReady()) {
    esp_task_wdt_reset();
    long clen = -1;
    // Each part is:  [leading CRLF]  --boundary  Content-* headers  blank line
    // then the JPEG payload. The leading blank line (the CRLF that terminated
    // the previous frame's data) must be skipped, not mistaken for the blank
    // line that ends THIS part's headers.
    int  hdrs = 0;
    bool inHeaders = false;
    for (;;) {
      if (!readHeaderLine(s, line, sizeof(line))) { http.end(); return gotAny; }
      if (line[0] == '\0') {
        if (inHeaders) break;        // blank line ends the part headers
        continue;                    // leading CRLF before the boundary — skip
      }
      if (line[0] == '-' && line[1] == '-') { inHeaders = true; clen = -1; continue; } // boundary
      inHeaders = true;
      if (!strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
      if (++hdrs > 20) { http.end(); return gotAny; }  // runaway guard
    }
    if (clen <= 0 || (size_t)clen > CAM_MAX) {
      // No usable length (or absurd) — bail to snapshot fallback.
      http.end(); return gotAny;
    }
    uint8_t *buf = (uint8_t *)ps_malloc((size_t)clen);
    if (!buf) { http.end(); return gotAny; }
    if (!readExact(s, buf, (size_t)clen)) { free(buf); http.end(); return gotAny; }
    installFrame(buf, (size_t)clen);
    gotAny = true;
    setCam(PsCamState::Streaming);
    // The trailing CRLF before the next boundary is consumed by the next
    // readHeaderLine()s (they skip empty/boundary lines).
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  http.end();
  return gotAny;
}

// One snapshot fetch into a fresh buffer; installs it on success.
static bool snapshotOnce() {
  esp_task_wdt_reset();
  WiFiClient client;
  HTTPClient http; prepHttp(http);
  if (!http.begin(client, camUrl(CAM_SNAPSHOT_PATH))) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  int total = http.getSize();
  size_t cap = (total > 0 && (size_t)total < CAM_MAX) ? (size_t)total : CAM_MAX;
  uint8_t *buf = (uint8_t *)ps_malloc(cap);
  if (!buf) { http.end(); return false; }
  WiFiClient *s = http.getStreamPtr();
  size_t got = 0; uint32_t last = millis();
  while (http.connected() && got < cap && (total < 0 || got < (size_t)total)) {
    int avail = s->available();
    if (avail > 0) {
      size_t want = cap - got; if ((int)want > avail) want = avail;
      int r = s->readBytes(buf + got, want);
      if (r <= 0) break;
      got += r; last = millis();
    } else {
      if (millis() - last > CAM_STALL_MS) break;
      vTaskDelay(pdMS_TO_TICKS(4));
    }
    esp_task_wdt_reset();
  }
  http.end();
  if (got < 64 || (total > 0 && got < (size_t)total)) { free(buf); return false; }
  installFrame(buf, got);
  return true;
}

// ===================================================================
// tasks
// ===================================================================
static uint32_t nextPollMs = 0;
static int      pollErrs   = 0;

static void psNetTask(void *) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();

    if (!wifiReady()) {
      { Lock lk(psMutex); snap.link = PsLink::NeedWifi; }
      // drop any queued commands so they don't fire stale when WiFi returns
      PsCmd drop; while (cmdQueue && xQueueReceive(cmdQueue, &drop, 0) == pdPASS) {}
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // run queued (already-confirmed) control commands first
    PsCmd msg;
    for (int n = 0; n < 4 && cmdQueue && xQueueReceive(cmdQueue, &msg, 0) == pdPASS; n++) {
      esp_task_wdt_reset();
      runCommand(msg);
    }

    if (resyncNow) { resyncNow = false; nextPollMs = 0; }

    if ((int32_t)(millis() - nextPollMs) >= 0) {
      int st = pollMoonraker();
      uint32_t gap;
      if (st == 200) {
        pollErrs = 0;
        PsState s; { Lock lk(psMutex); s = snap.state; }
        gap = (s == PsState::Printing || s == PsState::Paused)
                ? PS_POLL_PRINTING_MS : PS_POLL_IDLE_MS;
      } else {
        if (++pollErrs >= 2) { Lock lk(psMutex); snap.link = PsLink::Offline; snap.lastHttpStatus = st; }
        int shift = pollErrs > 3 ? 3 : pollErrs;
        gap = 1500u << shift;
        if (gap > 12000) gap = 12000;
      }
      nextPollMs = millis() + gap;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void psCamTask(void *) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();

    if (!camWanted || !wifiReady()) {
      setCam(PsCamState::Off);
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    setCam(PsCamState::Connecting);
    bool streamed = false;
#if defined(CAM_USE_MJPEG) && CAM_USE_MJPEG
    streamed = streamMjpeg();   // returns on break / no-longer-wanted
#endif
    if (!camWanted) continue;

    if (!streamed) {
      // Fallback: periodic single-frame snapshots.
      bool ok = snapshotOnce();
      setCam(ok ? PsCamState::SnapshotOk : PsCamState::Unavailable);
      // honour the snapshot cadence, but stay responsive to leaving the page
      uint32_t wait = ok ? CAM_SNAPSHOT_MS : 2000;
      uint32_t until = millis() + wait;
      while (camWanted && (int32_t)(millis() - until) < 0) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }
}

// ===================================================================
// public API
// ===================================================================
void printstreamInit() {
  if (!psMutex)  psMutex  = xSemaphoreCreateMutex();
  if (!camMutex) camMutex = xSemaphoreCreateMutex();
  if (!cmdQueue) cmdQueue = xQueueCreate(8, sizeof(PsCmd));
  { Lock lk(psMutex);
    memset(&snap, 0, sizeof(snap));
    snap.link = PsLink::NeedWifi;
    snap.state = PsState::Unknown;
    snap.cam = PsCamState::Off; }
}

static bool psTasksStarted = false;

void printstreamStartTasks() {
  if (psTasksStarted) return;   // idempotent: spawn the tasks at most once
  // 8 KB net task; 10 KB camera task (HTTPClient + multipart buffers).
  xTaskCreatePinnedToCore(psNetTask, "ps_net", 8192,  nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(psCamTask, "ps_cam", 10240, nullptr, 3, nullptr, 0);
  psTasksStarted = true;
}

// OS2.1: called from the view's onEnter(). Idempotent — safe to call on every
// entry; the first call creates the sync primitives and spawns the tasks, and
// subsequent calls are no-ops. Replaces the BaseOS main.cpp wiring.
void printstreamEnsureStarted() {
  printstreamInit();        // idempotent (guards each primitive)
  printstreamStartTasks();  // idempotent (psTasksStarted guard)
}

void printstreamGetSnapshot(PsSnapshot *out) {
  if (!out) return;
  Lock lk(psMutex);
  *out = snap;
}

uint32_t psCamGen() { return camGen; }

const uint8_t *psCamLock(size_t *lenOut) {
  if (!camMutex) { if (lenOut) *lenOut = 0; return nullptr; }
  xSemaphoreTake(camMutex, portMAX_DELAY);
  if (!camBuf) { xSemaphoreGive(camMutex); if (lenOut) *lenOut = 0; return nullptr; }
  if (lenOut) *lenOut = camLen;
  return camBuf;   // caller MUST call psCamUnlock()
}
void psCamUnlock() { if (camMutex) xSemaphoreGive(camMutex); }

void psSetCameraWanted(bool wanted) { camWanted = wanted; }

void printstreamPostCmd(PsCmd cmd) {
  if (cmdQueue) xQueueSend(cmdQueue, &cmd, 0);
}

#else  // ---- WiFi gated off: harmless stubs so the app still links ----

void printstreamInit() {}
void printstreamStartTasks() {}
void printstreamEnsureStarted() {}
void printstreamGetSnapshot(PsSnapshot *out) {
  if (out) { memset(out, 0, sizeof(*out)); out->link = PsLink::NeedWifi; }
}
uint32_t       psCamGen() { return 0; }
const uint8_t *psCamLock(size_t *lenOut) { if (lenOut) *lenOut = 0; return nullptr; }
void           psCamUnlock() {}
void           psSetCameraWanted(bool) {}
void           printstreamPostCmd(PsCmd) {}

#endif
