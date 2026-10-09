// BluetoothView — BLE scanner. A background task runs back-to-back 3 s active
// scans (bluedroid via the Arduino BLE lib) and publishes a merged device
// table; the view draws a snapshot of it. Apple Continuity type 0x07
// (proximity pairing) adverts carry the headphone model, which is how AirPods
// Max etc. are named even though they advertise without a local name.
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <string.h>
#include <stdio.h>

#include "bluetooth.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "wifi_svc.h"
#include <esp_heap_caps.h>
#include <esp_bt_main.h>

static const int16_t W = 240, H = 280;
static const int16_t LIST_Y = 52, ROW_H = 38;
static const int     ROWS = (H - LIST_Y) / ROW_H;     // 6
static const int     MAX_DEV = 40;
static const uint32_t STALE_MS = 15000;               // drop after 15 s unseen
#define REMOTE_NAME "EWatch Remote"

struct BleDev {
  uint8_t  addr[6];
  int8_t   rssi;
  uint32_t lastSeen;
  char     name[24];
  char     apple[24];      // decoded Apple model ("" if not Apple audio)
  uint16_t appleModel;     // raw 16-bit model id, 0 if none
  uint8_t  mfr[31];
  uint8_t  mfrLen;
};

static SemaphoreHandle_t gMux = nullptr;
static BleDev  *gDev = nullptr;      // PSRAM: internal RAM is for the BT stack
static int      gCount = 0;
static uint32_t gGen = 0;            // bumped when the table changes
static uint32_t gScans = 0;
static volatile bool gRun = false;
static TaskHandle_t gTask = nullptr;
static bool     gInited = false;
static bool     gFailed = false;      // init failed (shown on screen)

// Snapshot the view draws from (stable while a detail page is open).
static BleDev *sDev = nullptr;
static int    sCount = 0;

// Apple proximity-pairing model ids (bytes 1-2 of the type 0x07 payload).
// Community-documented (furiousMAC/continuity, OpenHaystack); unknown ids are
// shown as hex so a new one can be added from the log.
static const char *appleModelName(uint16_t m) {
  switch (m) {
    case 0x0220: return "AirPods";
    case 0x0F20: return "AirPods 2";
    case 0x1320: return "AirPods 3";
    case 0x1920: return "AirPods 4";
    case 0x1B20: return "AirPods 4 ANC";
    case 0x0E20: return "AirPods Pro";
    case 0x1420: return "AirPods Pro 2";
    case 0x2420: return "AirPods Pro 2 USB-C";
    case 0x0A20: return "AirPods Max";
    case 0x1F20: return "AirPods Max USB-C";
    case 0x0320: return "Powerbeats3";
    case 0x0B20: return "Powerbeats Pro";
    case 0x0520: return "BeatsX";
    case 0x0620: return "Beats Solo3";
    case 0x0920: return "Beats Studio3";
    case 0x1020: return "Beats Flex";
    case 0x1120: return "Beats Studio Buds";
    case 0x1220: return "Beats Fit Pro";
    default:     return nullptr;
  }
}

// Walk Apple's TLVs (after the 0x004C company id) for a type 0x07 record.
static uint16_t parseAppleModel(const uint8_t *m, int n) {
  if (n < 4 || m[0] != 0x4C || m[1] != 0x00) return 0;
  int i = 2;
  while (i + 2 <= n) {
    uint8_t type = m[i], len = m[i + 1];
    if (i + 2 + len > n) break;
    if (type == 0x07 && len >= 3) return (uint16_t)(m[i + 3] << 8 | m[i + 4]);
    i += 2 + len;
  }
  return 0;
}

