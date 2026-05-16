// Background WiFi service. See wifi_svc.h.
//
// Entire implementation gated on EWATCH_ENABLE_WIFI so a build without WiFi
// doesn't pull WiFi.h / WebServer.h / DNSServer.h (and the IDF libraries
// they bring) into the link — shrinks the firmware materially.
#include "wifi_svc.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

// The single task converges the actual radio toward the model's desired
// (enabled, mode) state. Mode flips trigger a teardown + bring-up. In AP
// mode the WebServer + DNS captive portal are owned here; the previous
// WebSetup app's HTML form has migrated in with an added Known-Networks
// editor. In client mode we periodically scan + connect to the strongest
// known SSID; if the connection drops we fall back into scanning.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>
#include "model.h"
#include "storage.h"
#include "display.h"          // backlightSet for the brightness slider
#include "controller.h"       // requestSetRTC

// ---------- desired vs actual state ----------
enum class RadioState : uint8_t { Off, AP, Scanning, Connecting, Connected };
static RadioState         actual    = RadioState::Off;
static uint32_t           connStart = 0;
static uint32_t           scanStart = 0;
static uint32_t           lastScanDoneMs   = 0;
static uint32_t           lastScanLaunchMs = 0;
static bool               scanInFlight     = false;
static bool               scanRequested    = false;

// Don't pound the WiFi stack with back-to-back scans. The ESP32 v2 driver's
// AP-list processing runs asynchronously on `ppTask`, and tearing down the
// previous scan result while that task is mid-iterate has been observed to
// crash in wifi_get_ap_list_process (LoadProhibited @ 0x000f000f). A 5 s
// floor gives the internal task time to fully settle between cycles.
static const uint32_t SCAN_INTERVAL_MS   = 5000;
static const uint32_t SCAN_SETTLE_MS     = 300;     // wait after disconnect / mode flip

// ---------- AP / web server ----------
static const char     AP_SSID[]  = "EWATCH_SETUP";
static const int      AP_CHAN    = 6;
static const uint8_t  DNS_PORT   = 53;
static WebServer      server(80);
static DNSServer      dns;
static bool           serverUp = false;
static uint32_t       saveCount = 0;

// ---------- last scan cache (for the web form's SSID picker) ----------
struct ScanEntry { char ssid[33]; int8_t rssi; uint8_t chan; uint8_t enc; };
static constexpr int  SCAN_MAX = 16;
static ScanEntry      scanCache[SCAN_MAX];
static uint8_t        scanCacheN = 0;
static SemaphoreHandle_t scanMutex = nullptr;

static void scanCachePopulate() {
  int n = WiFi.scanComplete();
  if (n < 0) return;
  if (xSemaphoreTake(scanMutex, portMAX_DELAY) != pdTRUE) return;
  scanCacheN = 0;
  for (int i = 0; i < n && scanCacheN < SCAN_MAX; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    strncpy(scanCache[scanCacheN].ssid, s.c_str(), 32);
    scanCache[scanCacheN].ssid[32] = '\0';
    scanCache[scanCacheN].rssi = WiFi.RSSI(i);
    scanCache[scanCacheN].chan = WiFi.channel(i);
    scanCache[scanCacheN].enc  = (uint8_t)WiFi.encryptionType(i);
    scanCacheN++;
  }
  xSemaphoreGive(scanMutex);
}

// ---------- color helpers (rgb565 <-> #rrggbb) ----------
static uint16_t rgb565From888(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) |
         ((uint16_t)(g & 0xFC) << 3) |
          (uint16_t)(b >> 3);
}
static String rgb565ToHex(uint16_t c) {
  uint8_t r5 = (c >> 11) & 0x1F;
  uint8_t g6 = (c >>  5) & 0x3F;
  uint8_t b5 = (c      ) & 0x1F;
  uint8_t r = (r5 << 3) | (r5 >> 2);
  uint8_t g = (g6 << 2) | (g6 >> 4);
  uint8_t b = (b5 << 3) | (b5 >> 2);
  char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}
static bool parseHexColor(const String &s, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s.length() != 7 || s[0] != '#') return false;
  long v = strtol(s.c_str() + 1, nullptr, 16);
  r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
  return true;
}

