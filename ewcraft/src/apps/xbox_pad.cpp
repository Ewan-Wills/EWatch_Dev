// Xbox controller service + "Xbox Pad" screen — see xbox_pad.h.
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>

#include "xbox_pad.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "wifi_svc.h"

static const int16_t W = 240, H = 280;

enum class PSt : uint8_t { Off, Searching, Connecting, Connected, Failed };
static volatile PSt  gSt = PSt::Off;
static volatile bool gEnabled = false;
static volatile uint32_t gGen = 0;
static char gErr[40] = "";
static TaskHandle_t gTask = nullptr;
static bool gInited = false;

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static PadState gPad = {};
static NimBLEClient *gClient = nullptr;
static NimBLERemoteCharacteristic *gOut = nullptr;   // rumble output report
static volatile bool gDropped = false;
static volatile uint8_t gRumbleStrength = 0; static volatile uint16_t gRumbleMs = 0;

static float axis(uint16_t raw) {
  float v = ((float)raw - 32768.0f) / 32768.0f;
  const float dz = 0.12f;                            // stick dead zone
  if (fabsf(v) < dz) return 0;
  return (v > 0 ? v - dz : v + dz) / (1.0f - dz);
}

// NimBLE host task: parse straight into the shared state (tiny, no logging).
static void onReport(NimBLERemoteCharacteristic *, uint8_t *d, size_t len, bool) {
  if (len < 16) return;
  PadState p;
  portENTER_CRITICAL(&gMux);
  p = gPad;
  portEXIT_CRITICAL(&gMux);
  p.lx = axis(d[0] | (d[1] << 8));
  p.ly = axis(d[2] | (d[3] << 8));
  p.rx = axis(d[4] | (d[5] << 8));
  p.ry = axis(d[6] | (d[7] << 8));
  p.lt = (d[8] | (d[9] << 8)) / 1023.0f;
  p.rt = (d[10] | (d[11] << 8)) / 1023.0f;
  uint16_t b = 0;
  uint8_t b13 = d[13], b14 = d[14], b15 = d[15];
  if (b13 & 0x01) b |= PAD_A;   if (b13 & 0x02) b |= PAD_B;
  if (b13 & 0x08) b |= PAD_X;   if (b13 & 0x10) b |= PAD_Y;
  if (b13 & 0x40) b |= PAD_LB;  if (b13 & 0x80) b |= PAD_RB;
  if (b14 & 0x04) b |= PAD_VIEW; if (b14 & 0x08) b |= PAD_MENU;
  if (b14 & 0x10) b |= PAD_XBOX; if (b14 & 0x20) b |= PAD_LS;
  if (b14 & 0x40) b |= PAD_RS;   if (b15 & 0x01) b |= PAD_SHARE;
  switch (d[12]) {                                   // hat, clockwise from N
    case 1: b |= PAD_UP; break;    case 2: b |= PAD_UP | PAD_RIGHT; break;
    case 3: b |= PAD_RIGHT; break; case 4: b |= PAD_DOWN | PAD_RIGHT; break;
    case 5: b |= PAD_DOWN; break;  case 6: b |= PAD_DOWN | PAD_LEFT; break;
    case 7: b |= PAD_LEFT; break;  case 8: b |= PAD_UP | PAD_LEFT; break;
  }
  p.buttons = b;
  p.reports++;
  portENTER_CRITICAL(&gMux);
  gPad = p;
  portEXIT_CRITICAL(&gMux);
}

class PadClientCb : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *) override { gDropped = true; }
  bool onConfirmPIN(uint32_t) override { return true; }
};
static PadClientCb gClientCb;

PadState padGet() {
  PadState p;
  portENTER_CRITICAL(&gMux);
  p = gPad;
  portEXIT_CRITICAL(&gMux);
  p.connected = gSt == PSt::Connected;
  return p;
}
bool     padEnabled() { return gEnabled; }
uint32_t padGen() { return gGen; }
void padRumble(uint8_t strength, uint16_t ms) { gRumbleStrength = strength; gRumbleMs = ms; }

static void setSt(PSt s, const char *err = nullptr) {
  gSt = s;
  if (err) strlcpy(gErr, err, sizeof(gErr)); else gErr[0] = 0;
  gGen++;
}

const char *padStatus() {
  static char b[48];
  switch (gSt) {
    case PSt::Off:        return "Controller off";
    case PSt::Searching:  return "Searching... (pair or switch on)";
    case PSt::Connecting: return "Connecting...";
    case PSt::Connected:  snprintf(b, sizeof b, "Connected  bat %u%%", gPad.battery); return b;
    case PSt::Failed:     snprintf(b, sizeof b, "Failed: %s", gErr); return b;
  }
  return "";
}

static bool isXbox(NimBLEAdvertisedDevice &d) {
  if (d.haveAppearance() && d.getAppearance() == 0x03C4) return true;          // gamepad
  if (d.haveName() && d.getName().find("Xbox") != std::string::npos) return true;
  return false;
}