static void fmtAddr(const uint8_t *a, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

// Merge one advert into the table and log first sightings. Runs on our scan
// task, never on bluedroid's BTC task (its small stack overflowed when this
// formatted log lines there).
static void mergeAdvert(BleDev &n) {
  if (n.mfrLen) {
    n.appleModel = parseAppleModel(n.mfr, n.mfrLen);
    if (n.appleModel) {
      const char *nm = appleModelName(n.appleModel);
      if (nm) strlcpy(n.apple, nm, sizeof(n.apple));
      else    snprintf(n.apple, sizeof(n.apple), "Apple audio %04X", n.appleModel);
    }
  }
  bool isNew = false;
  xSemaphoreTake(gMux, portMAX_DELAY);
  int k = 0;
  for (; k < gCount; k++) if (!memcmp(gDev[k].addr, n.addr, 6)) break;
  if (k == gCount) {
    if (gCount < MAX_DEV) gCount++;
    else {  // full: replace the weakest
      k = 0;
      for (int j = 1; j < gCount; j++) if (gDev[j].rssi < gDev[k].rssi) k = j;
    }
    isNew = true;
    gDev[k] = n;
  } else {
    // Scan responses come separately: keep a name/model we already have.
    BleDev &o = gDev[k];
    o.rssi = n.rssi; o.lastSeen = n.lastSeen;
    if (n.name[0]) strlcpy(o.name, n.name, sizeof(o.name));
    if (n.mfrLen) { memcpy(o.mfr, n.mfr, n.mfrLen); o.mfrLen = n.mfrLen; }
    if (n.appleModel) { o.appleModel = n.appleModel; strlcpy(o.apple, n.apple, sizeof(o.apple)); }
  }
  gGen++;
  xSemaphoreGive(gMux);

  if (isNew) {
    char a[18]; fmtAddr(n.addr, a);
    char hex[2 * 31 + 1] = "";
    for (int i = 0; i < n.mfrLen; i++) sprintf(hex + 2 * i, "%02X", n.mfr[i]);
    EWLOGI("BLE", "dev addr=%s rssi=%d name=\"%s\" apple=\"%s\" mfr=%s",
           a, n.rssi, n.name, n.apple, hex);
  }
}

static QueueHandle_t gAdvQ = nullptr;     // BTC task -> scan task
static volatile bool gScanDone = false;

// Runs on bluedroid's BTC task: copy the raw fields and get out.
class ScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    BleDev n = {};
    memcpy(n.addr, *d.getAddress().getNative(), 6);
    n.rssi = (int8_t)d.getRSSI();
    n.lastSeen = millis();
    if (d.haveName()) strlcpy(n.name, d.getName().c_str(), sizeof(n.name));
    if (d.haveManufacturerData()) {
      std::string md = d.getManufacturerData();
      n.mfrLen = (uint8_t)min((size_t)sizeof(n.mfr), md.size());
      memcpy(n.mfr, md.data(), n.mfrLen);
    }
    xQueueSend(gAdvQ, &n, 0);           // drop if the queue is full
  }
};
static ScanCb gCb;

static void onScanDone(BLEScanResults) { gScanDone = true; }

static void scanTask(void *) {
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&gCb, /*wantDuplicates=*/true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  BleDev n;
  while (gRun) {
    gScanDone = false;
    scan->start(3, onScanDone, false);  // non-blocking, 3 s
    uint32_t t0 = millis();
    while (!gScanDone && millis() - t0 < 4000) {
      if (!gRun) { scan->stop(); break; }   // stop() only while a scan is live
      if (xQueueReceive(gAdvQ, &n, pdMS_TO_TICKS(50)) == pdTRUE) mergeAdvert(n);
    }
    while (xQueueReceive(gAdvQ, &n, 0) == pdTRUE) mergeAdvert(n);
    if (gRun && !gScanDone) scan->stop();
    scan->clearResults();
    uint32_t now = millis();
    xSemaphoreTake(gMux, portMAX_DELAY);
    gScans++;
    int w = 0;
    for (int i = 0; i < gCount; i++)
      if (now - gDev[i].lastSeen < STALE_MS) gDev[w++] = gDev[i];
    gCount = w;
    gGen++;
    int apple = 0;
    for (int i = 0; i < gCount; i++) if (gDev[i].appleModel) apple++;
    uint32_t scans = gScans;
    xSemaphoreGive(gMux);
    EWLOGI("BLE", "scan n=%lu devices=%d apple_audio=%d heap=%u",
           (unsigned long)scans, w, apple, (unsigned)ESP.getFreeHeap());
  }
  gTask = nullptr;
  vTaskDelete(nullptr);
}

