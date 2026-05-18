// Live frame receiver. See livestream.h for the wire protocol summary.
//
// Threading: render() runs on the render task. It opens a non-blocking TCP
// accept loop, pumps the event queue at ~500 Hz while waiting, and once a
// client connects it interleaves recv → decode → blit. There is no
// per-session task spawned — single-client by design.
//
// PSRAM budget: ~270 KB held for the life of the firmware after first entry
// (134 KB framebuffer + 138 KB rx scratch). Allocated lazily so users who
// never open the app pay nothing.
#if defined(EWATCH_ENABLE_STREAM) && EWATCH_ENABLE_STREAM && \
    defined(EWATCH_ENABLE_WIFI)   && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>

#include "livestream.h"
#include "display.h"
#include "event.h"
#include "haptic.h"
#include "model.h"
#include "lz4_decode.h"

static const uint16_t LISTEN_PORT = 7878;
static const int16_t  W = 240;
static const int16_t  H = 280;
static const uint32_t FRAME_BYTES = (uint32_t)W * H * 2u;     // 134 400
// Receive scratch: oversized to absorb the worst-case LZ4 expansion (input
// can be slightly larger than the uncompressed size on incompressible data).
static const size_t   RX_CAP = FRAME_BYTES + 4096;

#pragma pack(push, 1)
struct FrameHeader {
  uint8_t  magic;
  uint8_t  flags;
  uint16_t reserved;
  uint32_t len;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 8, "FrameHeader must be 8 bytes on the wire");

static const uint8_t MAGIC    = 0xE1;
static const uint8_t FLAG_LZ4 = 0x01;

static uint16_t  *frameBuf = nullptr;
static uint8_t   *rxBuf    = nullptr;
static WiFiServer server(LISTEN_PORT);
static bool       serverUp = false;

// True when a Touch / ButtonShort / TimerExpired tells us to leave the app.
struct ExitReq { bool wanted; Screen target; };

// Poll the event queue without blocking. Returns true if an exit-class event
// fired; e is unspecified if false.
static bool drainOneExit(ExitReq &req) {
  Event e;
  if (xQueueReceive(eventQueue, &e, 0) != pdPASS) return false;
  if (e.type == EventType::ButtonShort) { req.wanted = true; return true; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) { req.wanted = true; return true; }
  if (e.type == EventType::TimerExpired) { req.wanted = true; req.target = Screen::Timer; return true; }
  return false;
}

// Block until exactly n bytes have been read into dst, or the client drops,
// or the user cancels, or we go 5 s without seeing a single byte. Yields to
// other tasks (and feeds the task WDT) while waiting on the socket.
static bool recvExact(WiFiClient &c, uint8_t *dst, size_t n, ExitReq &req) {
  size_t  got      = 0;
  uint32_t lastByte = millis();
  uint32_t lastWdt  = millis();
  while (got < n) {
    int r = c.read(dst + got, n - got);    // returns immediately, 0 if empty
    if (r > 0) {
      got += r;
      lastByte = millis();
      continue;                            // pull again — there may be more
    }
    if (!c.connected() && c.available() == 0) return false;
    if (drainOneExit(req)) return false;
    if (millis() - lastByte > 5000) return false;            // stalled
    if (millis() - lastWdt  > 500)  { esp_task_wdt_reset(); lastWdt = millis(); }
    vTaskDelay(1);                         // single-tick yield
  }
  return true;
}

static void drawWaiting() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Stream");

  IPAddress ip = WiFi.localIP();
  if ((uint32_t)ip == 0) ip = WiFi.softAPIP();
  char addr[40];
  snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u",
           ip[0], ip[1], ip[2], ip[3], LISTEN_PORT);

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
}

static void drawError(const char *msg) {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Stream");
  gfx->setTextSize(2);
  gfx->setTextColor(RED, t.bg);
  gfx->setCursor(20, 130);  gfx->print(msg);
  drawBackButton();
}

