// Bluetooth LE mouse — see btmouse.h.
//
// Threads:
//   BTC task (bluedroid)  notification/scan/security callbacks: copy + flags only
//   "mouse" task          connect/reconnect, report decoding, cursor maths,
//                         virtual-finger state machine (20 ms tick)
//   I/O task              polls mouseTouch()/mouseTakeGesture()/mouseButton()
//   render task           mouseCursorPaint(), mouseTakeGoto(), MouseView
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "btmouse.h"
#include "ble_stack.h"
#include "display.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "ewlog.h"
#include "ewsim.h"

static const int16_t W = 240, H = 280;

// ===========================================================================
// HID report descriptor (Report Map) parser: find the mouse report's fields.
// ===========================================================================
struct Field { bool ok; uint8_t rid; uint16_t bit; uint8_t size; bool sgn; };
struct MouseMap {
  bool  ok;
  uint8_t rid;
  Field x, y, wheel, pan;
  Field btn[5];
};

static uint32_t getBits(const uint8_t *d, int len, const Field &f) {
  uint32_t v = 0;
  for (int i = 0; i < f.size && i < 32; i++) {
    int b = f.bit + i;
    if ((b >> 3) >= len) break;
    if (d[b >> 3] & (1 << (b & 7))) v |= (1u << i);
  }
  return v;
}
static int32_t getSigned(const uint8_t *d, int len, const Field &f) {
  if (!f.ok) return 0;
  uint32_t v = getBits(d, len, f);
  if (f.sgn && f.size < 32 && (v & (1u << (f.size - 1)))) v |= ~((1u << f.size) - 1);
  return (int32_t)v;
}

static bool parseReportMap(const uint8_t *d, int n, MouseMap &m) {
  memset(&m, 0, sizeof(m));
  static uint16_t off[256];
  memset(off, 0, sizeof(off));
  uint32_t page = 0, rsize = 0, rcount = 0; int32_t lmin = 0; uint8_t rid = 0;
  uint32_t usages[16]; int nU = 0; uint32_t umin = 0, umax = 0; bool range = false;
  int i = 0;
  while (i < n) {
    uint8_t b = d[i];
    if (b == 0xFE) { if (i + 1 >= n) break; i += 3 + d[i + 1]; continue; }   // long item
    int sz = b & 3; if (sz == 3) sz = 4;
    if (i + sz >= n + 0 && i + 1 + sz > n) break;
    uint32_t v = 0;
    for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
    int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
    switch (b & 0xFC) {
      case 0x04: page = v; break;                         // Usage Page
      case 0x14: lmin = sv; break;                        // Logical Minimum
      case 0x74: rsize = v; break;                        // Report Size
      case 0x84: rid = (uint8_t)v; break;                 // Report ID
      case 0x94: rcount = v; break;                       // Report Count
      case 0x08: if (nU < 16) usages[nU++] = sz == 4 ? v : (page << 16) | v; break;
      case 0x18: umin = sz == 4 ? v : (page << 16) | v; range = true; break;
      case 0x28: umax = sz == 4 ? v : (page << 16) | v; break;
      case 0x80: {                                        // Input
        bool constant = v & 1;
        for (uint32_t k = 0; k < rcount; k++) {
          uint32_t u = 0;
          if (nU) u = usages[k < (uint32_t)nU ? k : nU - 1];
          else if (range && umin + k <= umax) u = umin + k;
          if (!constant && u) {
            Field f = { true, rid, (uint16_t)(off[rid] + k * rsize), (uint8_t)rsize, lmin < 0 };
            uint16_t up = u >> 16, us = u & 0xFFFF;
            if (up == 0x01 && us == 0x30 && !m.x.ok) m.x = f;
            else if (up == 0x01 && us == 0x31 && !m.y.ok) m.y = f;
            else if (up == 0x01 && us == 0x38 && !m.wheel.ok) m.wheel = f;
            else if (up == 0x0C && us == 0x238 && !m.pan.ok) m.pan = f;
            else if (up == 0x09 && us >= 1 && us <= 5 && !m.btn[us - 1].ok) m.btn[us - 1] = f;
          }
        }
        off[rid] += rsize * rcount;
        nU = 0; range = false;
        break;
      }
      case 0x90: case 0xB0: case 0xA0: case 0xC0:         // Output/Feature/Collection
        nU = 0; range = false; break;
      default: break;
    }
    i += 1 + sz;
  }
  m.ok = m.x.ok && m.y.ok;
  m.rid = m.x.rid;
  return m.ok;
}

// Boot-mouse-like fallback: [buttons, x, y, wheel], signed 8-bit.
static void bootMap(MouseMap &m, uint8_t rid) {
  memset(&m, 0, sizeof(m));
  m.ok = true; m.rid = rid;
  for (int b = 0; b < 3; b++) m.btn[b] = { true, rid, (uint16_t)b, 1, false };
  m.x = { true, rid, 8, 8, true };
  m.y = { true, rid, 16, 8, true };
  m.wheel = { true, rid, 24, 8, true };
}