// --------------------------------------------------------------------------
// Radio lifetime: up while the app is open, fully down when it closes.
// --------------------------------------------------------------------------
static bool bleInit() {
  if (!gMux) gMux = xSemaphoreCreateMutex();
  if (!gDev) gDev = (BleDev *)ps_calloc(MAX_DEV, sizeof(BleDev));
  if (!sDev) sDev = (BleDev *)ps_calloc(MAX_DEV, sizeof(BleDev));
  if (!gAdvQ) gAdvQ = xQueueCreate(24, sizeof(BleDev));
  if (gInited) return true;
  bool wifiWasUp = wifiSvcRadioActive();
  wifiSvcSuspend(true, 3000);          // WiFi driver RAM -> BLE controller
  powerPerfPin(true);
  size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  EWLOGI("BLE", "pre-init wifi_was_up=%d int_free=%u int_largest=%u", wifiWasUp,
         (unsigned)h0, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  BLEDevice::init(REMOTE_NAME);
  // BLEDevice::init flags itself initialised even when a step fails, so ask
  // bluedroid directly; scanning on a half-started stack panics.
  gInited = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
  size_t h1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  gFailed = !gInited;
  if (!gInited) { gGen++; EWLOGE("BLE", "init failed int_free=%u", (unsigned)h1); return false; }
  EWLOGI("BLE", "init ok int_used=%d int_free=%u", (int)(h0 - h1), (unsigned)h1);
  return true;
}

static void scanStart() {
  if (!gInited || gTask) return;
  xSemaphoreTake(gMux, portMAX_DELAY);
  gCount = 0; gScans = 0; gGen++;      // fresh list (and fresh log) per visit
  xSemaphoreGive(gMux);
  gRun = true;
  xTaskCreatePinnedToCore(scanTask, "blescan", 6144, nullptr, 2, &gTask, 0);
}

static void scanStop() {
  if (!gTask) return;
  gRun = false;                        // the task stops its own scan and exits
  for (int i = 0; i < 100 && gTask; i++) delay(50);
}

// --------------------------------------------------------------------------
// Media remote: BLE HID consumer control ("media keys"). The phone/Mac pairs
// with "EWatch Remote" once in its Bluetooth settings and then treats these
// as keyboard media keys, so they drive whatever is playing — including
// audio routed to AirPods. Bonds live in NVS, so re-pairing isn't needed.
// --------------------------------------------------------------------------
enum MediaKey : uint8_t {   // bit in the 1-byte input report (order = report map)
  KEY_PLAYPAUSE = 1 << 0, KEY_NEXT = 1 << 1, KEY_PREV = 1 << 2,
  KEY_VOLUP = 1 << 3, KEY_VOLDOWN = 1 << 4, KEY_MUTE = 1 << 5,
};

static const uint8_t kReportMap[] = {
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID 1
  0x15, 0x00,        //   Logical Min 0
  0x25, 0x01,        //   Logical Max 1
  0x75, 0x01,        //   Report Size 1
  0x95, 0x06,        //   Report Count 6
  0x09, 0xCD,        //   Usage (Play/Pause)
  0x09, 0xB5,        //   Usage (Scan Next Track)
  0x09, 0xB6,        //   Usage (Scan Previous Track)
  0x09, 0xE9,        //   Usage (Volume Increment)
  0x09, 0xEA,        //   Usage (Volume Decrement)
  0x09, 0xE2,        //   Usage (Mute)
  0x81, 0x02,        //   Input (Data, Var, Abs)
  0x95, 0x02,        //   Report Count 2
  0x81, 0x01,        //   Input (Const) — pad to a byte
  0xC0,              // End Collection
};

static BLEServer         *gServer = nullptr;
static BLEHIDDevice      *gHid = nullptr;
static BLECharacteristic *gInput = nullptr;
static volatile bool gConnected = false;
static volatile bool gConnChanged = false;
static bool gRemoteUp = false;

// Callbacks run on the BTC task (small stack): flags only, no logging.
class RemoteServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override    { gConnected = true;  gConnChanged = true; }
  void onDisconnect(BLEServer *) override { gConnected = false; gConnChanged = true; }
};
static RemoteServerCb gServerCb;

