#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_wifi.h>
#include "pins.h"
#include "wifi_mod.h"

static String   apSsidSaved;
static String   apPassSaved;
static uint32_t apLastCheck = 0;

static WebServer server(80);
static DNSServer dns;
static String  pendingMsg;
static bool    haveNewMsg = false;

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static bool rtcSetTime(uint8_t h, uint8_t m, uint8_t s) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);                              // seconds reg
  Wire.write(dec2bcd(s) & 0x7F);
  Wire.write(dec2bcd(m) & 0x7F);
  Wire.write(dec2bcd(h) & 0x3F);                 // 24h
  return Wire.endTransmission() == 0;
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>EWatch</title>
<style>body{font-family:sans-serif;max-width:380px;margin:2em auto;padding:0 1em}
input,button{font-size:1.1em;padding:.5em;width:100%;box-sizing:border-box;margin:.3em 0}
h2{margin-top:1.5em}</style>
<h1>EWatch</h1>
<form method=POST action=/msg>
<h2>Message</h2>
<input name=text maxlength=120 placeholder="text to show on watch" autofocus>
<button>Send</button></form>
<form method=POST action=/time>
<h2>Set time (24h)</h2>
<input name=t type=time step=1 required>
<button>Set</button></form>
)HTML";

static void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

static void handleMsg() {
  String t = server.arg("text");
  if (t.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  if (t.length() > 120) t = t.substring(0, 120);
  pendingMsg = t;
  haveNewMsg = true;
  Serial.printf("AP msg : \"%s\"\n", t.c_str());
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleTime() {
  String t = server.arg("t");                    // "HH:MM" or "HH:MM:SS"
  if (t.length() < 5) { server.send(400, "text/plain", "bad time"); return; }
  int h = t.substring(0, 2).toInt();
  int m = t.substring(3, 5).toInt();
  int s = (t.length() >= 8) ? t.substring(6, 8).toInt() : 0;
  if (h > 23 || m > 59 || s > 59) { server.send(400, "text/plain", "range"); return; }
  bool ok = rtcSetTime(h, m, s);
  Serial.printf("AP time: %02d:%02d:%02d %s\n", h, m, s, ok ? "OK" : "FAIL");
  server.sendHeader("Location", "/");
  server.send(ok ? 303 : 500, "text/plain", ok ? "" : "rtc write failed");
}

void wifiBegin() {
  WiFi.onEvent(onWifiEvent);               // catch STA disconnect reason codes
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);             // clear any stored creds, drop AP
  delay(50);
  Serial.printf("WiFi   : STA mode, MAC=%s\n", WiFi.macAddress().c_str());
}

int wifiScan() {
  Serial.println("WiFi   : scanning...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n < 0) {
    Serial.printf("WiFi   : scan failed (%d)\n", n);
    return n;
  }
  Serial.printf("WiFi   : %d AP(s)\n", n);
  for (int i = 0; i < n && i < 10; i++) {
    Serial.printf("  %2d  %-32s  ch%2d  %4d dBm  %s\n",
                  i,
                  WiFi.SSID(i).c_str(),
                  WiFi.channel(i),
                  WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  }
  return n;
}

String wifiTopSSID(int8_t &rssiOut) {
  int n = WiFi.scanComplete();
  if (n <= 0) { rssiOut = 0; return String(); }
  int best = 0;
  for (int i = 1; i < n; i++) {
    if (WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  rssiOut = WiFi.RSSI(best);
  return WiFi.SSID(best);
}

bool wifiConnect(const char *ssid, const char *pass, uint32_t timeoutMs) {
  Serial.printf("WiFi   : connecting to \"%s\" (pw len=%u)...\n",
                ssid, (unsigned)(pass ? strlen(pass) : 0));
  WiFi.begin(ssid, pass);

  uint32_t t0 = millis();
  uint8_t lastStatus = 255;
  while (millis() - t0 < timeoutMs) {
    uint8_t s = WiFi.status();
    if (s != lastStatus) {
      Serial.printf("  t=%4lums  status=%u\n", (unsigned long)(millis() - t0), s);
      lastStatus = s;
    }
    if (s == WL_CONNECTED) break;
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi   : connected, IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("WiFi   : connect failed (status=%d)\n", WiFi.status());
  return false;
}

void wifiDisconnect()    { WiFi.disconnect(true, true); }
bool wifiIsConnected()   { return WiFi.status() == WL_CONNECTED; }
String wifiLocalIP()     { return WiFi.localIP().toString(); }

static const char *staDisconnectReason(uint8_t r) {
  switch (r) {
    case  1: return "UNSPECIFIED";
    case  2: return "AUTH_EXPIRE";
    case  3: return "AUTH_LEAVE";
    case  4: return "ASSOC_EXPIRE";
    case  5: return "ASSOC_TOOMANY";
    case  6: return "NOT_AUTHED";
    case  7: return "NOT_ASSOCED";
    case  8: return "ASSOC_LEAVE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";   // wrong password
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";               // wrong password (alt)
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "?";
  }
}

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("AP evt : START"); break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("AP evt : STOP"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("AP evt : client connected (now %u)\n",
                    WiFi.softAPgetStationNum());
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("AP evt : client disconnected (now %u)\n",
                    WiFi.softAPgetStationNum());
      break;
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("STA evt: START"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("STA evt: CONNECTED ssid=%s ch=%u\n",
                    (char*)info.wifi_sta_connected.ssid,
                    info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t r = info.wifi_sta_disconnected.reason;
      Serial.printf("STA evt: DISCONNECTED reason=%u (%s)\n",
                    r, staDisconnectReason(r));
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("STA evt: GOT_IP %s\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    default: break;
  }
}

void apBegin(const char *ssid, const char *pass) {
  apSsidSaved = ssid ? ssid : "";
  apPassSaved = pass ? pass : "";

  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  // Lock IP so DHCP advertises a coherent gateway; some Android stacks reject
  // leases without one.
  WiFi.softAPConfig(IPAddress(192,168,4,1),
                    IPAddress(192,168,4,1),
                    IPAddress(255,255,255,0));

  // Explicit channel + max_connection. Channel 6 is the most-used hint band
  // for handheld scanners; auto-channel sometimes lands on 13/14 depending
  // on country code, which Android ignores.
  bool ok = (pass && strlen(pass) >= 8)
              ? WiFi.softAP(ssid, pass, /*ch=*/6, /*hidden=*/0, /*max_conn=*/4)
              : WiFi.softAP(ssid, nullptr, 6, 0, 4);

  // Settle: arduino-esp32 v2.x fires an internal STOP/START during DHCP-server
  // bring-up. If the next radio call lands inside that window the AP can come
  // back up in a half-state where it doesn't beacon. Wait it out.
  delay(500);

  Serial.printf("AP     : \"%s\" %s  IP=%s  ch=%d  MAC=%s\n",
                ssid, ok ? "up" : "FAIL",
                WiFi.softAPIP().toString().c_str(),
                WiFi.channel(),
                WiFi.softAPmacAddress().c_str());

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/msg",  HTTP_POST, handleMsg);
  server.on("/time", HTTP_POST, handleTime);

  // --- Captive-portal probes. Phones probe these *immediately* after associating;
  // if they don't get the expected response they mark the AP "no internet" and
  // silently disconnect — looks exactly like "connecting... then exits".
  server.on("/generate_204", [] { server.send(204); });                  // Android
  server.on("/gen_204",      [] { server.send(204); });
  server.on("/hotspot-detect.html", [] {                                 // iOS / macOS
    server.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/library/test/success.html", [] {
    server.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/connecttest.txt", [] { server.send(200, "text/plain", "Microsoft Connect Test"); });
  server.on("/ncsi.txt",        [] { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound([] {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
  });
  server.begin();

  // Catch-all DNS so any hostname (including phone probe domains) resolves to us.
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());

  Serial.println("AP     : http://192.168.4.1/  (captive portal active)");
}

void apHandle() {
  dns.processNextRequest();
  server.handleClient();

  // Self-heal: if the AP interface dropped (brownout, mode flip, fw glitch),
  // bring it back up. Throttled so we don't thrash the radio.
  if (millis() - apLastCheck >= 2000) {
    apLastCheck = millis();
    if (!apIsUp() && apSsidSaved.length()) {
      Serial.println("AP     : down, restarting...");
      WiFi.softAPdisconnect(true);
      delay(50);
      apBegin(apSsidSaved.c_str(),
              apPassSaved.length() ? apPassSaved.c_str() : nullptr);
    }
  }
}
String apIP()   { return WiFi.softAPIP().toString(); }

bool apIsUp() {
  return (WiFi.getMode() & WIFI_AP) && WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
}

uint8_t apClientCount() { return WiFi.softAPgetStationNum(); }

bool apTakeMessage(String &out) {
  if (!haveNewMsg) return false;
  out = pendingMsg;
  haveNewMsg = false;
  return true;
}