// ===========================================================================
// Shared state
// ===========================================================================
enum class MSt : uint8_t { Off, NoDevice, Scanning, Connecting, Connected, Failed };

struct Found { char addr[18]; uint8_t type; char name[22]; int8_t rssi; };

static volatile MSt  gSt = MSt::Off;
static volatile uint32_t gGen = 0;
static volatile bool gEnabled = false;
static char     gSavedAddr[18] = "";
static uint8_t  gSavedType = 0;
static char     gName[32] = "";
static uint8_t  gSpeed = 3;                 // 1..5
static char     gErr[40] = "";
static Found    gFound[6];
static volatile int gFoundN = 0;
static volatile int gReqPair = -1;          // index into gFound
static volatile bool gReqScan = false, gReqForget = false;
static TaskHandle_t gTask = nullptr;
static bool gStackHeld = false;

// Reports from the BTC task.
struct Rpt { uint8_t rid; uint8_t len; uint8_t d[14]; };
static QueueHandle_t gRptQ = nullptr;
static uint16_t gHandleRid[8][2];           // {handle, reportId}
static int      gHandleN = 0;
static volatile bool gDisconnected = false;
static volatile bool gAuthDone = false, gAuthOk = false;

// Cursor + virtual finger (read by the I/O and render tasks).
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static float    gCx = W / 2, gCy = H / 2;
static volatile uint32_t gLastMoveMs = 0;
static volatile bool gVfDown = false;       // virtual finger
static volatile uint16_t gVfX = 0, gVfY = 0;
static volatile bool gVBtn = false;         // virtual side button
static volatile uint32_t gVBtnUntil = 0;
static Gesture  gGestQ[4]; static uint16_t gGestX[4], gGestY[4];
static volatile int gGestN = 0;
static volatile int gGoto = -1;
static volatile bool gWake = false, gActivity = false;
static volatile bool gLeftHeld = false;
static volatile uint32_t gSimUntil = 0;     // `msim` test reports: act as if connected
static MouseMap gSimMap;
static inline bool simActive() { return gSimUntil && (int32_t)(gSimUntil - millis()) > 0; }
// Cursor painter state (render task; read by the msim test hook).
static bool     cDrawn = false;
static int      cX = 0, cY = 0;
static bool     cPressed = false;
static uint32_t cPx = 0;
static volatile uint32_t cPaints = 0;       // cursor composites pushed (test visibility)

static void pushGesture(Gesture g, uint16_t x, uint16_t y) {
  portENTER_CRITICAL(&gMux);
  if (gGestN < 4) { gGestQ[gGestN] = g; gGestX[gGestN] = x; gGestY[gGestN] = y; gGestN++; }
  portEXIT_CRITICAL(&gMux);
}

// ---- hooks ----
void mouseTouch(bool &live, uint16_t &x, uint16_t &y) {
  if (!gVfDown) return;
  live = true; x = gVfX; y = gVfY;
}
Gesture mouseTakeGesture(uint16_t &x, uint16_t &y) {
  Gesture g = Gesture::None;
  portENTER_CRITICAL(&gMux);
  if (gGestN) {
    g = gGestQ[0]; x = gGestX[0]; y = gGestY[0];
    for (int i = 1; i < gGestN; i++) { gGestQ[i - 1] = gGestQ[i]; gGestX[i - 1] = gGestX[i]; gGestY[i - 1] = gGestY[i]; }
    gGestN--;
  }
  portEXIT_CRITICAL(&gMux);
  return g;
}
bool mouseButton()       { return gVBtn; }
int  mouseTakeGoto()     { int g = gGoto; gGoto = -1; return g; }
bool mouseTakeWake()     { bool w = gWake; gWake = false; return w; }
bool mouseTakeActivity() { bool a = gActivity; gActivity = false; return a; }
bool mouseConnected()    { return gSt == MSt::Connected || simActive(); }
bool mouseCursorActive() { return mouseConnected() && millis() - gLastMoveMs < 5000; }

// ===========================================================================
// Virtual finger: buttons/motion/wheel -> touch semantics (mouse task)
// ===========================================================================
static uint32_t gDownMs = 0, gLastTapMs = 0;
static float    gDownX = 0, gDownY = 0, gTravel = 0;
static bool     gLongFired = false;
static uint16_t gLastTapX = 0, gLastTapY = 0;
static uint32_t gReleaseAt = 0;             // keep the finger down >= 60 ms (I/O polls at 20 ms)
// Wheel flick: a scripted finger path.
static int      gFlickStep = 0, gFlickSteps = 0;
static float    gFlickX = 0, gFlickY0 = 0, gFlickDy = 0;
static int      gWheelAcc = 0;
static uint8_t  gPrevBtns = 0;