static void remoteAdvertise() {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x0180);               // generic remote control
  adv->addServiceUUID(gHid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->start();
}

static void remoteStart() {
  if (!gInited || gRemoteUp) return;
  BLESecurity *sec = new BLESecurity();     // bonded "just works" pairing
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  gServer = BLEDevice::createServer();
  gServer->setCallbacks(&gServerCb);
  gHid = new BLEHIDDevice(gServer);
  gInput = gHid->inputReport(1);
  gHid->manufacturer()->setValue("EWatch");
  gHid->pnp(0x02, 0x05AC, 0x820A, 0x0210);  // USB-IF vendor source, generic ids
  gHid->hidInfo(0x00, 0x01);
  gHid->reportMap((uint8_t *)kReportMap, sizeof(kReportMap));
  gHid->startServices();
  gHid->setBatteryLevel(100);
  remoteAdvertise();
  gRemoteUp = true;
  EWLOGI("BLE", "remote advertising name=\"%s\" int_free=%u", REMOTE_NAME,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void remotePause() { if (gRemoteUp && !gConnected) BLEDevice::getAdvertising()->stop(); }
static void remoteResume() { if (gRemoteUp && !gConnected) BLEDevice::getAdvertising()->start(); }

// Press + release, like a key tap. Called from the view (render task).
static bool remoteSend(uint8_t key, const char *name) {
  if (!gConnected || !gInput) { EWLOGW("BLE", "key %s dropped: not connected", name); return false; }
  uint8_t down = key, up = 0;
  gInput->setValue(&down, 1); gInput->notify();
  vTaskDelay(pdMS_TO_TICKS(20));
  gInput->setValue(&up, 1);   gInput->notify();
  EWLOGI("BLE", "key %s sent", name);
  return true;
}

// Full shutdown on leaving: the radio stays off outside the app and the CPU
// governor gets the clock back. The HID objects die with the stack.
static void bleStop() {
  if (!gInited) { powerPerfPin(false); wifiSvcSuspend(false, 0); return; }
  scanStop();
  if (gRemoteUp) BLEDevice::getAdvertising()->stop();
  BLEDevice::deinit(false);            // keep memory so a re-entry can init
  gInited = false;
  gRemoteUp = false; gConnected = false;
  gServer = nullptr; gHid = nullptr; gInput = nullptr;   // owned by the old stack
  powerPerfPin(false);
  wifiSvcSuspend(false, 0);
  EWLOGI("BLE", "deinit heap=%u", (unsigned)ESP.getFreeHeap());
}

// --------------------------------------------------------------------------

static void snapshot() {
  xSemaphoreTake(gMux, portMAX_DELAY);
  sCount = gCount;
  memcpy(sDev, gDev, sizeof(BleDev) * gCount);
  xSemaphoreGive(gMux);
  // Apple audio first, then strongest signal.
  for (int i = 1; i < sCount; i++) {
    BleDev t = sDev[i]; int j = i - 1;
    auto key = [](const BleDev &d) { return (d.appleModel ? 1000 : 0) + d.rssi; };
    while (j >= 0 && key(sDev[j]) < key(t)) { sDev[j + 1] = sDev[j]; j--; }
    sDev[j + 1] = t;
  }
}

// Remote layout: transport row + volume row.
struct Btn { int16_t x, y, w, h; uint8_t key; const char *name; };
static const Btn kBtns[] = {
  {  10, 112, 66, 66, KEY_PREV,      "prev"      },
  {  87, 104, 66, 82, KEY_PLAYPAUSE, "playpause" },
  { 164, 112, 66, 66, KEY_NEXT,      "next"      },
  {  10, 200, 105, 62, KEY_VOLDOWN,  "voldown"   },
  { 125, 200, 105, 62, KEY_VOLUP,    "volup"     },
};
static const int16_t SCAN_X = 180, SCAN_Y = 6, SCAN_W = 56, SCAN_H = 32;

static bool inBox(const Event &e, int16_t x, int16_t y, int16_t w, int16_t h) {
  return (int16_t)e.x >= x && (int16_t)e.x < x + w &&
         (int16_t)e.y >= y && (int16_t)e.y < y + h;
}

static void drawGlyph(uint8_t key, int16_t cx, int16_t cy, uint16_t c) {
  switch (key) {
    case KEY_PLAYPAUSE:
      gfx->fillTriangle(cx - 16, cy - 13, cx - 16, cy + 13, cx + 2, cy, c);
      gfx->fillRect(cx + 6, cy - 12, 4, 24, c);
      gfx->fillRect(cx + 13, cy - 12, 4, 24, c);
      break;
    case KEY_NEXT:
      gfx->fillTriangle(cx - 14, cy - 11, cx - 14, cy + 11, cx + 2, cy, c);
      gfx->fillTriangle(cx - 2, cy - 11, cx - 2, cy + 11, cx + 14, cy, c);
      break;
    case KEY_PREV:
      gfx->fillTriangle(cx + 14, cy - 11, cx + 14, cy + 11, cx - 2, cy, c);
      gfx->fillTriangle(cx + 2, cy - 11, cx + 2, cy + 11, cx - 14, cy, c);
      break;
    case KEY_VOLUP:
    case KEY_VOLDOWN:
      gfx->fillRect(cx - 22, cy - 6, 8, 12, c);                         // speaker
      gfx->fillTriangle(cx - 14, cy - 6, cx - 4, cy - 14, cx - 4, cy + 14, c);
      gfx->fillTriangle(cx - 14, cy - 6, cx - 14, cy + 6, cx - 4, cy + 14, c);
      gfx->fillRect(cx + 4, cy - 2, 16, 4, c);                          // minus
      if (key == KEY_VOLUP) gfx->fillRect(cx + 10, cy - 8, 4, 16, c);   // plus
      break;
  }
}

static void drawRemoteButton(const Btn &b, bool pressed) {
  ThemeColors t = theme();
  uint16_t fill = pressed ? t.accent : t.line;
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 14, fill);
  drawGlyph(b.key, b.x + b.w / 2, b.y + b.h / 2, contrastFor(fill));
}

void BluetoothView::drawRemote() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Remote");
  gfx->fillRoundRect(SCAN_X, SCAN_Y, SCAN_W, SCAN_H, 8, t.line);
  gfx->setTextSize(2);
  gfx->setTextColor(contrastFor(t.line), t.line);
  gfx->setCursor(SCAN_X + 4, SCAN_Y + 9);
  gfx->print("Scan");
  for (const Btn &b : kBtns) drawRemoteButton(b, false);
  drawRemoteStatus();
}