// HTML-escape for SSID / labels.
static String esc(const String &in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'':out += "&#39;";  break;
      default:  out += c;
    }
  }
  return out;
}

// ---------- HTML form ----------
static String htmlPage() {
  bool     wt, wb, wi, wifiOn;
  uint16_t to, off, bg, fg, yr;
  uint8_t  th, br, hh, mm, ss, wd, dy, mo, apClients;
  WifiMode wMode;
  bool     rtcOk, wConn;
  int8_t   rssi;
  char     curSsid[33];
  { ModelLock lk;
    wt  = model.wakeOnTouch;     wb  = model.wakeOnButton;  wi  = model.wakeOnImu;
    to  = model.sleepTimeoutSec; off = model.sleepToOffSec;
    th  = model.imuWakeThreshold; br = model.brightness;
    bg  = model.bgColor;         fg  = model.fgColor;
    hh  = model.hour; mm = model.minute; ss = model.second;
    wd  = model.weekday; dy = model.day; mo = model.month; yr = model.year;
    rtcOk = model.rtcOk;
    wifiOn = model.wifiEnabled; wMode = model.wifiMode;
    wConn = model.wifiConnected; rssi = model.wifiRssi;
    strncpy(curSsid, model.wifiSsid, 32); curSsid[32] = '\0';
    apClients = model.wifiApClients;
  }
  String bgHex = rgb565ToHex(bg);
  String fgHex = rgb565ToHex(fg);

  String h; h.reserve(8192);
  h += F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>EWatch</title><style>"
         "body{font-family:-apple-system,sans-serif;max-width:460px;margin:1em auto;"
         "padding:0 1em;background:#111;color:#eee}"
         "h1{font-weight:300;margin:.2em 0}"
         "label{display:block;margin:.6em 0 .2em;font-size:.9em;color:#9bd}"
         "input,select{font-size:1em;padding:.45em;width:100%;box-sizing:border-box;"
         "background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
         "input[type=color]{height:42px;padding:2px}"
         "input[type=range]{padding:0}"
         "input[type=checkbox],input[type=radio]{width:auto;margin-right:.4em}"
         ".row{display:flex;gap:.6em}.row>*{flex:1}"
         "button{margin-top:1em;padding:.7em;font-size:1.05em;border:0;border-radius:8px;"
         "background:#0a7;color:#fff;cursor:pointer;width:100%}"
         "button.warn{background:#933;margin-top:.3em}"
         "fieldset{border:1px solid #333;border-radius:8px;padding:.6em 1em;margin:1em 0}"
         "legend{padding:0 .4em;color:#9bd}"
         ".chk{display:flex;align-items:center;margin:.4em 0}"
         ".muted{color:#888;font-size:.85em}"
         ".net{display:flex;align-items:center;gap:.5em;padding:.4em 0;border-bottom:1px solid #222}"
         ".net .ssid{flex:1;font-family:monospace;font-size:.9em;word-break:break-all}"
         ".net form{margin:0}"
         ".net button{width:auto;margin:0;padding:.3em .6em;font-size:.85em}"
         "</style>"
         "<h1>EWatch</h1>");
  h += "<p class=muted>Saves: "; h += saveCount;
  h += " &middot; AP clients: "; h += apClients;
  if (wifiOn && wMode == WifiMode::Client) {
    h += " &middot; STA: ";
    if (wConn) { h += esc(curSsid); h += " ("; h += rssi; h += " dBm)"; }
    else h += "scanning";
  }
  h += "</p>";

  // ===== WiFi section first so it's prominent =====
  h += "<form method=POST action=/wifi>";
  h += F("<fieldset><legend>WiFi</legend>"
         "<div class=chk><input type=checkbox id=wfOn name=wifiOn");
  if (wifiOn) h += " checked";
  h += F("><label for=wfOn>WiFi enabled</label></div>"
         "<label>Mode</label>"
         "<div class=chk><input type=radio id=wmAP name=wifiMode value=0");
  if (wMode == WifiMode::AP) h += " checked";
  h += F("><label for=wmAP>Host (AP) — this page</label></div>"
         "<div class=chk><input type=radio id=wmCl name=wifiMode value=1");
  if (wMode == WifiMode::Client) h += " checked";
  h += F("><label for=wmCl>Client — connect to a saved network</label></div>"
         "<button>Apply WiFi mode</button>"
         "</fieldset></form>");

  // ===== Known networks (add + list) =====
  h += F("<fieldset><legend>Known networks</legend>");
  uint8_t kn = Storage::knownCount();
  if (kn == 0) {
    h += "<p class=muted>No saved networks yet.</p>";
  } else {
    for (uint8_t i = 0; i < kn; i++) {
      char s[Storage::KNOWN_SSID], p[Storage::KNOWN_PASS];
      if (!Storage::knownAt(i, s, p)) continue;
      h += "<div class=net>";
      h += "<span class=ssid>"; h += esc(String(s)); h += "</span>";
      h += "<form method=POST action=/known/del>";
      h += "<input type=hidden name=i value="; h += i; h += ">";
      h += "<button class=warn>Delete</button></form>";
      h += "</div>";
    }
  }
  h += F("<form method=POST action=/known/add>"
         "<label>Pick from last scan</label>"
         "<select name=pick onchange=\"document.getElementById('ssidIn').value=this.value\">"
         "<option value=''>(or type below)</option>");
  if (xSemaphoreTake(scanMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (uint8_t i = 0; i < scanCacheN; i++) {
      h += "<option value=\""; h += esc(scanCache[i].ssid); h += "\">";
      h += esc(scanCache[i].ssid); h += "  ("; h += scanCache[i].rssi; h += " dBm)";
      h += "</option>";
    }
    xSemaphoreGive(scanMutex);
  }
  h += F("</select>"
         "<label>SSID</label>"
         "<input id=ssidIn name=ssid maxlength=32>"
         "<label>Password (blank = open)</label>"
         "<input name=pass type=text maxlength=64 autocomplete=off>"
         "<button>Add / update</button></form>");
  h += "<form method=POST action=/known/scan style='margin-top:.4em'><button class=warn style='background:#456'>Rescan</button></form>";
  h += "</fieldset>";

  // ===== Display + Sleep + RTC settings =====
  h += "<form method=POST action=/save>";

  h += F("<fieldset><legend>Display</legend>"
         "<label>Brightness <span id=brv>");
  h += br;
  h += F("</span> / 255</label>"
         "<input type=range name=bright min=0 max=255 value=");
  h += br;
  h += F(" oninput=\"document.getElementById('brv').textContent=this.value\">"
         "<div class=row>"
         "<div><label>Background</label><input type=color name=bg value=");
  h += bgHex;
  h += F("></div><div><label>Foreground</label><input type=color name=fg value=");
  h += fgHex;
  h += F("></div></div></fieldset>");

  h += F("<fieldset><legend>Sleep &amp; Wake</legend>"
         "<label>Auto-sleep after (sec, 0 = never)</label>"
         "<input type=number name=sleepSec min=0 max=3600 value=");
  h += to;
  h += F("><label>Then power off after (sec, 0 = never)</label>"
         "<input type=number name=sleepOff min=0 max=3600 value=");
  h += off;
  h += F("><label>IMU wake threshold (0..255, ~0.063 g/LSB)</label>"
         "<input type=number name=imuThresh min=0 max=255 value=");
  h += th;
  h += F("><div class=chk><input type=checkbox id=wt name=wkTouch");
  if (wt) h += " checked";
  h += F("><label for=wt>Wake on touch</label></div>"
         "<div class=chk><input type=checkbox id=wb name=wkButton");
  if (wb) h += " checked";
  h += F("><label for=wb>Wake on button</label></div>"
         "<div class=chk><input type=checkbox id=wif name=wkImu");
  if (wi) h += " checked";
  h += F("><label for=wif>Wake on motion (IMU)</label></div></fieldset>");

  h += F("<fieldset><legend>Time &amp; Date</legend>");
  if (!rtcOk) h += F("<p style='color:#f93'>(RTC not detected — writes may fail)</p>");
  char hms[12]; snprintf(hms, sizeof(hms), "%02u:%02u:%02u", hh, mm, ss);
  char ymd[12]; snprintf(ymd, sizeof(ymd), "%04u-%02u-%02u", yr, mo, dy);
  h += F("<label>Time</label><input type=time name=time step=1 value=");
  h += hms;
  h += F("><label>Date</label><input type=date name=date value=");
  h += ymd;
  h += F("><label>Weekday</label><select name=wday>");
  const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  for (int i = 0; i < 7; i++) {
    h += "<option value="; h += i;
    if (i == wd) h += " selected";
    h += '>'; h += i; h += " - "; h += days[i]; h += "</option>";
  }
  h += F("</select></fieldset>"
         "<button>Save settings</button></form>"
         "<p class=muted>Open AP \"");
  h += AP_SSID;
  h += F("\". Settings persist to NVS.</p>");
  return h;
}

