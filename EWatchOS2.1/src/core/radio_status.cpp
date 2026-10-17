#include <Arduino.h>
#include "radio_status.h"
#include "model.h"
#include "wifi_svc.h"
#include "ble_stack.h"
#include "bluetooth.h"
#include "ui_style.h"
#if EW_APP_MOUSE
#include "btmouse.h"
#endif

// RSSI bands (dBm). WiFi stays usable to about -75; BLE to about -85.
static LinkQ bandWifi(int r) { return r >= -60 ? LinkQ::Good : r >= -72 ? LinkQ::Medium : LinkQ::Poor; }
static LinkQ bandBle(int r)  { return r >= -65 ? LinkQ::Good : r >= -80 ? LinkQ::Medium : LinkQ::Poor; }

LinkQ wifiLinkQ() {
  bool en, conn; WifiMode mode; int8_t rssi; uint8_t clients;
  { ModelLock lk; en = model.wifiEnabled; conn = model.wifiConnected; mode = model.wifiMode;
    rssi = model.wifiRssi; clients = model.wifiApClients; }
  if (!en) return LinkQ::Off;
  if (mode == WifiMode::AP) return clients ? LinkQ::Good : LinkQ::None;   // no RSSI for an AP
  if (conn && rssi < 0) return bandWifi(rssi);                          // inside a sync window
  int8_t last;
  if (wifiSvcLastLink(last)) return bandWifi(last);                      // last window's link
  return LinkQ::None;
}

LinkQ bleLinkQ() {
  if (bleStackUsers() == 0) return LinkQ::Off;
  uint8_t peer[6];
  bool linked = btRemotePeer(peer);
#if EW_APP_MOUSE
  if (!linked) linked = mousePeer(peer);
#endif
  if (!linked) return LinkQ::None;
  static uint32_t lastAskMs = 0;
  if (lastAskMs == 0 || millis() - lastAskMs > 4000) { lastAskMs = millis() | 1; bleRequestRssi(peer); }
  int8_t r;
  if (!bleLastRssi(r, 12000)) return LinkQ::None;   // first reading lands within ~100 ms
  return bandBle(r);
}

uint16_t linkQColor(LinkQ q) {
  switch (q) {
    case LinkQ::Good:   return aura::kGreen;
    case LinkQ::Medium: return aura::kOrange;
    case LinkQ::Poor:   return aura::kRed;
    default:            return 0x8410;   // mid grey: on, no link
  }
}

const char *linkQName(LinkQ q) {
  static const char *n[] = { "off", "none", "poor", "medium", "good" };
  return n[(uint8_t)q];
}