void BluetoothView::drawRemoteStatus() {
  ThemeColors t = theme();
  gfx->fillRect(0, 54, W, 44, t.bg);
  gfx->setTextSize(1);
  if (gFailed) {
    gfx->setTextColor(RED, t.bg);
    gfx->setCursor(36, 70); gfx->print("Bluetooth failed to start");
    return;
  }
  if (gConnected) {
    gfx->fillCircle(16, 66, 5, GREEN);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(28, 62); gfx->print("Connected");
  } else {
    gfx->fillCircle(16, 66, 5, ORANGE);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(28, 58); gfx->print("Pair \"" REMOTE_NAME "\" in your");
    gfx->setCursor(28, 72); gfx->print("phone's Bluetooth settings");
  }
}

void BluetoothView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true; shownGen = 0xFFFFFFFF; scroll = 0; detail = -1;
  mode = Mode::Remote; shownConn = -1;
  if (bleInit()) remoteStart();
}

void BluetoothView::onExit() { bleStop(); }

void BluetoothView::setMode(Mode m) {
  if (m == mode) return;
  mode = m; firstDraw = true; shownGen = 0xFFFFFFFF; scroll = 0; detail = -1; shownConn = -1;
  if (m == Mode::Scan) { remotePause(); scanStart(); }
  else                 { scanStop(); remoteResume(); }
  EWLOGI("BLE", "mode=%s", m == Mode::Scan ? "scan" : "remote");
}

