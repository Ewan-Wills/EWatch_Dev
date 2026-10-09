// BleControlsView — see blecontrols.h for the wire protocol.
// BLE callbacks run on the BTC task (small stack): they only copy raw bytes
// into a queue and set flags. Parsing, logging, drawing and notifies all
// happen on the render task in render().
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "blecontrols.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "wifi_svc.h"
#include <esp_heap_caps.h>
#include <esp_bt_main.h>

static const int16_t W = 240, H = 280;
static const int16_t LIST_Y = 84, ROW_H = 32;
static const int     ROWS = (H - LIST_Y) / ROW_H;     // 6
static const int     MAX_VARS = 16;
#define DEV_NAME "EWatch Controls"

// Shared with ewatch.cloud (ewatch-frontend/src/foundation/ble.jsx).
#define SVC_UUID "7e0a0001-5c3b-4c8e-9a6f-2d1e0c0b0a01"
#define CMD_UUID "7e0a0002-5c3b-4c8e-9a6f-2d1e0c0b0a01"
#define EVT_UUID "7e0a0003-5c3b-4c8e-9a6f-2d1e0c0b0a01"

struct RxMsg { uint16_t len; char s[182]; };
struct Var   { char id[20]; char val[40]; uint32_t at; };

static QueueHandle_t      gRxQ = nullptr;
static BLEServer         *gServer = nullptr;
static BLECharacteristic *gCmd = nullptr;
static BLECharacteristic *gEvt = nullptr;
static bool gInited = false, gFailed = false, gUp = false;
static volatile bool gConnected = false, gConnChanged = false;
static volatile uint32_t gDropped = 0;

static Var     *gVars = nullptr;       // PSRAM
static int      gNVars = 0;
static int      gLast = -1;            // index of the most recently updated var
static uint32_t gGen = 0;              // bumps on every table change
static uint32_t gRxCount = 0;
static char     gMsg[64];

// ---------------------------------------------------------------------------
// BTC-task callbacks: flags and a queue copy only (no logging, no Strings).
// ---------------------------------------------------------------------------
class CtlServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override    { gConnected = true;  gConnChanged = true; }
  void onDisconnect(BLEServer *) override { gConnected = false; gConnChanged = true; }
};
class CtlCmdCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    static RxMsg m;                    // BTC task is the only writer
    size_t n = c->getLength();
    if (n > sizeof(m.s) - 1) n = sizeof(m.s) - 1;
    memcpy(m.s, c->getData(), n);
    m.s[n] = 0; m.len = (uint16_t)n;
    if (xQueueSend(gRxQ, &m, 0) != pdTRUE) gDropped++;
  }
};
static CtlServerCb gServerCb;
static CtlCmdCb    gCmdCb;

// ---------------------------------------------------------------------------
// Radio lifetime: up while the screen is open, fully down when it closes
// (same pattern as the Bluetooth app: WiFi suspended, CPU pinned).
// ---------------------------------------------------------------------------
static bool bleUp() {
  if (!gRxQ) gRxQ = xQueueCreate(16, sizeof(RxMsg));
  if (!gVars) gVars = (Var *)ps_calloc(MAX_VARS, sizeof(Var));
  if (gUp) return true;
  wifiSvcSuspend(true, 3000);          // WiFi driver RAM -> BLE controller
  powerPerfPin(true);
  size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  BLEDevice::init(DEV_NAME);
  // BLEDevice::init flags itself initialised even when a step fails.
  gInited = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
  gFailed = !gInited;
  if (!gInited) {
    EWLOGE("BLECTL", "init failed int_free=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return false;
  }
  BLEDevice::setMTU(185);              // web side writes up to ~180 bytes
  gServer = BLEDevice::createServer();
  gServer->setCallbacks(&gServerCb);
  BLEService *svc = gServer->createService(SVC_UUID);
  gCmd = svc->createCharacteristic(CMD_UUID, BLECharacteristic::PROPERTY_WRITE |
                                             BLECharacteristic::PROPERTY_WRITE_NR);
  gCmd->setCallbacks(&gCmdCb);
  gEvt = svc->createCharacteristic(EVT_UUID, BLECharacteristic::PROPERTY_READ |
                                             BLECharacteristic::PROPERTY_NOTIFY);
  gEvt->addDescriptor(new BLE2902());
  gEvt->setValue("idle");
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);          // name goes in the scan response
  adv->start();
  gUp = true;
  size_t h1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  EWLOGI("BLECTL", "advertising name=\"%s\" int_used=%d int_free=%u", DEV_NAME,
         (int)(h0 - h1), (unsigned)h1);
  return true;
}