static bool connectTo(NimBLEAddress addr) {
  setSt(PSt::Connecting);
  EWLOGI("PAD", "connect %s", addr.toString().c_str());
  if (!gClient) {
    gClient = NimBLEDevice::createClient();
    gClient->setClientCallbacks(&gClientCb, false);
    gClient->setConnectionParams(6, 12, 0, 300);     // 7.5-15 ms interval: low input lag
    gClient->setConnectTimeout(6);
  }
  gDropped = false;
  if (!gClient->connect(addr, true)) { setSt(PSt::Failed, "connect"); return false; }
  // Encrypt (Just Works + bond) before touching the HID service.
  if (!gClient->secureConnection()) { gClient->disconnect(); setSt(PSt::Failed, "pairing"); return false; }
  NimBLERemoteService *svc = gClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (!svc) { gClient->disconnect(); setSt(PSt::Failed, "no HID service"); return false; }
  int subs = 0;
  gOut = nullptr;
  std::vector<NimBLERemoteCharacteristic *> *chars = svc->getCharacteristics(true);
  for (auto *c : *chars) {
    if (!(c->getUUID() == NimBLEUUID((uint16_t)0x2A4D))) continue;
    if (c->canNotify() && c->subscribe(true, onReport, true)) subs++;
    else if ((c->canWrite() || c->canWriteNoResponse()) && !gOut) gOut = c;
  }
  if (!subs) { gClient->disconnect(); setSt(PSt::Failed, "no input report"); return false; }
  NimBLERemoteService *bat = gClient->getService(NimBLEUUID((uint16_t)0x180F));
  if (bat) {
    NimBLERemoteCharacteristic *bc = bat->getCharacteristic(NimBLEUUID((uint16_t)0x2A19));
    if (bc && bc->canRead()) { std::string v = bc->readValue(); if (v.size()) gPad.battery = (uint8_t)v[0]; }
  }
  Preferences p; p.begin("pad", false); p.putString("addr", addr.toString().c_str()); p.putUChar("type", addr.getType()); p.end();
  setSt(PSt::Connected);
  EWLOGI("PAD", "connected reports=%d rumble=%d bat=%u", subs, gOut != nullptr, gPad.battery);
  return true;
}

static void sendRumble() {
  if (!gOut || gSt != PSt::Connected || !gRumbleMs) return;
  // Output report 3: enable bits, LT, RT, left, right magnitudes, on-time
  // (10 ms units), delay, repeat.
  uint8_t r[8] = { 0x03, 0, 0, gRumbleStrength, gRumbleStrength,
                   (uint8_t)(gRumbleMs / 10 > 255 ? 255 : gRumbleMs / 10), 0, 0 };
  gRumbleMs = 0;
  gOut->writeValue(r, sizeof r, false);
}

static void padTask(void *) {
  for (;;) {
    if (!gEnabled) {
      if (gClient && gClient->isConnected()) gClient->disconnect();
      if (gSt != PSt::Off) setSt(PSt::Off);
      delay(150);
      continue;
    }
    if (!gInited) {
      wifiSvcSuspend(true, 3000);                    // WiFi driver RAM -> BLE controller
      NimBLEDevice::init("EWatch");
      NimBLEDevice::setSecurityAuth(true, false, true);   // bond, no MITM, SC
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
      NimBLEDevice::setPower(ESP_PWR_LVL_P9);
      gInited = true;
      EWLOGI("PAD", "nimble up heap=%u", (unsigned)ESP.getFreeHeap());
    }
    if (gSt == PSt::Connected) {
      if (gDropped || !gClient->isConnected()) {
        EWLOGI("PAD", "disconnected");
        portENTER_CRITICAL(&gMux); uint32_t r = gPad.reports; gPad = {}; gPad.reports = r; portEXIT_CRITICAL(&gMux);
        setSt(PSt::Searching);
      } else { sendRumble(); delay(30); continue; }
    }
    // Search: bonded controller switching on, or one in pairing mode.
    setSt(PSt::Searching);
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(97); scan->setWindow(67);
    NimBLEScanResults res = scan->start(3, false);
    bool done = false;
    for (int i = 0; i < res.getCount() && !done && gEnabled; i++) {
      NimBLEAdvertisedDevice d = res.getDevice(i);
      if (!isXbox(d)) continue;
      EWLOGI("PAD", "found %s rssi=%d", d.getName().c_str(), d.getRSSI());
      done = connectTo(d.getAddress());
    }
    scan->clearResults();
    if (!done) delay(400);
  }
}

void padBegin() {
  Preferences p; p.begin("pad", true); gEnabled = p.getBool("on", false); p.end();
  if (!gTask) xTaskCreatePinnedToCore(padTask, "xboxpad", 6144, nullptr, 3, &gTask, 0);
}