void BluetoothView::render() {
  if (!gfx) return;
  if (gConnChanged) {                  // from the BTC task: log + re-advertise here
    gConnChanged = false;
    EWLOGI("BLE", "remote %s", gConnected ? "connected" : "disconnected");
    if (!gConnected && gRemoteUp && mode == Mode::Remote) BLEDevice::getAdvertising()->start();
  }
  if (mode == Mode::Remote) {
    if (firstDraw) { drawRemote(); firstDraw = false; shownConn = gConnected; }
    if (shownConn != (int)gConnected) { shownConn = gConnected; drawRemoteStatus(); }
    return;
  }
  if (firstDraw) {
    ThemeColors t = theme();
    gfx->fillScreen(t.bg);
    drawTitleBar("Bluetooth");
    firstDraw = false;
  }
  if (detail >= 0) return;            // detail page is static
  uint32_t g;
  xSemaphoreTake(gMux, portMAX_DELAY); g = gGen; xSemaphoreGive(gMux);
  if (g == shownGen) return;
  shownGen = g;
  snapshot();
  drawList();
}

void BluetoothView::drawList() {
  ThemeColors t = theme();
  gfx->fillRect(0, LIST_Y, W, H - LIST_Y, t.bg);
  gfx->setTextSize(1);
  if (sCount == 0) {
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(gFailed ? 36 : 70, 150);
    gfx->print(gFailed ? "Bluetooth failed to start" : "Scanning...");
    return;
  }
  if (scroll > sCount - 1) scroll = max(0, sCount - ROWS);
  for (int r = 0; r < ROWS && scroll + r < sCount; r++) {
    const BleDev &d = sDev[scroll + r];
    int16_t y = LIST_Y + r * ROW_H;
    bool apple = d.appleModel != 0;
    if (apple) gfx->fillRoundRect(4, y + 1, W - 8, ROW_H - 2, 6, t.line);
    uint16_t bg = apple ? t.line : t.bg;
    uint16_t fg = apple ? contrastFor(t.line) : t.fg;
    // No name in the advert: say whose it is if the company id tells us.
    bool isApple = d.mfrLen >= 2 && d.mfr[0] == 0x4C && d.mfr[1] == 0x00;
    const char *title = d.apple[0] ? d.apple
                      : d.name[0]  ? d.name
                      : isApple    ? "Apple device" : "Unknown";
    gfx->setTextSize(2);
    gfx->setTextColor(fg, bg);
    gfx->setCursor(10, y + 5);
    char tt[17]; strlcpy(tt, title, sizeof(tt));   // 16 chars @ 12 px, clear of the bars
    gfx->print(tt);
    gfx->setTextSize(1);
    gfx->setTextColor(apple ? fg : t.accent, bg);
    gfx->setCursor(10, y + 25);
    char sub[40], a[18]; fmtAddr(d.addr, a);
    snprintf(sub, sizeof(sub), "%s  %d dBm", a, d.rssi);
    gfx->print(sub);
    // Signal bars.
    int bars = d.rssi > -60 ? 4 : d.rssi > -70 ? 3 : d.rssi > -80 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      int16_t bh = 4 + b * 4, bx = W - 34 + b * 6;
      gfx->fillRect(bx, y + 26 - bh + 4, 4, bh, b < bars ? fg : t.bg == bg ? t.line : t.bg);
    }
  }
  char foot[24];
  snprintf(foot, sizeof(foot), "%d found", sCount);
  gfx->setTextSize(1);
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(150, 32);
  gfx->print(foot);
}