// Pump one connected client until the user cancels or the link drops.
// Returns true if the user asked to leave the whole app; false if the client
// merely disconnected and we should go back to "waiting".
static bool streamClient(WiFiClient &c, ExitReq &req) {
  c.setNoDelay(true);
  gfx->fillScreen(BLACK);
  hapticBuzz(40, 30);

  uint32_t frames   = 0;
  uint32_t lastStat = millis();
  uint32_t tRecv = 0, tDec = 0, tDraw = 0;   // accumulated per second

  for (;;) {
    uint32_t t0 = millis();

    FrameHeader hdr;
    if (!recvExact(c, (uint8_t *)&hdr, sizeof(hdr), req)) return req.wanted;
    if (hdr.magic != MAGIC || hdr.len == 0 || hdr.len > RX_CAP) {
      Serial.printf("STREAM: bad header magic=0x%02X len=%u — dropping\n",
                    hdr.magic, (unsigned)hdr.len);
      return false;
    }
    if (!recvExact(c, rxBuf, hdr.len, req)) return req.wanted;
    uint32_t t1 = millis();

    if (hdr.flags & FLAG_LZ4) {
      int dec = lz4_decompress_block(rxBuf, (int)hdr.len,
                                     (uint8_t *)frameBuf, (int)FRAME_BYTES);
      if (dec != (int)FRAME_BYTES) {
        Serial.printf("STREAM: lz4 decode failed (got %d, want %u)\n",
                      dec, (unsigned)FRAME_BYTES);
        return false;
      }
    } else {
      if (hdr.len != FRAME_BYTES) {
        Serial.printf("STREAM: raw frame wrong size %u\n", (unsigned)hdr.len);
        return false;
      }
      memcpy(frameBuf, rxBuf, FRAME_BYTES);
    }
    uint32_t t2 = millis();

    gfx->draw16bitRGBBitmap(0, 0, frameBuf, W, H);
    uint32_t t3 = millis();

    tRecv += (t1 - t0);
    tDec  += (t2 - t1);
    tDraw += (t3 - t2);
    frames++;
    esp_task_wdt_reset();                    // feed WDT every frame

    uint32_t now = millis();
    if (now - lastStat >= 1000) {
      uint32_t f = frames ? frames : 1;
      Serial.printf("STREAM: %lu FPS  recv=%lu ms  dec=%lu ms  draw=%lu ms (avg/frame)\n",
                    (unsigned long)(frames * 1000u / (now - lastStat)),
                    (unsigned long)(tRecv / f),
                    (unsigned long)(tDec  / f),
                    (unsigned long)(tDraw / f));
      frames = 0; tRecv = tDec = tDraw = 0;
      lastStat = now;
    }

    // Opportunistic cancel check between frames (recvExact also checks).
    if (drainOneExit(req)) return req.wanted;
  }
}

void StreamView::onEnter() {
  // Refuse to touch lwip if WiFi has never been brought up — server.begin()
  // would assert in tcpip_send_msg_wait_sem ("Invalid mbox") because the
  // TCP/IP thread mailbox is only created when esp_wifi is started.
  if (WiFi.getMode() == WIFI_OFF) return;

  if (!frameBuf) frameBuf = (uint16_t *)ps_malloc(FRAME_BYTES);
  if (!rxBuf)    rxBuf    = (uint8_t  *)ps_malloc(RX_CAP);
  if (!serverUp) {
    server.begin();
    server.setNoDelay(true);
    serverUp = true;
  }
}

void StreamView::render() {
  if (!gfx) return;
  ExitReq req{false, Screen::AppList};

  if (WiFi.getMode() == WIFI_OFF) {
    drawError("WiFi is OFF");
    ThemeColors t = theme();
    gfx->setTextSize(1);
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(20, 170);
    gfx->print("Enable in");
    gfx->setCursor(20, 184);
    gfx->print("Settings > WiFi");
    for (;;) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(100)) == pdPASS) {
        if (e.type == EventType::ButtonShort) break;
        if (e.type == EventType::Touch && tappedBack(e.x, e.y)) break;
      }
      esp_task_wdt_reset();
    }
    switchTo(Screen::AppList);
    return;
  }

  if (!frameBuf || !rxBuf) {
    drawError("Out of memory");
    vTaskDelay(pdMS_TO_TICKS(1500));
    switchTo(Screen::AppList);
    return;
  }

  for (;;) {
    drawWaiting();

    WiFiClient client;
    while (!client) {
      client = server.accept();
      if (client) break;
      if (drainOneExit(req)) break;
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (req.wanted) break;

    Serial.println("STREAM: client connected");
    bool exitWholeApp = streamClient(client, req);
    client.stop();
    Serial.println("STREAM: client disconnected");
    if (exitWholeApp) break;
    // Loop back to waiting screen for a fresh connection.
  }

  switchTo(req.target);
}

#endif  // gated