void padSetEnabled(bool on) {
  gEnabled = on;
  Preferences p; p.begin("pad", false); p.putBool("on", on); p.end();
  gGen++;
}

// ===========================================================================
// "Xbox Pad" screen: pairing + live input
// ===========================================================================
static const int16_t BTN_Y = 228, BTN_H = 40;

void PadView::onEnter() {
  firstDraw = true; shownGen = 0xFFFFFFFF; shownReports = 0xFFFFFFFF;
  if (!gTask) padBegin();
  if (!gEnabled) padSetEnabled(true);              // opening the app means "find my controller"
}

void PadView::drawStatic() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Xbox Pad");
  uint16_t fill = gEnabled ? t.line : t.accent;
  gfx->fillRoundRect(8, BTN_Y, W - 16, BTN_H, 12, fill);
  gfx->setTextSize(2);
  gfx->setTextColor(contrastFor(fill), fill);
  const char *lbl = gEnabled ? "Turn off" : "Turn on";
  gfx->setCursor((W - (int16_t)strlen(lbl) * 12) / 2, BTN_Y + 13);
  gfx->print(lbl);
}

static void stick(int16_t cx, int16_t cy, float x, float y, bool pressed, ThemeColors t) {
  gfx->fillCircle(cx, cy, 30, t.bg);
  gfx->drawCircle(cx, cy, 30, t.line);
  gfx->fillCircle(cx + (int16_t)(x * 22), cy + (int16_t)(y * 22), 8, pressed ? t.accent : t.fg);
}

static void bar(int16_t x, int16_t y, float v, ThemeColors t) {
  gfx->drawRect(x, y, 50, 10, t.line);
  gfx->fillRect(x + 1, y + 1, 48, 8, t.bg);
  gfx->fillRect(x + 1, y + 1, (int16_t)(48 * v), 8, t.accent);
}

static void pill(int16_t x, int16_t y, const char *l, bool on, ThemeColors t) {
  uint16_t f = on ? t.accent : t.line;
  gfx->fillRoundRect(x, y, 34, 18, 6, f);
  gfx->setTextSize(1);
  gfx->setTextColor(contrastFor(f), f);
  gfx->setCursor(x + (34 - (int16_t)strlen(l) * 6) / 2, y + 5);
  gfx->print(l);
}

void PadView::drawLive(const PadState &p) {
  ThemeColors t = theme();
  gfx->fillRect(0, 50, W, 20, t.bg);
  gfx->setTextSize(1);
  gfx->fillCircle(12, 59, 4, p.connected ? GREEN : gSt == PSt::Failed ? RED : ORANGE);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(22, 55); gfx->print(padStatus());
  if (!p.connected && gEnabled) {
    gfx->fillRect(0, 72, W, 150, t.bg);
    gfx->setTextColor(t.line, t.bg);
    const char *help[] = { "Switch the controller on.", "First time: hold its pair",
                           "button (top edge) until the", "Xbox logo flashes fast." };
    for (int i = 0; i < 4; i++) { gfx->setCursor(16, 100 + i * 16); gfx->print(help[i]); }
    return;
  }
  stick(50, 112, p.lx, p.ly, p.buttons & PAD_LS, t);
  stick(190, 112, p.rx, p.ry, p.buttons & PAD_RS, t);
  bar(25, 76, p.lt, t); bar(165, 76, p.rt, t);
  pill(103, 90, "Y", p.buttons & PAD_Y, t);
  pill(83, 112, "X", p.buttons & PAD_X, t);  pill(123, 112, "B", p.buttons & PAD_B, t);
  pill(103, 134, "A", p.buttons & PAD_A, t);
  pill(10, 158, "LB", p.buttons & PAD_LB, t); pill(196, 158, "RB", p.buttons & PAD_RB, t);
  pill(63, 158, "View", p.buttons & PAD_VIEW, t); pill(143, 158, "Menu", p.buttons & PAD_MENU, t);
  pill(103, 158, "Xbox", p.buttons & PAD_XBOX, t);
  pill(63, 186, "<", p.buttons & PAD_LEFT, t); pill(103, 182, "^", p.buttons & PAD_UP, t);
  pill(143, 186, ">", p.buttons & PAD_RIGHT, t); pill(103, 202, "v", p.buttons & PAD_DOWN, t);
}

void PadView::render() {
  if (!gfx) return;
  PadState p = padGet();
  if (firstDraw || shownGen != gGen) { drawStatic(); firstDraw = false; shownGen = gGen; shownReports = 0xFFFFFFFF; }
  if (p.reports != shownReports) { shownReports = p.reports; drawLive(p); }
}

void PadView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Gesture && e.gesture == Gesture::SwipeRight)) { switchTo(Screen::AppList); return; }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
  if (e.y >= BTN_Y && e.y < BTN_Y + BTN_H) { hapticBuzz(60, 30); padSetEnabled(!gEnabled); }
}
