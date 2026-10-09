// WiFi settings pages: power/mode/status + the saved-networks list. Both
// compiled out entirely when EWATCH_ENABLE_WIFI=0.
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "storage.h"
#include "wifi_svc.h"
#include "system_views.h"

static const int16_t W = 240;
static const int16_t H = 280;

// =====================================================================
// SettingsWifi — top-level WiFi page: power on/off, mode (Host AP vs
// Client), live status, link to the saved-networks list.
// =====================================================================
class SettingsWifiView : public View {
public:
  void onEnter() override {
    uiClearAll(); firstDraw = true;
    last.en = false; last.mode = WifiMode::AP; last.conn = false;
    last.rssi = 0; last.cli = 0; last.rtcOk = false; last.ssid[0] = '\0';
    // Opening this page is an explicit user action needing the network —
    // ask the RadioManager for a sync window so the status becomes live.
    wifiSvcKickWindow();
  }
  void render() override {
    if (!gfx) return;
    Model snap;
    { ModelLock lk; snap = model; }
    if (firstDraw) {
      drawTitleBar("WiFi");
      drawKnownBtn();
      firstDraw = false;
    }
    if (last.en != snap.wifiEnabled)               drawEnableBtn(snap.wifiEnabled);
    if (last.mode != snap.wifiMode || last.en != snap.wifiEnabled)
      drawModeBtns(snap.wifiMode);
    Snap cur{ snap.wifiEnabled, snap.wifiMode, snap.wifiConnected,
              snap.wifiRssi, snap.wifiApClients, snap.rtcOk,
              snap.wifiIpV4, "" };
    strncpy(cur.ssid, snap.wifiSsid, 32);
    bool statusChanged =
       last.en != cur.en || last.mode != cur.mode ||
       last.conn != cur.conn || last.rssi != cur.rssi ||
       last.cli  != cur.cli || last.ip != cur.ip ||
       strncmp(last.ssid, cur.ssid, 32) != 0;
    if (statusChanged) drawStatus(cur);
    last = cur;
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::Settings); return; }

    if (uiInRect(e.x, e.y, ENA_X, ENA_Y, ENA_W, ENA_H)) {
      { ModelLock lk; model.wifiEnabled = !model.wifiEnabled; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (uiInRect(e.x, e.y, MODE_AP_X, MODE_Y, MODE_W, MODE_H)) {
      { ModelLock lk; model.wifiMode = WifiMode::AP; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (uiInRect(e.x, e.y, MODE_CL_X, MODE_Y, MODE_W, MODE_H)) {
      { ModelLock lk; model.wifiMode = WifiMode::Client; model.revision++; }
      Storage::save(); hapticBuzz(50, 60); return;
    }
    if (uiInRect(e.x, e.y, KN_X, KN_Y, KN_W, KN_H)) {
      switchTo(Screen::SettingsKnownNets); return;
    }
  }
private:
  struct Snap {
    bool     en; WifiMode mode; bool conn;
    int8_t   rssi; uint8_t cli; bool rtcOk;
    uint32_t ip;
    char     ssid[33];
  } last{false, WifiMode::AP, false, 0, 0, false, 0, ""};
  bool firstDraw = true;

  static const int16_t ENA_X = 20, ENA_Y = 60,  ENA_W = 200, ENA_H = 40;
  static const int16_t MODE_Y = 116, MODE_W = 96, MODE_H = 40;
  static const int16_t MODE_AP_X = 20;
  static const int16_t MODE_CL_X = 240 - 20 - MODE_W;     // 124
  static const int16_t STAT_Y = 168, STAT_H = 60;
  static const int16_t KN_X = 20, KN_Y = 236, KN_W = 200, KN_H = 36;

  void drawEnableBtn(bool on) {
    uint16_t bg = on ? DARKGREEN : DARKGREY;
    gfx->fillRoundRect(ENA_X, ENA_Y, ENA_W, ENA_H, 8, bg);
    gfx->drawRoundRect(ENA_X, ENA_Y, ENA_W, ENA_H, 8, WHITE);
    gfx->setTextColor(WHITE, bg);
    gfx->setTextSize(2);
    const char *l = on ? "WiFi: ON" : "WiFi: OFF";
    int16_t lw = (int16_t)strlen(l) * 12;
    gfx->setCursor(ENA_X + (ENA_W - lw) / 2, ENA_Y + 12);
    gfx->print(l);
  }
  void drawModeBtn(int16_t x, const char *label, bool active) {
    uint16_t bg = active ? PURPLE : DARKGREY;
    gfx->fillRoundRect(x, MODE_Y, MODE_W, MODE_H, 8, bg);
    gfx->drawRoundRect(x, MODE_Y, MODE_W, MODE_H, 8, WHITE);
    gfx->setTextColor(WHITE, bg);
    gfx->setTextSize(2);
    int16_t lw = (int16_t)strlen(label) * 12;
    gfx->setCursor(x + (MODE_W - lw) / 2, MODE_Y + 12);
    gfx->print(label);
  }
  void drawModeBtns(WifiMode m) {
    drawModeBtn(MODE_AP_X, "Host",   m == WifiMode::AP);
    drawModeBtn(MODE_CL_X, "Client", m == WifiMode::Client);
  }
  void drawStatus(const Snap &s) {
    gfx->fillRect(0, STAT_Y, W, STAT_H, BLACK);
    gfx->setTextSize(1);
    if (!s.en) {
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(20, STAT_Y + 12); gfx->print("Radio off");
      return;
    }
    char buf[80];
    if (s.mode == WifiMode::AP) {
      gfx->setTextColor(YELLOW, BLACK);
      gfx->setCursor(12, STAT_Y);
      snprintf(buf, sizeof(buf), "AP: %s", s.ssid[0] ? s.ssid : "EWATCH_SETUP");
      gfx->print(buf);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
      gfx->setCursor(12, STAT_Y + 14);
      gfx->print("http://192.168.4.1/");
      gfx->setCursor(12, STAT_Y + 28);
      snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)s.cli);
      gfx->print(buf);
    } else {
      if (s.conn) {
        gfx->setTextColor(GREEN, BLACK);
        gfx->setCursor(12, STAT_Y);
        snprintf(buf, sizeof(buf), "Connected: %s", s.ssid);
        gfx->print(buf);
        { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
        gfx->setCursor(12, STAT_Y + 14);
        if (s.ip) {
          snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u/",
                   (unsigned)(s.ip       & 0xFF), (unsigned)((s.ip >>  8) & 0xFF),
                   (unsigned)((s.ip >> 16) & 0xFF), (unsigned)((s.ip >> 24) & 0xFF));
          gfx->print(buf);
        }
        gfx->setCursor(12, STAT_Y + 28);
        snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)s.rssi);
        gfx->print(buf);
      } else {
        gfx->setTextColor(ORANGE, BLACK);
        gfx->setCursor(12, STAT_Y); gfx->print("Scanning for saved SSIDs...");
        if (Storage::knownCount() == 0) {
          gfx->setTextColor(0x8410, BLACK);
          gfx->setCursor(12, STAT_Y + 16);
          gfx->print("(no networks saved yet)");
        }
      }
    }
  }
  void drawKnownBtn() {
    { ThemeColors _t = theme(); gfx->fillRoundRect(KN_X, KN_Y, KN_W, KN_H, 6, _t.accent); }
    gfx->drawRoundRect(KN_X, KN_Y, KN_W, KN_H, 6, WHITE);
    { ThemeColors _t = theme(); gfx->setTextColor(contrastFor(_t.accent), _t.accent); }
    gfx->setTextSize(2);
    const char *l = "Saved networks >";
    int16_t lw = (int16_t)strlen(l) * 12;
    gfx->setCursor(KN_X + (KN_W - lw) / 2, KN_Y + 10);
    gfx->print(l);
  }
};

