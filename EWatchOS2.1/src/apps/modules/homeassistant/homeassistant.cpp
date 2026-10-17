// Home Assistant light remote — implementation.
//
// See homeassistant.h for the architecture note. This whole file is gated on
// the WiFi feature flag so a no-WiFi build still links (the view then shows a
// "needs WiFi" message). We keep WiFi ON for this app.

#include "homeassistant.h"
#include <Arduino_GFX_Library.h>
#include "display.h"   // gfx, frameCanvas, theme
#include "haptic.h"    // hapticBuzz
#include "haptic_roles.h"
#include "model.h"     // model + ModelLock
#include "FreeSansBold24pt7b.h"  // big ON/OFF label font

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>

// Compile-time config (copy secrets/ha.example.h -> secrets/ha.local.h).
#if __has_include("ha.local.h")   // secrets/ha.local.h (git-ignored): your HA URL + token
#  include "ha.local.h"
#else
#  include "ha.example.h"
#endif

// ===================================================================
// service state (owned by the background task, read via snapshot)
// ===================================================================
static SemaphoreHandle_t haMutex  = nullptr;
static QueueHandle_t     haCmdQ   = nullptr;
static HASnapshot        snap;

struct HALock {
  HALock()  { if (haMutex) xSemaphoreTake(haMutex, portMAX_DELAY); }
  ~HALock() { if (haMutex) xSemaphoreGive(haMutex); }
};

// Bound every network op so a stalled connect on a weak link can't hang the
// task past the 20 s task-watchdog.
static const int HTTP_CONNECT_MS = 6000;
static const int HTTP_READ_MS    = 6000;

static const uint32_t POLL_MS       = 5000;   // steady-state state poll cadence
static const uint32_t POLL_ERR_MS   = 8000;   // backoff after an error

// True while the token still reads as the shipped placeholder — the app then
// shows "set token" instead of hammering a bogus endpoint.
static bool configLooksUnset() {
  return strcmp(HA_TOKEN, "PASTE_YOUR_LONG_LIVED_TOKEN_HERE") == 0 ||
         HA_TOKEN[0] == '\0';
}

const char *haLightName() {
  static const char *name = (HA_LIGHT_NAME[0] ? HA_LIGHT_NAME : HA_ENTITY_ID);
  return name;
}

// ===================================================================
// REST helpers (plain HTTP + Bearer token)
// ===================================================================
// One request. Returns HTTP status (negative = connection/lib error). When
// `out` is non-null the (small) response body is captured for JSON parsing.
static int haRequest(const char *method, const String &url,
                     const String &body, String *out) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_MS);
  http.setTimeout(HTTP_READ_MS);
  http.setReuse(false);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  if (body.length()) http.addHeader("Content-Type", "application/json");

  esp_task_wdt_reset();
  int code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
  if (code > 0 && out) *out = http.getString();
  http.end();
  return code;
}

// GET /api/states/<entity> -> update snap.light / snap.conn. Returns status.
static int pollState() {
  String url = String(HA_BASE_URL) + "/api/states/" + HA_ENTITY_ID;
  String resp;
  int code = haRequest("GET", url, "", &resp);

  HALight light = HALight::Unknown;
  if (code == 200) {
    // Only the "state" field matters; filter the rest out so a big attribute
    // blob can't blow the parser.
    JsonDocument filter; filter["state"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
      const char *st = doc["state"] | "";
      if      (!strcmp(st, "on"))  light = HALight::On;
      else if (!strcmp(st, "off")) light = HALight::Off;
      else                         light = HALight::Unavailable;  // unavailable/unknown
    }
  }

  { HALock lk;
    snap.lastHttpStatus = code;
    if (code == 200) { snap.conn = HAConn::Online; snap.light = light; }
    else             { snap.conn = HAConn::Error; }
  }
  return code;
}

// POST /api/services/light/turn_on|turn_off {"entity_id": "<entity>"}
static int callService(bool turnOn) {
  String url  = String(HA_BASE_URL) + "/api/services/light/" +
                (turnOn ? "turn_on" : "turn_off");
  String body = String("{\"entity_id\":\"") + HA_ENTITY_ID + "\"}";
  return haRequest("POST", url, body, nullptr);
}

static void runToggle() {
  { HALock lk; snap.busy = true; }

  // Decide the target from the last known state; default to turn_on when we
  // have never polled a definite value.
  HALight cur; { HALock lk; cur = snap.light; }
  bool turnOn = (cur != HALight::On);   // On -> off, everything else -> on

  int code = callService(turnOn);

  { HALock lk;
    snap.lastHttpStatus = code;
    if (code >= 200 && code < 300) {
      snap.conn  = HAConn::Online;
      snap.light = turnOn ? HALight::On : HALight::Off;  // optimistic; poll confirms
    } else {
      snap.conn = HAConn::Error;
    }
    snap.busy = false;
  }
}