void BluetoothView::drawDetail() {
  ThemeColors t = theme();
  gfx->fillRect(0, LIST_Y, W, H - LIST_Y, t.bg);
  const BleDev &d = sDev[detail];
  char a[18]; fmtAddr(d.addr, a);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setTextSize(2);
  gfx->setCursor(8, 60);
  gfx->print(d.apple[0] ? d.apple : (d.name[0] ? d.name : "(no name)"));
  gfx->setTextSize(1);
  int16_t y = 88;
  auto row = [&](const char *k, const char *v) {
    gfx->setTextColor(t.accent, t.bg); gfx->setCursor(8, y); gfx->print(k);
    gfx->setTextColor(t.fg, t.bg);     gfx->setCursor(56, y); gfx->print(v);
    y += 14;
  };
  char buf[40];
  row("addr", a);
  snprintf(buf, sizeof(buf), "%d dBm", d.rssi); row("rssi", buf);
  row("name", d.name[0] ? d.name : "-");
  if (d.appleModel) { snprintf(buf, sizeof(buf), "0x%04X", d.appleModel); row("model", buf); }
  y += 6;
  gfx->setTextColor(t.accent, t.bg); gfx->setCursor(8, y); gfx->print("manufacturer data"); y += 14;
  gfx->setTextColor(t.fg, t.bg);
  char hex[3];
  for (int i = 0; i < d.mfrLen; i++) {
    if (i % 12 == 0) { gfx->setCursor(8, y); y += 12; }
    sprintf(hex, "%02X", d.mfr[i]);
    gfx->print(hex); gfx->print(' ');
  }
  if (d.appleModel) {
    y += 10;
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(8, y);     gfx->print("Audio needs Bluetooth Classic");
    gfx->setCursor(8, y + 12); gfx->print("(A2DP). This chip is BLE only.");
  }
}

// Back from the scanner (or its detail page) goes to the remote; back from
// the remote leaves the app.
void BluetoothView::goBack() {
  if (mode == Mode::Scan && detail >= 0) { detail = -1; firstDraw = true; shownGen = 0xFFFFFFFF; return; }
  if (mode == Mode::Scan) { setMode(Mode::Remote); return; }
  switchTo(Screen::AppList);
}

void BluetoothView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { goBack(); return; }
  if (e.type == EventType::Gesture) {
    Gesture g = e.gesture;
    if (g == Gesture::SwipeRight) { goBack(); return; }
    if (mode == Mode::Scan && detail < 0) {
      if (g == Gesture::SwipeUp && scroll + ROWS < sCount) { scroll += ROWS - 1; drawList(); }
      if (g == Gesture::SwipeDown && scroll > 0) { scroll = max(0, scroll - (ROWS - 1)); drawList(); }
    }
    return;
  }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { goBack(); return; }

  if (mode == Mode::Remote) {
    if (inBox(e, SCAN_X, SCAN_Y, SCAN_W, SCAN_H)) { setMode(Mode::Scan); return; }
    for (const Btn &b : kBtns) {
      if (!inBox(e, b.x, b.y, b.w, b.h)) continue;
      drawRemoteButton(b, true);
      bool ok = remoteSend(b.key, b.name);
      hapticBuzz(ok ? 70 : 120, ok ? 30 : 80);   // long buzz = not connected
      drawRemoteButton(b, false);
      return;
    }
    return;
  }

  if (detail < 0 && e.y >= LIST_Y) {
    int i = scroll + (e.y - LIST_Y) / ROW_H;
    if (i < sCount) {
      detail = i;
      hapticBuzz(60, 40);
      char a[18]; fmtAddr(sDev[i].addr, a);
      EWLOGI("BLE", "detail addr=%s", a);
      drawDetail();
    }
  }
}