static void vfSet(bool down, float x, float y) {
  int ix = (int)x, iy = (int)y;
  if (ix < 0) ix = 0; if (ix >= W) ix = W - 1;
  if (iy < 0) iy = 0; if (iy >= H) iy = H - 1;
  gVfX = ix; gVfY = iy; gVfDown = down;
}

static void leftDown() {
  gDownMs = millis(); gDownX = gCx; gDownY = gCy; gTravel = 0; gLongFired = false;
  gReleaseAt = 0;
  gLeftHeld = true;
  vfSet(true, gCx, gCy);
}

static void leftUp() {
  gLeftHeld = false;
  uint32_t now = millis(), dt = now - gDownMs;
  float dx = gCx - gDownX, dy = gCy - gDownY;
  if (!gLongFired && gTravel < 10) {
    bool dbl = now - gLastTapMs < 350 && abs((int)gCx - gLastTapX) < 12 && abs((int)gCy - gLastTapY) < 12;
    pushGesture(dbl ? Gesture::DoubleTap : Gesture::SingleTap, (uint16_t)gCx, (uint16_t)gCy);
    gLastTapMs = dbl ? 0 : now; gLastTapX = (uint16_t)gCx; gLastTapY = (uint16_t)gCy;
  } else if (!gLongFired && dt < 450 && gTravel > 50) {
    // Quick flick-drag = swipe (like the touch chip). Slow drags stay drags,
    // so sliders and drag-scrolling don't fire swipe-right "back".
    Gesture g = fabsf(dx) > fabsf(dy) ? (dx > 0 ? Gesture::SwipeRight : Gesture::SwipeLeft)
                                      : (dy > 0 ? Gesture::SwipeDown : Gesture::SwipeUp);
    pushGesture(g, (uint16_t)gCx, (uint16_t)gCy);
  }
  gReleaseAt = gDownMs + 60;                  // tick lifts the finger when due
}

static void vfTick() {
  uint32_t now = millis();
  // Long press: held still for 0.8 s (the chip reports it while still held).
  if (gLeftHeld && !gLongFired && gTravel < 10 && now - gDownMs > 800) {
    gLongFired = true;
    pushGesture(Gesture::LongPress, (uint16_t)gCx, (uint16_t)gCy);
  }
  if (gLeftHeld) vfSet(true, gCx, gCy);
  else if (gReleaseAt && (int32_t)(now - gReleaseAt) >= 0 && !gFlickSteps) { gReleaseAt = 0; vfSet(false, gCx, gCy); }
  // Wheel flick in progress: down, 4 moves, up (~120 ms), then the next one.
  if (gFlickSteps) {
    if (gFlickStep < gFlickSteps) {
      float t = (float)gFlickStep / (gFlickSteps - 1);
      vfSet(true, gFlickX, gFlickY0 + gFlickDy * t);
      gFlickStep++;
    } else {
      vfSet(false, gFlickX, gFlickY0 + gFlickDy);
      gFlickSteps = 0;
    }
  } else if (gWheelAcc && !gLeftHeld && !gVfDown) {
    // Positive = finger moves down (content scrolls back up); negative = up.
    int notches = gWheelAcc; gWheelAcc = 0;
    if (notches > 6) notches = 6; if (notches < -6) notches = -6;
    gFlickX = gCx;
    gFlickDy = notches * 28.0f;
    gFlickY0 = gCy - gFlickDy / 2;
    if (gFlickY0 < 20) gFlickY0 = 20; if (gFlickY0 > H - 20) gFlickY0 = H - 20;
    if (gFlickY0 + gFlickDy < 0) gFlickDy = -gFlickY0;
    if (gFlickY0 + gFlickDy > H - 1) gFlickDy = H - 1 - gFlickY0;
    gFlickStep = 0; gFlickSteps = 6;
  }
  if (gVBtn && (int32_t)(now - gVBtnUntil) >= 0) gVBtn = false;
}