// =====================================================================
// SettingsKnownNets — list of saved SSIDs with per-row delete. Adding new
// networks happens via the AP web form (typing passwords on the touchscreen
// would be miserable).
// =====================================================================
class SettingsKnownNetsView : public View {
public:
  void onEnter() override { uiClearAll(); firstDraw = true; lastN = 0xFF; }
  void render() override {
    if (!gfx) return;
    if (firstDraw) {
      drawTitleBar("Networks");
      firstDraw = false;
    }
    uint8_t n = Storage::knownCount();
    if (n != lastN) { drawList(); lastN = n; }
  }
  void onEvent(const Event &e) override {
    if (e.type == EventType::ButtonShort) { switchTo(Screen::SettingsWifi); return; }
    if (e.type != EventType::Touch) return;
    if (tappedBack(e.x, e.y)) { switchTo(Screen::SettingsWifi); return; }

    uint8_t n = Storage::knownCount();
    for (uint8_t i = 0; i < n; i++) {
      int16_t y = ROW_Y0 + i * ROW_H;
      if (uiInRect(e.x, e.y, DEL_X, y, DEL_W, ROW_H - 4)) {
        Storage::knownRemove(i);
        Storage::knownSave();
        hapticBuzz(80, 80);
        lastN = 0xFF;        // force list redraw
        return;
      }
    }
  }
private:
  bool   firstDraw = true;
  uint8_t lastN = 0xFF;
  static const int16_t ROW_Y0 = 56;
  static const int16_t ROW_H  = 26;
  static const int16_t DEL_X  = 188;
  static const int16_t DEL_W  = 44;