static void bleDown() {
  if (gUp) {
    BLEDevice::getAdvertising()->stop();
    BLEDevice::deinit(false);          // keep memory so a re-entry can init
  }
  gUp = false; gInited = false; gConnected = false;
  gServer = nullptr; gCmd = nullptr; gEvt = nullptr;   // owned by the old stack
  powerPerfPin(false);
  wifiSvcSuspend(false, 0);
  EWLOGI("BLECTL", "deinit heap=%u", (unsigned)ESP.getFreeHeap());
}

static void notify(const char *s) {
  if (!gConnected || !gEvt) return;
  gEvt->setValue((uint8_t *)s, strlen(s));
  gEvt->notify();
}

// ---------------------------------------------------------------------------
// Commands (render task)
// ---------------------------------------------------------------------------
static int findOrAddVar(const char *id) {
  for (int i = 0; i < gNVars; i++) if (strcmp(gVars[i].id, id) == 0) return i;
  if (gNVars < MAX_VARS) {
    strlcpy(gVars[gNVars].id, id, sizeof(gVars[0].id));
    return gNVars++;
  }
  // Table full: recycle the oldest entry.
  int old = 0;
  for (int i = 1; i < gNVars; i++) if (gVars[i].at < gVars[old].at) old = i;
  strlcpy(gVars[old].id, id, sizeof(gVars[0].id));
  return old;
}

static void handleCmd(char *line, uint32_t &msgUntil) {
  while (*line == ' ') line++;
  if (!*line) return;
  char *eq = strchr(line, '=');
  const char *id = line, *val = "";
  if (eq) { *eq = 0; val = eq + 1; }
  if (!*id) return;
  gRxCount++;
  EWLOGI("BLECTL", "cmd id=%s val=%s", id, val);

  if (strcmp(id, "clear") == 0) {
    gNVars = 0; gLast = -1;
  } else {
    int i = findOrAddVar(id);
    strlcpy(gVars[i].val, val, sizeof(gVars[0].val));
    gVars[i].at = millis();
    gLast = i;
  }
  if (strcmp(id, "buzz") == 0) {
    int ms = atoi(val); if (ms <= 0) ms = 80; if (ms > 1000) ms = 1000;
    hapticBuzz(200, (uint16_t)ms);
  } else if (strcmp(id, "bl") == 0) {
    int v = atoi(val); if (v < 5) v = 5; if (v > 255) v = 255;  // never fully dark
    backlightSet((uint8_t)v);
  } else if (strcmp(id, "msg") == 0) {
    strlcpy(gMsg, val, sizeof(gMsg));
    msgUntil = millis() + 3000;
  }
  gGen++;
  char ack[40];
  snprintf(ack, sizeof(ack), "ack %s", id);
  notify(ack);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void BleControlsView::drawStatus() {
  ThemeColors t = theme();
  gfx->fillRect(0, 50, W, 30, t.bg);
  gfx->setTextSize(1);
  if (gFailed) {
    gfx->setTextColor(RED, t.bg);
    gfx->setCursor(16, 60); gfx->print("Bluetooth failed to start");
    return;
  }
  gfx->fillCircle(16, 64, 5, gConnected ? GREEN : ORANGE);
  gfx->setTextColor(t.fg, t.bg);
  if (gConnected) {
    gfx->setCursor(28, 54); gfx->print("Connected to ewatch.cloud");
    char b[40]; snprintf(b, sizeof(b), "%lu commands", (unsigned long)gRxCount);
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(28, 68); gfx->print(b);
  } else {
    gfx->setCursor(28, 54); gfx->print("Open Controls on ewatch.cloud");
    gfx->setCursor(28, 68); gfx->print("and connect \"" DEV_NAME "\"");
  }
}

void BleControlsView::drawRows() {
  ThemeColors t = theme();
  gfx->fillRect(0, LIST_Y, W, H - LIST_Y, t.bg);
  gfx->drawFastHLine(20, LIST_Y - 2, 200, t.line);
  if (gNVars == 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(44, LIST_Y + 40); gfx->print("No commands received yet");
    return;
  }
  if (scroll > gNVars - 1) scroll = max(0, gNVars - ROWS);
  for (int r = 0; r < ROWS && scroll + r < gNVars; r++) {
    int i = scroll + r;
    const Var &v = gVars[i];
    int16_t y = LIST_Y + r * ROW_H;
    bool hot = (i == gLast);
    uint16_t bg = hot ? t.accent : t.bg;
    if (hot) gfx->fillRoundRect(6, y + 2, W - 12, ROW_H - 4, 8, bg);
    uint16_t fg = hot ? contrastFor(bg) : t.fg;
    gfx->setTextSize(1);
    gfx->setTextColor(hot ? fg : t.accent, bg);
    gfx->setCursor(14, y + 6); gfx->print(v.id);
    gfx->setTextSize(2);
    gfx->setTextColor(fg, bg);
    char shown[18]; strlcpy(shown, v.val, sizeof(shown));   // 2x font: ~17 cols
    gfx->setCursor(14, y + 15); gfx->print(shown);
  }
}

void BleControlsView::drawMsg() {
  ThemeColors t = theme();
  gfx->fillScreen(t.accent);
  uint16_t fg = contrastFor(t.accent);
  gfx->setTextColor(fg, t.accent);
  gfx->setTextSize(3);
  // Word-wrap at 12 columns (3x font), up to 5 lines, centred vertically.
  char lines[5][13]; int n = 0;
  const char *p = gMsg;
  while (*p && n < 5) {
    int len = strlen(p), take = len > 12 ? 12 : len;
    if (len > 12) { int sp = take; while (sp > 0 && p[sp] != ' ') sp--; if (sp > 0) take = sp; }
    memcpy(lines[n], p, take); lines[n][take] = 0; n++;
    p += take; while (*p == ' ') p++;
  }
  int16_t y = (H - n * 30) / 2;
  for (int i = 0; i < n; i++) {
    int16_t x = (W - (int16_t)strlen(lines[i]) * 18) / 2;
    gfx->setCursor(x, y + i * 30); gfx->print(lines[i]);
  }
}

void BleControlsView::drawAll() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Controls");
  drawStatus();
  drawRows();
}