static void handleReport(const MouseMap &m, const uint8_t *d, int len) {
  int32_t dx = getSigned(d, len, m.x), dy = getSigned(d, len, m.y);
  int32_t wh = getSigned(d, len, m.wheel), pan = getSigned(d, len, m.pan);
  uint8_t btns = 0;
  for (int b = 0; b < 5; b++) if (m.btn[b].ok && getBits(d, len, m.btn[b])) btns |= 1 << b;

  if (dx || dy) {
    // Pointer acceleration: slow moves stay precise, fast moves cross the
    // 240 px screen in a flick.
    float gain = 0.15f + gSpeed * 0.12f;
    float mag = sqrtf((float)(dx * dx + dy * dy));
    float acc = gain * (1.0f + mag * 0.05f);
    float nx = gCx + dx * acc, ny = gCy + dy * acc;
    if (nx < 0) nx = 0; if (nx > W - 1) nx = W - 1;
    if (ny < 0) ny = 0; if (ny > H - 1) ny = H - 1;
    if (gLeftHeld) gTravel += fabsf(nx - gCx) + fabsf(ny - gCy);
    gCx = nx; gCy = ny;
  }
  if (dx || dy || wh || pan || btns != gPrevBtns) {
    gLastMoveMs = millis(); gWake = true; gActivity = true;
  }
  uint8_t pressed = btns & ~gPrevBtns, released = gPrevBtns & ~btns;
  if (pressed & 0x01) leftDown();
  if (released & 0x01) leftUp();
  if (pressed & 0x02) { gVBtn = true; gVBtnUntil = millis() + 200; }          // right = back
  if (pressed & 0x04) gGoto = (int)Screen::Watch;                             // middle = home
  if (pressed & 0x08) { gVBtn = true; gVBtnUntil = millis() + 200; }          // side "back"
  gPrevBtns = btns;
  // HID wheel: + = rolled away from you = scroll up = finger moves down.
  // gWheelAcc > 0 drags the finger down, so it takes the wheel value as-is.
  if (wh) gWheelAcc += wh;
  if (pan) pushGesture(pan > 0 ? Gesture::SwipeLeft : Gesture::SwipeRight, (uint16_t)gCx, (uint16_t)gCy);
}

// ===========================================================================
// BLE (bluedroid client)
// ===========================================================================
static BLEClient *gClient = nullptr;
static MouseMap   gMap;

class MouseSecCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t r) override { gAuthOk = r.success; gAuthDone = true; }
};
static MouseSecCb gSecCb;

class MouseClientCb : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override { gDisconnected = true; }
};
static MouseClientCb gClientCb;

static void onReport(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool) {
  Rpt r; r.rid = 0;
  uint16_t h = c->getHandle();
  for (int i = 0; i < gHandleN; i++) if (gHandleRid[i][0] == h) r.rid = (uint8_t)gHandleRid[i][1];
  r.len = len > sizeof(r.d) ? sizeof(r.d) : len;
  memcpy(r.d, data, r.len);
  if (gRptQ) xQueueSend(gRptQ, &r, 0);
}

class MouseScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice ad) override {
    bool hid = ad.haveServiceUUID() && ad.isAdvertisingService(BLEUUID((uint16_t)0x1812));
    bool mouseApp = ad.haveAppearance() && (ad.getAppearance() == 0x03C2 || ad.getAppearance() == 0x03C0);
    if (!hid && !mouseApp) return;
    std::string a = ad.getAddress().toString();
    for (int i = 0; i < gFoundN; i++) if (strcmp(gFound[i].addr, a.c_str()) == 0) return;
    if (gFoundN >= 6) return;
    Found &f = gFound[gFoundN];
    strlcpy(f.addr, a.c_str(), sizeof(f.addr));
    f.type = (uint8_t)ad.getAddressType();
    strlcpy(f.name, ad.haveName() ? ad.getName().c_str() : "(no name)", sizeof(f.name));
    f.rssi = ad.haveRSSI() ? ad.getRSSI() : 0;
    gFoundN = gFoundN + 1;
    gGen++;
  }
};
static MouseScanCb gScanCb;

static void setSt(MSt s, const char *err = nullptr) {
  gSt = s;
  if (err) strlcpy(gErr, err, sizeof(gErr)); else gErr[0] = 0;
  gGen++;
}

static void savePrefs() {
  Preferences p; p.begin("mouse", false);
  p.putBool("on", gEnabled); p.putString("addr", gSavedAddr);
  p.putUChar("type", gSavedType); p.putUChar("speed", gSpeed);
  p.putString("name", gName);
  p.end();
}

