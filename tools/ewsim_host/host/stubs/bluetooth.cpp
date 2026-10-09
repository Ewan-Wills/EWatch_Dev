// ewsim_host stub for src/apps/bluetooth.cpp (Bluedroid BLE remote + scanner).
// BLE is not simulated: the view paints a NOT SIMULATED card and the run fails
// unless --allow-unsupported.
#include <Arduino_GFX_Library.h>
#include "bluetooth.h"
#include "host_unsupported_view.h"
void BluetoothView::onEnter() { ewsimHostUnsupported("Bluetooth app (BLE radio)"); firstDraw = true; }
void BluetoothView::onExit() {}
void BluetoothView::render() { if (firstDraw) { ewsimDrawUnsupported("Bluetooth", "needs the BLE radio"); firstDraw = false; } }
void BluetoothView::onEvent(const Event &) {}

// Background remote service API (Control Centre tile + media row). No radio:
// on/off is tracked so the UI can be exercised; nothing ever connects, and
// sends fail with the same log line the firmware prints when unpaired.
#include "ewlog.h"
#include "model.h"
static bool sBtOn = false;
bool btRemoteOn()        { return sBtOn; }
bool btRemoteConnected() { return false; }
bool btRemoteFailed()    { return false; }
bool btRemoteSetOn(bool on) {
  if (on == sBtOn) return true;
  sBtOn = on;
  EWLOGI("BLE", on ? "remote on (background)" : "remote off");
  { ModelLock lk; model.revision++; }
  return true;
}
void btBegin() { EWLOGI("BLE", "boot pref on=0 (sim)"); }
bool btRemoteSend(uint8_t key) {
  const char *name = key == BT_KEY_PLAYPAUSE ? "playpause" : key == BT_KEY_NEXT ? "next" :
                     key == BT_KEY_PREV ? "prev" : "other";
  EWLOGW("BLE", "key %s dropped: not connected", name);
  return false;
}
void btDrawMediaGlyph(Arduino_GFX *g, uint8_t key, int16_t cx, int16_t cy, uint16_t c) {
  if (key == BT_KEY_PLAYPAUSE) { g->fillTriangle(cx - 16, cy - 13, cx - 16, cy + 13, cx + 2, cy, c);
                                 g->fillRect(cx + 6, cy - 12, 4, 24, c); g->fillRect(cx + 13, cy - 12, 4, 24, c); }
  else if (key == BT_KEY_NEXT) { g->fillTriangle(cx - 14, cy - 11, cx - 14, cy + 11, cx + 2, cy, c);
                                 g->fillTriangle(cx - 2, cy - 11, cx - 2, cy + 11, cx + 14, cy, c); }
  else                         { g->fillTriangle(cx + 14, cy - 11, cx + 14, cy + 11, cx - 2, cy, c);
                                 g->fillTriangle(cx + 2, cy - 11, cx + 2, cy + 11, cx - 14, cy, c); }
}