  void drawList() {
    gfx->fillRect(0, ROW_Y0 - 2, W, H - ROW_Y0 - 2, BLACK);
    uint8_t n = Storage::knownCount();
    if (n == 0) {
      gfx->setTextSize(2);
      gfx->setTextColor(DARKGREY, BLACK);
      gfx->setCursor(28, 110); gfx->print("(none saved)");
      gfx->setTextSize(1);
      gfx->setTextColor(0x8410, BLACK);
      gfx->setCursor(12, 200); gfx->print("Add via Web Setup AP:");
      gfx->setCursor(12, 214); gfx->print("Settings -> WiFi -> Host,");
      gfx->setCursor(12, 228); gfx->print("join EWATCH_SETUP, open page.");
      return;
    }
    for (uint8_t i = 0; i < n && i < Storage::KNOWN_MAX; i++) {
      char s[Storage::KNOWN_SSID], p[Storage::KNOWN_PASS];
      if (!Storage::knownAt(i, s, p)) continue;
      int16_t y = ROW_Y0 + i * ROW_H;
      gfx->setTextSize(1);
      { ThemeColors _t = theme(); gfx->setTextColor(_t.fg, _t.bg); }
      gfx->setCursor(12, y + 7);
      char trunc[29];
      strncpy(trunc, s, 28); trunc[28] = '\0';
      gfx->print(trunc);
      gfx->fillRoundRect(DEL_X, y, DEL_W, ROW_H - 4, 4, MAROON);
      gfx->drawRoundRect(DEL_X, y, DEL_W, ROW_H - 4, 4, WHITE);
      gfx->setTextColor(WHITE, MAROON);
      gfx->setTextSize(2);
      gfx->setCursor(DEL_X + (DEL_W - 12) / 2, y + 5);
      gfx->print('X');
    }
  }
};

static SettingsWifiView      sWifi;
static SettingsKnownNetsView sKnownNets;
View *settingsWifiViewPtr()      { return &sWifi; }
View *settingsKnownNetsViewPtr() { return &sKnownNets; }

#endif  // EWATCH_ENABLE_WIFI