static bool connectMouse(const char *addr, uint8_t type) {
  setSt(MSt::Connecting);
  EWLOGI("MOUSE", "connect %s type=%u", addr, type);
  if (!gClient) { gClient = BLEDevice::createClient(); gClient->setClientCallbacks(&gClientCb); }
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(&gSecCb);
  BLESecurity sec;
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec.setCapability(ESP_IO_CAP_NONE);
  sec.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  gAuthDone = false; gAuthOk = false; gDisconnected = false;
  if (!gClient->connect(BLEAddress(addr), (esp_ble_addr_type_t)type)) {
    setSt(MSt::Failed, "connect failed");
    return false;
  }
  // Mice only serve reports over an encrypted link: wait for bonding.
  for (int i = 0; i < 100 && !gAuthDone && !gDisconnected; i++) delay(100);
  EWLOGI("MOUSE", "auth done=%d ok=%d", (int)gAuthDone, (int)gAuthOk);
  if (gDisconnected) { setSt(MSt::Failed, "dropped while pairing"); return false; }
  BLERemoteService *svc = gClient->getService(BLEUUID((uint16_t)0x1812));
  if (!svc) { gClient->disconnect(); setSt(MSt::Failed, "no HID service"); return false; }

  BLERemoteCharacteristic *mapC = svc->getCharacteristic(BLEUUID((uint16_t)0x2A4B));
  bool mapped = false;
  if (mapC && mapC->canRead()) {
    std::string rm = mapC->readValue();
    mapped = parseReportMap((const uint8_t *)rm.data(), rm.size(), gMap);
    EWLOGI("MOUSE", "report map %u bytes, mouse=%d rid=%u x=%u/%u y=%u/%u wheel=%d pan=%d",
           (unsigned)rm.size(), (int)mapped, gMap.rid, gMap.x.bit, gMap.x.size, gMap.y.bit, gMap.y.size,
           (int)gMap.wheel.ok, (int)gMap.pan.ok);
  }

  gHandleN = 0;
  int subscribed = 0;
  uint8_t firstRid = 0;
  std::map<uint16_t, BLERemoteCharacteristic *> *chars = svc->getCharacteristicsByHandle();
  for (auto &kv : *chars) {
    BLERemoteCharacteristic *c = kv.second;
    if (!c->getUUID().equals(BLEUUID((uint16_t)0x2A4D)) || !c->canNotify()) continue;
    uint8_t rid = 0, rtype = 1;
    BLERemoteDescriptor *ref = c->getDescriptor(BLEUUID((uint16_t)0x2908));
    if (ref) {
      std::string v = ref->readValue();
      if (v.size() >= 2) { rid = (uint8_t)v[0]; rtype = (uint8_t)v[1]; }
    }
    if (rtype != 1 || gHandleN >= 8) continue;            // input reports only
    gHandleRid[gHandleN][0] = c->getHandle(); gHandleRid[gHandleN][1] = rid; gHandleN++;
    if (!subscribed) firstRid = rid;
    c->registerForNotify(onReport);
    subscribed++;
    EWLOGI("MOUSE", "input report handle=0x%04x rid=%u", c->getHandle(), rid);
  }
  if (!subscribed) { gClient->disconnect(); setSt(MSt::Failed, "no input reports"); return false; }
  if (!mapped) { bootMap(gMap, firstRid); EWLOGW("MOUSE", "no usable report map: boot layout"); }
  strlcpy(gName, gName[0] ? gName : addr, sizeof(gName));
  setSt(MSt::Connected);
  EWLOGI("MOUSE", "connected %s reports=%d", gName, subscribed);
  gLastMoveMs = millis();
  gCx = W / 2; gCy = H / 2;
  return true;
}

static void doScan() {
  setSt(MSt::Scanning);
  gFoundN = 0;
  BLEScan *s = BLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&gScanCb, true);
  s->setActiveScan(true);
  s->setInterval(100); s->setWindow(99);
  s->start(6, false);                         // blocking 6 s
  s->clearResults();
  EWLOGI("MOUSE", "scan found=%d", (int)gFoundN);
  setSt(gSavedAddr[0] ? MSt::Connecting : MSt::NoDevice);
}

static uint32_t gLastTick = 0;
static void serviceInput() {
  Rpt r;
  while (xQueueReceive(gRptQ, &r, 0) == pdTRUE) {
    if (r.rid == 0xEE) { handleReport(gSimMap, r.d, r.len); continue; }   // msim
    if (gMap.ok && (r.rid == gMap.rid || gMap.rid == 0 || gHandleN == 1)) handleReport(gMap, r.d, r.len);
  }
  if (millis() - gLastTick >= 20) { gLastTick = millis(); vfTick(); }
}

// Input task: always responsive (reports -> cursor/finger, 20 ms finger tick).
// Kept apart from the BLE task, whose connect/scan calls block for seconds.
static void mouseInTask(void *) {
  for (;;) { serviceInput(); delay(4); }
}

