// Stream (WiFi) — see livestream.h; packet handling lives in core/ewstream.
#if defined(EW_APP_LIVESTREAM) && EW_APP_LIVESTREAM && \
    defined(EWATCH_ENABLE_WIFI)   && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "wifi_svc.h"   // wifiSvcHold: keep the duty-cycled radio up while streaming
#include "livestream.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "ewlog.h"
#include "ewstream.h"

static const uint16_t LISTEN_PORT = 7878;

static WiFiServer        sServer(LISTEN_PORT);
static bool              sServerUp = false;
static WiFiClient        sClient;
static SemaphoreHandle_t sMtx = nullptr;          // sClient: rx task vs replies
static TaskHandle_t      sRxTask = nullptr;
static volatile bool     sRxStop = false;
static volatile bool     sConnected = false;

static void tcpWrite(const uint8_t *p, size_t n) {
  xSemaphoreTake(sMtx, portMAX_DELAY);
  if (sClient && sClient.connected()) sClient.write(p, n);
  xSemaphoreGive(sMtx);
}

// Core 0: accept one client at a time and feed its bytes to the parser.
static void rxTask(void *) {
  static uint8_t buf[1460];
  while (!sRxStop) {
    if (!sConnected) {
      xSemaphoreTake(sMtx, portMAX_DELAY);
      WiFiClient c = sServer.accept();
      if (c) { c.setNoDelay(true); sClient = c; sConnected = true; }
      xSemaphoreGive(sMtx);
      if (!sConnected) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
      ewsResetParser();
      EWLOGI("STRM", "client connected");
      ewsReply(">>READY stream");
    }
    int n = 0;
    bool alive = true;
    xSemaphoreTake(sMtx, portMAX_DELAY);
    int a = sClient.available();
    if (a > 0) n = sClient.read(buf, a < (int)sizeof(buf) ? a : sizeof(buf));
    else alive = sClient.connected();
    xSemaphoreGive(sMtx);
    if (n > 0) { ewsFeed(buf, n); continue; }
    if (!alive) {
      xSemaphoreTake(sMtx, portMAX_DELAY);
      sClient.stop();
      sConnected = false;
      xSemaphoreGive(sMtx);
      ewsResetParser();
      EWLOGI("STRM", "client disconnected");
      continue;
    }
    vTaskDelay(1);
  }
  sRxTask = nullptr;
  vTaskDelete(nullptr);
}

void StreamView::onEnter() {
  showing = drawn = false;
  lastStatMs = millis(); statFrames = 0;
  // EWatchOS duty-cycles WiFi: ask for the radio and hold it while we're on
  // screen (Settings > WiFi must be on, Client mode for ewatch.local).
  wifiSvcHold(true);
  for (int i = 0; i < 150 && WiFi.getMode() == WIFI_OFF; i++) {   // up to ~15 s
    wifiSvcHold(true);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // lwip has no mailbox until esp_wifi starts: server.begin() would assert.
  if (WiFi.getMode() == WIFI_OFF) { st = St::NoWifi; return; }
  if (!sMtx) sMtx = xSemaphoreCreateMutex();
  if (!ewsBegin(tcpWrite)) { st = St::NoMem; return; }
  if (!sServerUp) { sServer.begin(); sServer.setNoDelay(true); sServerUp = true; }
  WiFi.setSleep(false);                     // modem sleep adds 100+ ms per packet
  sRxStop = false; sConnected = false;
  xTaskCreatePinnedToCore(rxTask, "strx", 4096, nullptr, 2, &sRxTask, 0);
  st = St::Live;
}

void StreamView::onExit() {
  if (st == St::Live) {
    sRxStop = true;
    ewsStop();
    for (int i = 0; i < 100 && sRxTask; i++) vTaskDelay(pdMS_TO_TICKS(2));
    xSemaphoreTake(sMtx, portMAX_DELAY);
    sClient.stop(); sConnected = false;
    xSemaphoreGive(sMtx);
    ewsEnd();
  }
  wifiSvcHold(false);
}

void StreamView::drawWaiting() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Stream");
  IPAddress ip = WiFi.localIP();
  if ((uint32_t)ip == 0) ip = WiFi.softAPIP();
  char addr[40];
  snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], LISTEN_PORT);
  gfx->setTextSize(2);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(20, 90);   gfx->print("Waiting for");
  gfx->setCursor(20, 116);  gfx->print("Mac client...");
  gfx->setTextSize(1);
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(20, 160);  gfx->print("Host:");
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(20, 176);  gfx->print("ewatch.local");
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(20, 200);  gfx->print("IP:");
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(20, 216);  gfx->print(addr);
  drawBackButton();
  drawn = true;
}

void StreamView::drawError(const char *msg, const char *hint) {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Stream");
  gfx->setTextSize(2);
  gfx->setTextColor(RED, t.bg);
  gfx->setCursor(20, 130);  gfx->print(msg);
  gfx->setTextSize(1);
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(20, 170);  gfx->print(hint);
  drawBackButton();
  drawn = true;
}

void StreamView::render() {
  if (!gfx) return;
  if (st != St::Live) {
    if (!drawn) {
      if (st == St::NoWifi) drawError("WiFi is OFF", "Enable in Settings > WiFi");
      else drawError("Out of memory", "Close other apps");
    }
    return;
  }
  wifiSvcHold(true);                        // deadman: lapses ~3 s after the last ping
  if (ewsService(10)) { showing = true; statFrames++; }
  uint32_t now = millis();
  if ((showing && now - ewsStats().lastPktMs > 3000) || !drawn) {
    showing = false;
    drawWaiting();
  }
  if (now - lastStatMs >= 5000) {
    const EwsStats &s = ewsStats();
    if (statFrames)
      EWLOGI("STRM", "fps=%lu dec_us=%lu push_us=%lu bad=%lu",
             (unsigned long)(statFrames * 1000 / (now - lastStatMs)),
             (unsigned long)s.decUs, (unsigned long)s.pushUs, (unsigned long)s.bad);
    lastStatMs = now; statFrames = 0;
  }
}

void StreamView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Touch && !showing && tappedBack(e.x, e.y))) {
    hapticBuzz(120, 20);
    switchTo(Screen::AppList);
  }
}

#endif  // gated