// ---------------------------------------------------------------------------

void BleControlsView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true; shownConn = -1; shownGen = 0xFFFFFFFF; msgUntil = 0; scroll = 0;
  bleUp();
}

void BleControlsView::onExit() { bleDown(); }

void BleControlsView::render() {
  if (!gfx) return;
  if (gConnChanged) {                  // from the BTC task: log + re-advertise here
    gConnChanged = false;
    EWLOGI("BLECTL", "%s", gConnected ? "connected" : "disconnected");
    if (gConnected) notify("hello app=blecontrols v=1");
    else if (gUp) BLEDevice::getAdvertising()->start();
  }
  RxMsg m;
  while (gRxQ && xQueueReceive(gRxQ, &m, 0) == pdTRUE) {
    char *save = nullptr;
    for (char *ln = strtok_r(m.s, "\n", &save); ln; ln = strtok_r(nullptr, "\n", &save))
      handleCmd(ln, msgUntil);
  }
  if (gDropped) { EWLOGW("BLECTL", "dropped=%lu (queue full)", (unsigned long)gDropped); gDropped = 0; }

  bool msgUp = msgUntil && (int32_t)(msgUntil - millis()) > 0;
  if (msgUp) {
    if (shownGen != gGen) { drawMsg(); shownGen = gGen; }
    return;
  }
  if (msgUntil) { msgUntil = 0; firstDraw = true; }   // overlay just expired
  if (firstDraw) {
    drawAll(); firstDraw = false; shownConn = gConnected; shownGen = gGen;
    return;
  }
  if (shownConn != (int)gConnected) { shownConn = gConnected; drawStatus(); }
  if (shownGen != gGen) { shownGen = gGen; drawStatus(); drawRows(); }
}

void BleControlsView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    // Side button: tell the web page first (a panel can react to it), then leave.
    notify("evt side=1");
    switchTo(Screen::AppList);
    return;
  }
  if (e.type == EventType::Gesture) {
    Gesture g = e.gesture;
    if (g == Gesture::SwipeRight) { switchTo(Screen::AppList); return; }
    if (g == Gesture::SwipeUp && scroll + ROWS < gNVars) { scroll += ROWS - 1; drawRows(); }
    if (g == Gesture::SwipeDown && scroll > 0) { scroll = max(0, scroll - (ROWS - 1)); drawRows(); }
    return;
  }
  if (e.type != EventType::Touch) return;
  if (msgUntil) { msgUntil = 0; firstDraw = true; return; }   // tap dismisses msg
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
  if (e.y >= LIST_Y) {
    int i = scroll + (e.y - LIST_Y) / ROW_H;
    if (i < gNVars) {
      char b[48]; snprintf(b, sizeof(b), "evt tap=%s", gVars[i].id);
      notify(b);
      hapticBuzz(60, 30);
      EWLOGI("BLECTL", "tap row id=%s", gVars[i].id);
    }
  }
}