static void mouseTask(void *) {
  uint32_t nextTry = 0, backoff = 4000;          // absent mouse: retry 4 s, doubling to 60 s
  for (;;) {
    if (!gEnabled) {
      if (gSt == MSt::Connected && gClient) gClient->disconnect();
      if (gStackHeld) { bleStackRelease("mouse"); gStackHeld = false; }
      if (!simActive()) { gVfDown = false; gLeftHeld = false; }
      if (gSt != MSt::Off) setSt(MSt::Off);
      delay(50);
      continue;
    }
    if (!gStackHeld) {
      if (!bleStackAcquire("mouse")) { setSt(MSt::Failed, "Bluetooth failed"); delay(2000); continue; }
      gStackHeld = true;
      BLEDevice::setMTU(64);
    }
    if (gReqForget) {
      gReqForget = false;
      if (gSt == MSt::Connected) gClient->disconnect();
      gSavedAddr[0] = 0; gName[0] = 0; savePrefs();
      setSt(MSt::NoDevice);
    }
    if (gReqScan) {
      gReqScan = false;
      if (gSt == MSt::Connected) gClient->disconnect();
      doScan();
    }
    if (gReqPair >= 0) {
      int i = gReqPair; gReqPair = -1;
      if (i < gFoundN) {
        strlcpy(gSavedAddr, gFound[i].addr, sizeof(gSavedAddr));
        gSavedType = gFound[i].type;
        strlcpy(gName, gFound[i].name, sizeof(gName));
        if (connectMouse(gSavedAddr, gSavedType)) savePrefs();
        nextTry = millis() + 3000;
      }
    }
    if (gSt == MSt::Connected && (gDisconnected || !gClient->isConnected())) {
      EWLOGI("MOUSE", "disconnected");
      gVfDown = false; gLeftHeld = false; gPrevBtns = 0;
      setSt(MSt::Connecting);
      nextTry = millis() + 1000;
    }
    if (gSt != MSt::Connected && gSt != MSt::Scanning) {
      if (!gSavedAddr[0]) { if (gSt != MSt::NoDevice && gSt != MSt::Failed) setSt(MSt::NoDevice); }
      else if ((int32_t)(millis() - nextTry) >= 0) {
        if (connectMouse(gSavedAddr, gSavedType)) backoff = 4000;   // mice sleep: keep retrying gently
        else backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
        nextTry = millis() + backoff;
      }
    }
    delay(20);
  }
}

// Console test hook (ewlog): `msim dx dy buttons wheel [pan]` injects one
// boot-layout report through the same decode -> cursor -> finger path as a real
// mouse, and keeps the cursor live for 10 s. buttons: 1 left 2 right 4 middle.
static void simReport(int btn, int dx, int dy, int wh, int pan) {
  if (!gSimMap.ok) { bootMap(gSimMap, 0xEE); gSimMap.pan = { true, 0xEE, 32, 8, true }; }
  gSimUntil = millis() + 10000;
  Rpt r; r.rid = 0xEE; r.len = 5;
  r.d[0] = (uint8_t)btn; r.d[1] = (uint8_t)(int8_t)constrain(dx, -127, 127);
  r.d[2] = (uint8_t)(int8_t)constrain(dy, -127, 127); r.d[3] = (uint8_t)(int8_t)wh; r.d[4] = (uint8_t)(int8_t)pan;
  if (gRptQ) xQueueSend(gRptQ, &r, 0);
}

bool mouseHandleLine(char *line) {
  if (strncmp(line, "msimdrag ", 9) == 0) {     // msimdrag dx dy ms: left-button drag with real timing
    int dx = 0, dy = 0, ms = 200;
    sscanf(line + 9, "%d %d %d", &dx, &dy, &ms);
    simReport(1, 0, 0, 0, 0); delay(30);
    for (int i = 0; i < 5; i++) { simReport(1, dx / 5, dy / 5, 0, 0); delay(ms / 5); }
    simReport(0, 0, 0, 0, 0); delay(120);
    Serial.printf("msim ok drag cursor=%d,%d finger=%d\n", (int)gCx, (int)gCy, (int)gVfDown);
    return true;
  }
  if (strncmp(line, "msimto ", 7) == 0) {       // msimto x y: warp the cursor (tests)
    int x = 0, y = 0;
    sscanf(line + 7, "%d %d", &x, &y);
    gCx = constrain(x, 0, W - 1); gCy = constrain(y, 0, H - 1);
    gSimUntil = millis() + 10000; gLastMoveMs = millis();
    delay(60);
    Serial.printf("msim ok cursor=%d,%d paints=%lu drawn=%d at=%d,%d\n", (int)gCx, (int)gCy,
                  (unsigned long)cPaints, (int)cDrawn, cX, cY);
    return true;
  }
  if (strncmp(line, "msim", 4) != 0) return false;
  int dx = 0, dy = 0, btn = 0, wh = 0, pan = 0;
  int n = sscanf(line + 4, "%d %d %d %d %d", &dx, &dy, &btn, &wh, &pan);
  if (n < 2) { Serial.println("usage: msim dx dy [buttons] [wheel] [pan]"); return true; }
  if (!gSimMap.ok) { bootMap(gSimMap, 0xEE); gSimMap.pan = { true, 0xEE, 32, 8, true }; }
  gSimUntil = millis() + 10000;
  Rpt r; r.rid = 0xEE; r.len = 5;
  r.d[0] = (uint8_t)btn; r.d[1] = (uint8_t)(int8_t)constrain(dx, -127, 127);
  r.d[2] = (uint8_t)(int8_t)constrain(dy, -127, 127); r.d[3] = (uint8_t)(int8_t)wh; r.d[4] = (uint8_t)(int8_t)pan;
  if (gRptQ) xQueueSend(gRptQ, &r, 0);
  delay(30);                                    // let the mouse task apply it
  Serial.printf("msim ok cursor=%d,%d finger=%d paints=%lu drawn=%d at=%d,%d\n", (int)gCx, (int)gCy,
                (int)gVfDown, (unsigned long)cPaints, (int)cDrawn, cX, cY);
  return true;
}