// ---------- request handlers ----------
static void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

static void handleSave() {
  uint8_t  br  = (uint8_t)  constrain(server.arg("bright").toInt(),    0, 255);
  uint16_t to  = (uint16_t) constrain(server.arg("sleepSec").toInt(),  0, 3600);
  uint16_t off = (uint16_t) constrain(server.arg("sleepOff").toInt(),  0, 3600);
  uint8_t  th  = (uint8_t)  constrain(server.arg("imuThresh").toInt(), 0, 255);
  bool wt = server.hasArg("wkTouch");
  bool wb = server.hasArg("wkButton");
  bool wi = server.hasArg("wkImu");

  uint16_t bg = 0x0000, fg = 0xFFFF;
  uint8_t r, g, b;
  if (parseHexColor(server.arg("bg"), r, g, b)) bg = rgb565From888(r, g, b);
  if (parseHexColor(server.arg("fg"), r, g, b)) fg = rgb565From888(r, g, b);

  { ModelLock lk;
    model.brightness       = br;
    model.bgColor          = bg;
    model.fgColor          = fg;
    model.sleepTimeoutSec  = to;
    model.sleepToOffSec    = off;
    model.imuWakeThreshold = th;
    model.wakeOnTouch      = wt;
    model.wakeOnButton     = wb;
    model.wakeOnImu        = wi;
    model.revision++;
  }
  Storage::save();
  backlightSet(br);

  String tStr = server.arg("time");
  String dStr = server.arg("date");
  int wd = constrain(server.arg("wday").toInt(), 0, 6);
  if (tStr.length() >= 5 && dStr.length() >= 10) {
    int hh = tStr.substring(0, 2).toInt();
    int mm = tStr.substring(3, 5).toInt();
    int ss = (tStr.length() >= 8) ? tStr.substring(6, 8).toInt() : 0;
    int yr = dStr.substring(0, 4).toInt();
    int mo = dStr.substring(5, 7).toInt();
    int dy = dStr.substring(8, 10).toInt();
    requestSetRTC(hh, mm, ss, wd, dy, mo, yr);
  }

  saveCount++;
  Serial.printf("WEB: save#%lu br=%u sleep=%u/%u thr=%u bg=%04x fg=%04x\n",
                (unsigned long)saveCount, br, to, off, th, bg, fg);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleWifi() {
  bool en = server.hasArg("wifiOn");
  WifiMode m = (server.arg("wifiMode").toInt() == 1)
                 ? WifiMode::Client : WifiMode::AP;
  { ModelLock lk;
    model.wifiEnabled = en;
    model.wifiMode    = m;
    model.revision++; }
  Storage::save();
  Serial.printf("WEB: wifi en=%d mode=%s\n", en, m == WifiMode::AP ? "AP" : "Client");
  // Note: switching to client mode will tear down the AP, killing this
  // connection. Send the response first; the converger picks the switch up
  // on its next poll.
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleKnownAdd() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  bool ok = Storage::knownUpsert(ssid.c_str(), pass.c_str());
  if (ok) Storage::knownSave();
  Serial.printf("WEB: known add \"%s\" ok=%d\n", ssid.c_str(), ok);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleKnownDel() {
  uint8_t i = (uint8_t)constrain(server.arg("i").toInt(), 0, 255);
  bool ok = Storage::knownRemove(i);
  if (ok) Storage::knownSave();
  Serial.printf("WEB: known del %u ok=%d\n", i, ok);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleKnownScan() {
  wifiSvcRequestScan();
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleNotFound() {
  String loc = String("http://") + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", loc, true);
  server.send(302, "text/plain", loc);
}

// ---------- AP lifecycle ----------
static void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, nullptr, AP_CHAN);
  delay(50);
  IPAddress ip = WiFi.softAPIP();
  dns.start(DNS_PORT, "*", ip);
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/wifi",        HTTP_POST, handleWifi);
  server.on("/known/add",   HTTP_POST, handleKnownAdd);
  server.on("/known/del",   HTTP_POST, handleKnownDel);
  server.on("/known/scan",  HTTP_POST, handleKnownScan);
  server.onNotFound(handleNotFound);
  server.begin();
  serverUp = true;
  { ModelLock lk;
    strncpy(model.wifiSsid, AP_SSID, 32); model.wifiSsid[32] = '\0';
    model.wifiConnected = false; model.wifiRssi = 0;
    model.revision++; }
  Serial.printf("WIFI: AP up ssid=%s url=http://%s/\n",
                AP_SSID, ip.toString().c_str());
}

static void stopAP() {
  if (serverUp) {
    server.stop();
    dns.stop();
    serverUp = false;
  }
  WiFi.softAPdisconnect(true);
}

// ---------- STA helpers ----------
// Wait for any in-flight scan / AP-list processing to finish before yanking
// state. Bounded so a wedged driver can't hang us forever.
static void waitForScanIdle(uint32_t maxMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < maxMs) {
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) return;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void startScanIfIdle() {
  if (scanInFlight) return;
  // Cooldown floor so we don't continuously scan when no known network is in
  // range. Without it the converger's 20 ms tick triggers a new scan the
  // instant the previous finishes, which is what caused the wifi_get_ap_list
  // crash under SPI/DMA load.
  if (lastScanLaunchMs != 0 && (millis() - lastScanLaunchMs) < SCAN_INTERVAL_MS) return;
  // Note: no explicit scanDelete() here. scanNetworks(true) replaces the
  // internal AP-list state itself; calling scanDelete while the WiFi
  // protocol task is still iterating the previous list is unsafe.
  WiFi.scanNetworks(true, false);
  scanInFlight    = true;
  scanStart       = millis();
  lastScanLaunchMs = millis();
}

// Pick the strongest network whose SSID is in the saved list. Returns true
// if we have a candidate (filled outSsid/outPass).
static bool pickStrongestKnown(char *outSsid, char *outPass) {
  int n = WiFi.scanComplete();
  if (n <= 0) return false;
  int bestIdx = -1; int8_t bestRssi = -128;
  char passBuf[Storage::KNOWN_PASS];
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    if (!Storage::knownLookup(s.c_str(), passBuf)) continue;
    int8_t r = WiFi.RSSI(i);
    if (r > bestRssi) { bestRssi = r; bestIdx = i; }
  }
  if (bestIdx < 0) return false;
  String s = WiFi.SSID(bestIdx);
  strncpy(outSsid, s.c_str(), 32); outSsid[32] = '\0';
  Storage::knownLookup(s.c_str(), outPass);
  return true;
}

// ---------- desired-state converger ----------
static void teardownToOff() {
  stopAP();
  if (scanInFlight) {
    waitForScanIdle(800);          // let ppTask finish AP-list processing
    scanInFlight = false;
  }
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  vTaskDelay(pdMS_TO_TICKS(SCAN_SETTLE_MS));
  WiFi.mode(WIFI_OFF);
  { ModelLock lk;
    model.wifiConnected = false; model.wifiRssi = 0;
    model.wifiSsid[0] = '\0'; model.wifiApClients = 0;
    model.revision++; }
  actual = RadioState::Off;
  lastScanLaunchMs = 0;             // allow first scan immediately on re-enable
}

static void converge() {
  bool     wantOn;
  WifiMode wantMode;
  { ModelLock lk; wantOn = model.wifiEnabled; wantMode = model.wifiMode; }

  // OFF requested.
  if (!wantOn) {
    if (actual != RadioState::Off) teardownToOff();
    return;
  }

  // AP requested.
  if (wantMode == WifiMode::AP) {
    if (actual != RadioState::AP) {
      if (actual != RadioState::Off) teardownToOff();
      startAP();
      actual = RadioState::AP;
    }
    return;
  }

  // Client requested.
  if (actual == RadioState::AP || actual == RadioState::Off) {
    // Switching from AP/off into STA: bring up STA mode and trigger a scan.
    stopAP();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    scanInFlight = false;
    actual = RadioState::Scanning;
    startScanIfIdle();
    return;
  }

  if (actual == RadioState::Scanning) {
    if (scanRequested) {
      // User asked for an immediate rescan (web form Rescan button). Honor
      // it by clearing the cooldown — but only if no scan is in flight, so
      // we don't tear an in-progress scan out from under ppTask.
      scanRequested = false;
      if (!scanInFlight) lastScanLaunchMs = 0;
    }
    startScanIfIdle();

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n < 0) {
      // Failed; cooldown will gate the next attempt.
      scanInFlight = false;
      return;
    }
    // Scan done — cache for the web form, then choose a target.
    scanCachePopulate();
    lastScanDoneMs = millis();
    char tSsid[33], tPass[Storage::KNOWN_PASS];
    if (pickStrongestKnown(tSsid, tPass)) {
      Serial.printf("WIFI: connecting to \"%s\"\n", tSsid);
      WiFi.begin(tSsid, tPass);
      connStart = millis();
      actual = RadioState::Connecting;
      scanInFlight = false;            // begin() consumes scan list itself
      { ModelLock lk;
        strncpy(model.wifiSsid, tSsid, 32); model.wifiSsid[32] = '\0';
        model.wifiConnected = false; model.wifiRssi = 0;
        model.revision++; }
    } else {
      // No known network in range. Don't scanDelete here — leave the list
      // alone for ppTask to fully process, and let the cooldown gate the
      // next launch. Cache stays valid for the web form's "pick from scan".
      scanInFlight = false;
      { ModelLock lk;
        model.wifiSsid[0] = '\0'; model.wifiConnected = false; model.wifiRssi = 0;
        model.revision++; }
    }
    return;
  }

  if (actual == RadioState::Connecting) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      actual = RadioState::Connected;
      { ModelLock lk;
        model.wifiConnected = true;
        model.wifiRssi = WiFi.RSSI();
        model.revision++; }
      Serial.printf("WIFI: connected, IP=%s RSSI=%d\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      return;
    }
    // Timeout — bounce back to scanning.
    if (millis() - connStart > 15000) {
      Serial.println("WIFI: connect timeout, rescanning");
      WiFi.disconnect(true, false);
      vTaskDelay(pdMS_TO_TICKS(SCAN_SETTLE_MS));
      actual = RadioState::Scanning;
      scanInFlight    = false;
      lastScanLaunchMs = 0;          // allow immediate rescan after failed connect
    }
    return;
  }

  if (actual == RadioState::Connected) {
    wl_status_t s = WiFi.status();
    if (s != WL_CONNECTED) {
      Serial.println("WIFI: disconnected, rescanning");
      actual = RadioState::Scanning;
      { ModelLock lk; model.wifiConnected = false; model.wifiRssi = 0;
        model.revision++; }
      scanInFlight    = false;
      lastScanLaunchMs = 0;          // allow immediate rescan on drop
      return;
    }
    // Periodically refresh RSSI in the model.
    static uint32_t lastRssiUpdate = 0;
    if (millis() - lastRssiUpdate > 2000) {
      lastRssiUpdate = millis();
      int8_t r = WiFi.RSSI();
      ModelLock lk; if (model.wifiRssi != r) { model.wifiRssi = r; model.revision++; }
    }
  }
}

// ---------- task loop ----------
static void taskWifi(void *) {
  for (;;) {
    converge();
    if (serverUp) {
      dns.processNextRequest();
      server.handleClient();
      uint8_t c = WiFi.softAPgetStationNum();
      ModelLock lk;
      if (model.wifiApClients != c) { model.wifiApClients = c; model.revision++; }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- public API ----------
void wifiSvcInit() {
  if (!scanMutex) scanMutex = xSemaphoreCreateMutex();
}

void wifiSvcStartTask() {
  xTaskCreatePinnedToCore(taskWifi, "wifi", 8192, nullptr, 4, nullptr, 0);
}

void wifiSvcRequestScan() {
  scanRequested = true;
}

#else  // EWATCH_ENABLE_WIFI == 0 — stubs so callers still link.

void wifiSvcInit() {}
void wifiSvcStartTask() {}
void wifiSvcRequestScan() {}

#endif  // EWATCH_ENABLE_WIFI