// ===================================================================
// background service task
// ===================================================================
static void serviceTask(void *) {
  esp_task_wdt_add(nullptr);
  uint32_t nextPollMs = 0;

  for (;;) {
    esp_task_wdt_reset();

    if (configLooksUnset()) {
      { HALock lk; snap.conn = HAConn::NeedConfig; }
      // Still drain the command queue so taps don't pile up.
      HACmd drop; while (haCmdQ && xQueueReceive(haCmdQ, &drop, 0) == pdPASS) {}
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    bool wifiOk; { ModelLock lk; wifiOk = model.wifiConnected; }
    if (!wifiOk) {
      { HALock lk; snap.conn = HAConn::NeedWifi; snap.light = HALight::Unknown; }
      HACmd drop; while (haCmdQ && xQueueReceive(haCmdQ, &drop, 0) == pdPASS) {}
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Drain queued commands (toggle / refresh).
    HACmd msg;
    bool forcePoll = false;
    for (int n = 0; n < 4 && haCmdQ && xQueueReceive(haCmdQ, &msg, 0) == pdPASS; n++) {
      esp_task_wdt_reset();
      if (msg == HACmd::Toggle) { runToggle(); forcePoll = true; }
      else                      { forcePoll = true; }   // Refresh
    }

    // Poll state on cadence (or immediately after a command).
    if (forcePoll || (int32_t)(millis() - nextPollMs) >= 0) {
      if (snap.conn == HAConn::NeedWifi || snap.conn == HAConn::NeedConfig) {
        { HALock lk; snap.conn = HAConn::Connecting; }
      }
      int code = pollState();
      nextPollMs = millis() + (code == 200 ? POLL_MS : POLL_ERR_MS);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ===================================================================
// public API
// ===================================================================
void haInit() {
  if (!haMutex) haMutex = xSemaphoreCreateMutex();
  if (!haCmdQ)  haCmdQ  = xQueueCreate(8, sizeof(HACmd));

  memset(&snap, 0, sizeof(snap));
  snap.conn  = HAConn::NeedWifi;
  snap.light = HALight::Unknown;
  snap.busy  = false;
  snap.lastHttpStatus = 0;

  Serial.printf("HA: init (entity %s, token %s)\n", HA_ENTITY_ID,
                configLooksUnset() ? "UNSET" : "set");
}

void haSvcStartTask() {
  // Stack sized for HTTPClient + ArduinoJson; no TLS here (plain HTTP).
  xTaskCreatePinnedToCore(serviceTask, "ha", 8192, nullptr, 4, nullptr, 0);
}

void haGetSnapshot(HASnapshot *out) { HALock lk; *out = snap; }

void haPostCmd(HACmd cmd) {
  if (haCmdQ) xQueueSend(haCmdQ, &cmd, 0);
}

// ===================================================================
// view
// ===================================================================
static const int16_t SCREEN_W = 240;
static const int16_t SCREEN_H = 280;

// State -> dot colour.
static uint16_t lightColor(HALight l) {
  switch (l) {
    case HALight::On:          return RGB565(255, 196, 64);   // warm amber
    case HALight::Off:         return RGB565(70, 70, 78);     // dim grey
    case HALight::Unavailable: return RGB565(200, 60, 60);    // red
    default:                   return RGB565(110, 110, 120);  // unknown
  }
}

static const char *lightText(HALight l) {
  switch (l) {
    case HALight::On:          return "ON";
    case HALight::Off:         return "OFF";
    case HALight::Unavailable: return "N/A";
    default:                   return "--";
  }
}

void HomeAssistantView::onEnter() {
  // OS2.1: self-init lazily (no main.cpp wiring). haInit() is idempotent; the
  // background task is spawned exactly once, the first time the app is opened.
  static bool started = false;
  if (!started) { haInit(); haSvcStartTask(); started = true; }

  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw   = true;
  shownConn   = (HAConn)0xFF;
  shownLight  = (HALight)0xFF;
  shownBusy   = false;
  shownStatus = 0x7FFFFFFF;
  optimistic  = false;
  haPostCmd(HACmd::Refresh);   // get fresh state on open
}

void HomeAssistantView::drawButton(const HASnapshot &s, bool full) {
  ThemeColors t = theme();

  // Effective light state (optimistic echo until the service confirms).
  HALight eff = s.light;
  if (optimistic && s.busy) eff = optimisticLight;

  // Big round button.
  btnW = 160; btnH = 160;
  btnX = (SCREEN_W - btnW) / 2;
  btnY = 70;

  bool changed = full || eff != shownLight || s.busy != shownBusy;
  if (!changed) return;

  uint16_t fill = lightColor(eff);
  uint16_t ring = (eff == HALight::On) ? RGB565(255, 230, 150) : t.line;

  gfx->fillRoundRect(btnX, btnY, btnW, btnH, 28, fill);
  gfx->drawRoundRect(btnX, btnY, btnW, btnH, 28, ring);
  gfx->drawRoundRect(btnX + 1, btnY + 1, btnW - 2, btnH - 2, 27, ring);

  // Center label (ON / OFF / N/A / --).
  uint16_t txt = contrastFor(fill);
  const char *lbl = s.busy ? "..." : lightText(eff);
  gfx->setFont(&FreeSansBold24pt7b);
  gfx->setTextColor(txt);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(lbl, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(btnX + (btnW - (int)w) / 2 - x1,
                 btnY + (btnH - (int)h) / 2 - y1);
  gfx->print(lbl);
  gfx->setFont(nullptr);

  shownLight = eff;
  shownBusy  = s.busy;
}

void HomeAssistantView::drawStatus(const HASnapshot &s, bool full) {
  ThemeColors t = theme();
  bool changed = full || s.conn != shownConn || s.lastHttpStatus != shownStatus;
  if (!changed) return;

  // Clear the status band.
  gfx->fillRect(0, 240, SCREEN_W, 40, t.bg);

  const char *line = "";
  uint16_t col = t.fg;
  switch (s.conn) {
    case HAConn::NeedWifi:   line = "WiFi not connected";       col = RGB565(220,170,60); break;
    case HAConn::NeedConfig: line = "Set token in ha.local.h";  col = RGB565(220,120,60); break;
    case HAConn::Connecting: line = "Connecting...";            col = t.fg; break;
    case HAConn::Online:     line = (s.light == HALight::On) ? "Light is on"
                                  : (s.light == HALight::Off) ? "Light is off"
                                  : "Unavailable";              col = t.fg; break;
    case HAConn::Error:      line = "HA unreachable";           col = RGB565(220,80,80); break;
  }

  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  gfx->setTextColor(col, t.bg);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((SCREEN_W - (int)w) / 2 - x1, 250);
  gfx->print(line);

  // Tiny HTTP status in the corner for debugging (only when meaningful).
  if (s.lastHttpStatus != 0) {
    char buf[16]; snprintf(buf, sizeof(buf), "%d", s.lastHttpStatus);
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(SCREEN_W - 30, 267);
    gfx->print(buf);
  }

  shownConn   = s.conn;
  shownStatus = s.lastHttpStatus;
}

void HomeAssistantView::render() {
  if (!gfx) return;
  HASnapshot s; haGetSnapshot(&s);

  // Clear optimistic echo once the service is no longer busy.
  if (optimistic && !s.busy) optimistic = false;

  bool full = firstDraw;
  if (full) {
    ThemeColors t = theme();
    gfx->fillScreen(t.bg);
    drawTitleBar(haLightName());
    firstDraw = false;
  }

  drawButton(s, full);
  drawStatus(s, full);
}

void HomeAssistantView::onEvent(const Event &e) {
  // Back: hardware button or right-swipe both arrive as ButtonShort.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }

  if (e.type == EventType::Touch) {
    bool inBtn = e.x >= btnX && e.x < btnX + btnW &&
                 e.y >= btnY && e.y < btnY + btnH;
    if (inBtn) {
      HASnapshot s; haGetSnapshot(&s);
      // Only act when we can actually reach HA.
      if (s.conn == HAConn::Online || s.conn == HAConn::Error ||
          s.conn == HAConn::Connecting) {
        haptic(HapticRole::Tap);
        // Optimistic flip so the button reacts instantly.
        optimistic      = true;
        optimisticLight = (s.light == HALight::On) ? HALight::Off : HALight::On;
        haPostCmd(HACmd::Toggle);
      } else {
        haptic(HapticRole::Failed);   // tapped, but HA is unreachable
      }
    }
  }
}

#else  // !EWATCH_ENABLE_WIFI ------------------------------------------------

// WiFi compiled out: provide no-op lifecycle + a view that explains why.
void haInit() {}
void haSvcStartTask() {}
void haGetSnapshot(HASnapshot *out) {
  if (out) { out->conn = HAConn::NeedWifi; out->light = HALight::Unknown;
             out->busy = false; out->lastHttpStatus = 0; }
}
void haPostCmd(HACmd) {}
const char *haLightName() { return "Home"; }

void HomeAssistantView::onEnter() {
  // WiFi compiled out: no task to start; just paint the explainer.
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true;
}
void HomeAssistantView::drawButton(const HASnapshot &, bool) {}
void HomeAssistantView::drawStatus(const HASnapshot &, bool) {}
void HomeAssistantView::render() {
  if (!gfx) return;
  if (firstDraw) {
    ThemeColors t = theme();
    gfx->fillScreen(t.bg);
    drawTitleBar("Home");
    gfx->setFont(nullptr); gfx->setTextSize(1);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(20, 120);
    gfx->print("WiFi disabled in build");
    firstDraw = false;
  }
}
void HomeAssistantView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) switchTo(Screen::AppList);
}

#endif