void mouseBegin() {
  Preferences p; p.begin("mouse", true);
  gEnabled = p.getBool("on", false);
  strlcpy(gSavedAddr, p.getString("addr", "").c_str(), sizeof(gSavedAddr));
  gSavedType = p.getUChar("type", 0);
  gSpeed = p.getUChar("speed", 3);
  strlcpy(gName, p.getString("name", "").c_str(), sizeof(gName));
  p.end();
  if (gSpeed < 1 || gSpeed > 5) gSpeed = 3;
  if (!gRptQ) gRptQ = xQueueCreate(24, sizeof(Rpt));
  if (!gTask) {
    xTaskCreatePinnedToCore(mouseTask, "mousebt", 8192, nullptr, 2, &gTask, 0);
    xTaskCreatePinnedToCore(mouseInTask, "mousein", 3072, nullptr, 3, nullptr, 0);
  }
  EWLOGI("MOUSE", "begin on=%d paired=%s", (int)gEnabled, gSavedAddr[0] ? gSavedAddr : "-");
}

// ===========================================================================
// Cursor (render task). Composited from the ewsim shadow (what the panel
// shows without the cursor) with the bus tap paused, so the shadow never
// contains cursor pixels and restoring is just a copy back.
// ===========================================================================
static const int CW = 11, CH = 17;
static const char *kArrow[CH] = {
  "X..........", "XX.........", "XOX........", "XOOX.......", "XOOOX......",
  "XOOOOX.....", "XOOOOOX....", "XOOOOOOX...", "XOOOOOOOX..", "XOOOOOOOOX.",
  "XOOOOOXXXXX", "XOOXOOX....", "XOX.XOOX...", "XX..XOOX...", "X....XOOX..",
  ".....XOOX..", "......XX...",
};

static void pushRect(int x, int y, bool withCursor, bool pressed) {
  const uint16_t *sh = ewsimShadow();
  if (!sh) return;
  int w = CW, h = CH;
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  static uint16_t buf[CW * CH];
  uint16_t fill = pressed ? 0xFD20 : 0xFFFF;          // orange while held
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++) {
      uint16_t p = sh[(y + r) * W + x + c];
      if (withCursor) {
        char k = kArrow[r][c];
        if (k == 'X') p = 0x0000; else if (k == 'O') p = fill;
      }
      buf[r * w + c] = p;
    }
  gfx->draw16bitRGBBitmap(x, y, buf, w, h);
}

void mouseCursorPaint() {
  if (!gfx || !ewsimShadow()) return;
  bool want = mouseCursorActive();
  int x = (int)gCx, y = (int)gCy;
  bool pressed = gLeftHeld;
  uint32_t px = ewsimPxCount();
  if (!want) {
    if (cDrawn) {
      ewsimTapPause(true);
      pushRect(cX, cY, false, false);                  // shadow = screen without cursor
      ewsimTapPause(false);
      cDrawn = false;
    }
    return;
  }
  if (cDrawn && x == cX && y == cY && px == cPx && pressed == cPressed) return;
  // No outer startWrite(): draw16bitRGBBitmap takes the SPI bus itself and the
  // bus lock isn't recursive (a nested acquire hung the render task -> WDT).
  ewsimTapPause(true);
  if (cDrawn && (x != cX || y != cY)) pushRect(cX, cY, false, false);
  pushRect(x, y, true, pressed);
  ewsimTapPause(false);
  cDrawn = true; cX = x; cY = y; cPressed = pressed;
  cPx = ewsimPxCount();
  cPaints++;
}

// ===========================================================================
// Mouse app (pairing + settings)
// ===========================================================================
static const int16_t BTN_Y = 88, BTN_H = 38, LIST_Y = 176, ROW_H = 26;

static const char *stText() {
  switch (gSt) {
    case MSt::Off:        return "Mouse off";
    case MSt::NoDevice:   return "On - no mouse paired";
    case MSt::Scanning:   return "Scanning for mice...";
    case MSt::Connecting: return "Connecting...";
    case MSt::Connected:  return "Connected";
    case MSt::Failed:     return "Failed";
  }
  return "";
}

static void button(int16_t x, int16_t y, int16_t w, const char *label, bool on) {
  ThemeColors t = theme();
  uint16_t fill = on ? t.accent : t.line;
  gfx->fillRoundRect(x, y, w, BTN_H, 10, fill);
  gfx->setTextSize(2);
  gfx->setTextColor(contrastFor(fill), fill);
  int16_t tw = strlen(label) * 12;
  gfx->setCursor(x + (w - tw) / 2, y + 11);
  gfx->print(label);
}

void MouseView::drawAll() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Mouse");
  gfx->setTextSize(1);
  MSt s = gSt;
  gfx->fillCircle(14, 60, 5, s == MSt::Connected ? GREEN : s == MSt::Failed ? RED : s == MSt::Off ? DARKGREY : ORANGE);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(26, 56); gfx->print(stText());
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(26, 70);
  if (s == MSt::Failed) gfx->print(gErr);
  else if (gSavedAddr[0]) gfx->print(gName[0] ? gName : gSavedAddr);
  button(8, BTN_Y, 108, gEnabled ? "On" : "Off", gEnabled);
  button(124, BTN_Y, 108, "Pair", false);
  // Speed - n +   Forget
  button(8, BTN_Y + 44, 40, "-", false);
  gfx->setTextSize(2); gfx->setTextColor(t.fg, t.bg);
  char b[8]; snprintf(b, sizeof b, "%u", gSpeed);
  gfx->setCursor(56, BTN_Y + 55); gfx->print(b);
  button(76, BTN_Y + 44, 40, "+", false);
  button(124, BTN_Y + 44, 108, "Forget", false);
  gfx->drawFastHLine(20, LIST_Y - 4, 200, t.line);
  gfx->setTextSize(1);
  if (gFoundN > 0 || s == MSt::Scanning) {
    for (int i = 0; i < gFoundN && i < 4; i++) {
      int16_t y = LIST_Y + i * ROW_H;
      gfx->fillRoundRect(6, y, W - 12, ROW_H - 3, 6, t.line);
      gfx->setTextColor(contrastFor(t.line), t.line);
      gfx->setCursor(12, y + 8);
      char row[40]; snprintf(row, sizeof row, "%.20s  %d dBm", gFound[i].name, gFound[i].rssi);
      gfx->print(row);
    }
    if (s == MSt::Scanning && gFoundN == 0) { gfx->setTextColor(t.line, t.bg); gfx->setCursor(12, LIST_Y + 8); gfx->print("Put the mouse in pairing mode..."); }
    else if (gFoundN) { gfx->setTextColor(t.line, t.bg); gfx->setCursor(12, LIST_Y + 4 * ROW_H + 2); gfx->print("Tap a mouse to pair"); }
  } else {
    gfx->setTextColor(t.line, t.bg);
    const char *help[] = { "Left: tap / drag   Right: back", "Middle: home   Wheel: scroll",
                           "Tilt wheel: swipe pages", "Hold left 0.8 s: long press" };
    for (int i = 0; i < 4; i++) { gfx->setCursor(12, LIST_Y + 6 + i * 16); gfx->print(help[i]); }
  }
}

void MouseView::onEnter() { firstDraw = true; shownGen = 0xFFFFFFFF; }

void MouseView::render() {
  if (!gfx) return;
  if (firstDraw || shownGen != gGen) { drawAll(); firstDraw = false; shownGen = gGen; }
}

static bool hit(const Event &e, int16_t x, int16_t y, int16_t w, int16_t h) {
  return e.x >= x && e.x < x + w && e.y >= y && e.y < y + h;
}

void MouseView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Gesture && e.gesture == Gesture::SwipeRight)) { switchTo(Screen::AppList); return; }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
  bool changed = true;
  if (hit(e, 8, BTN_Y, 108, BTN_H)) { gEnabled = !gEnabled; savePrefs(); }
  else if (hit(e, 124, BTN_Y, 108, BTN_H)) { if (!gEnabled) { gEnabled = true; savePrefs(); } gReqScan = true; }
  else if (hit(e, 8, BTN_Y + 44, 40, BTN_H)) { if (gSpeed > 1) gSpeed--; savePrefs(); }
  else if (hit(e, 76, BTN_Y + 44, 40, BTN_H)) { if (gSpeed < 5) gSpeed++; savePrefs(); }
  else if (hit(e, 124, BTN_Y + 44, 108, BTN_H)) gReqForget = true;
  else if (e.y >= LIST_Y && gFoundN) {
    int i = (e.y - LIST_Y) / ROW_H;
    if (i < gFoundN && i < 4) gReqPair = i; else changed = false;
  } else changed = false;
  if (changed) { haptic(HapticRole::Tap); gGen++; }
}

// Connected mouse address, for the watch-face BLE signal icon.
bool mousePeer(uint8_t out[6]) {
  if (gSt != MSt::Connected || !gClient) return false;
  memcpy(out, *gClient->getPeerAddress().getNative(), 6);
  return true;
}
