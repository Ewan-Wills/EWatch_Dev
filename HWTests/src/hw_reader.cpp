// Hardware reader for EWatch v2 — standalone, serial-only test sketch.
// Combines the I2C bus reader, the WiFi signal meter and a battery readout.
//
// On boot: recovers a stuck I2C bus, scans it and names the known parts,
// characterises the battery divider (off / settle curve), then starts a WiFi
// scan and joins the strongest network saved by AuroraOS (NVS "ewatch" ->
// "known", read-only here). After that, twice a second, one line:
//   RTC time | accel XYZ in g | touch point | battery V/% | WiFi RSSI
//
// Serial commands (115200, end with Enter):
//   i              rescan the I2C bus
//   d              hex-dump registers 0x00..0x3F of every I2C device
//   b              battery divider check (off reading + settle curve)
//   w              WiFi scan, print the AP table (active: sends probe requests)
//   wp             passive WiFi scan (listen only, radio never transmits)
//   t <dBm>        set WiFi TX power cap (2..20, default WIFI_TX_DBM)
//   c              connect to strongest saved network / disconnect
//   n              list saved SSIDs (passwords are never printed)
//   j ssid,pass    join a network not saved on the watch (not stored)
//   j N,pass       join row N of the last scan table (avoids typing the SSID;
//                  '#N' works too)
//   x [N]          measure crystal error in ppm against row N's beacons
//   m b|bg|bgn     restrict the 802.11 modes used to join (default bgn)
//   s [MB]         speed test while connected: TCP latency, download and
//                  upload against speed.cloudflare.com over plain HTTP
//                  (default 2 MB each way), plus the link state
//   p              pause / resume the live readout

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <WiFiClient.h>
#include "pins.h"

// ======================================================================
// Battery
// ======================================================================
// V_BAT+ -> Q1 (P-FET, on while PIN_BAT_MON_EN is HIGH) -> R6 -> tap -> R7
// -> V_BAT-, with C1 100 nF across R7. Tap = VBAT * R7 / (R6 + R7).
//   V2.1: R6 = R7 = 100k        -> 0.500, tau = 50k  * 100n = 5.0 ms
//   V2.0: R6 = 30k, R7 = 100k   -> 0.769, tau = 23k  * 100n = 2.3 ms
// The ratio is selected per env in platformio.ini.
#ifndef BAT_DIV_RATIO
#define BAT_DIV_RATIO 0.5f
#endif
static const uint32_t BAT_SETTLE_MS = 30;      // ~6 tau on V2.1
static const uint32_t BAT_PERIOD_MS = 1000;

static uint32_t batAdcMv = 0, batLast = 0;
static float    batV = 0;
static bool     batOk = false;

static uint32_t adcAvgMv(int n) {
  uint32_t acc = 0;
  for (int i = 0; i < n; i++) acc += analogReadMilliVolts(PIN_BAT_MON_ADC);
  return acc / n;
}

// Same linear curve AuroraOS uses (3.30 V = 0 %, 4.20 V = 100 %) so the two
// can be compared directly.
static int batPct(float v) {
  float p = (v - 3.30f) * (100.0f / 0.90f);
  return (int)constrain(p + 0.5f, 0.0f, 100.0f);
}

static void batSample() {
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  delay(BAT_SETTLE_MS);
  batAdcMv = adcAvgMv(16);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  batOk = batAdcMv > 0;
  batV = batAdcMv / 1000.0f / BAT_DIV_RATIO;
}

// Proves the reading comes from the divider: with it switched off the tap
// should bleed to ~0 V through R7. Then shows how long C1 takes to charge.
static void batCheck() {
  Serial.printf("\n---- battery divider (GPIO%d ADC, GPIO%d EN, ratio %.3f) ----\n",
                PIN_BAT_MON_ADC, PIN_BAT_MON_EN, BAT_DIV_RATIO);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  delay(100);
  uint32_t off = adcAvgMv(16);
  Serial.printf("  EN low : %4lu mV at tap %s\n", (unsigned long)off,
                off < 100 ? "(ok, divider off)" : "(HIGH - EN not switching Q1, or tap not on this pin?)");

  static const uint8_t at[] = {0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 50};
  uint32_t mv[sizeof at];
  uint32_t t0 = millis();
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  for (size_t k = 0; k < sizeof at; k++) {
    while (millis() - t0 < at[k]) {}
    mv[k] = analogReadMilliVolts(PIN_BAT_MON_ADC);
  }
  digitalWrite(PIN_BAT_MON_EN, LOW);
  uint32_t fin = mv[sizeof at - 1] ? mv[sizeof at - 1] : 1;
  Serial.println("  EN high, settle curve:");
  for (size_t k = 0; k < sizeof at; k++)
    Serial.printf("    t=%2u ms  %4lu mV  %3lu%%  -> %.3f V\n", at[k], (unsigned long)mv[k],
                  (unsigned long)(mv[k] * 100 / fin), mv[k] / 1000.0f / BAT_DIV_RATIO);
  Serial.println("  (AuroraOS samples at t=2 ms; check that against the settled value)\n");
  batSample();
}

// ======================================================================
// I2C
// ======================================================================
static const uint8_t RV_SEC = 0x00, RV_STATUS = 0x0E, RV_ID = 0x28;
static const uint8_t MMA_STATUS = 0x00, MMA_WHOAMI = 0x0D, MMA_XYZ_CFG = 0x0E,
                     MMA_CTRL1 = 0x2A, MMA_ID = 0x1A;
static const uint8_t CST_GESTURE = 0x01, CST_CHIPID = 0xA7, CST_FWVER = 0xA9;

static uint8_t  mmaAddr = 0;          // whichever SA0 variant answers
static bool     present[128];
static uint32_t nack[128];

static bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) { nack[addr]++; return false; }
  if (Wire.requestFrom(addr, n) != n)   { nack[addr]++; return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  if (Wire.endTransmission() != 0) { nack[addr]++; return false; }
  return true;
}

static uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// A slave holding SDA low (e.g. reset mid-transfer) wedges the bus. Clock
// SCL up to 9 times until it lets go, then issue a STOP.
static void busRecover() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(PIN_I2C_SDA)) {
    Serial.println("bus    : SDA idle high, OK");
    return;
  }
  Serial.println("bus    : SDA stuck LOW, clocking SCL to release...");
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && !digitalRead(PIN_I2C_SDA); i++) {
    digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  }
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);        // STOP: SDA low->high, SCL high
  digitalWrite(PIN_I2C_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  Serial.printf("bus    : SDA now %s\n", digitalRead(PIN_I2C_SDA) ? "HIGH (released)" : "still LOW - hardware fault?");
}

static const char *nameOf(uint8_t a) {
  switch (a) {
    case I2C_ADDR_RV3028:    return "RV-3028 RTC";
    case I2C_ADDR_MMA8451_A: return "MMA8451 accel (SA0=0)";
    case I2C_ADDR_MMA8451_B: return "MMA8451 accel (SA0=1)";
    case I2C_ADDR_TOUCH:     return "CST816S touch";
    default:                 return "unknown";
  }
}

static void i2cScan() {
  Serial.println("\n---- I2C scan 0x08..0x77 ----");
  int found = 0;
  mmaAddr = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    present[a] = probe(a);
    if (!present[a]) continue;
    found++;
    Serial.printf("  0x%02X  %s", a, nameOf(a));
    uint8_t v[2];
    if (a == I2C_ADDR_RV3028 && readRegs(a, RV_ID, v, 1))
      Serial.printf("  ID=0x%02X (HID %u, VID %u)", v[0], v[0] >> 4, v[0] & 0x0F);
    if ((a == I2C_ADDR_MMA8451_A || a == I2C_ADDR_MMA8451_B) && readRegs(a, MMA_WHOAMI, v, 1)) {
      Serial.printf("  WHO_AM_I=0x%02X %s", v[0], v[0] == MMA_ID ? "(ok)" : "(UNEXPECTED, want 0x1A)");
      if (v[0] == MMA_ID) mmaAddr = a;
    }
    if (a == I2C_ADDR_TOUCH && readRegs(a, CST_CHIPID, v, 1)) {
      uint8_t fw = 0;
      readRegs(a, CST_FWVER, &fw, 1);
      Serial.printf("  chipID=0x%02X fw=0x%02X", v[0], fw);
    }
    Serial.println();
  }
  Serial.printf("  %d device(s)", found);
  const uint8_t expect[] = {I2C_ADDR_RV3028, I2C_ADDR_TOUCH};
  for (uint8_t a : expect)
    if (!present[a]) Serial.printf("  | MISSING 0x%02X %s", a, nameOf(a));
  if (!mmaAddr) Serial.print("  | MISSING MMA8451 (0x1C/0x1D)");
  Serial.println("\n  (CST816S NACKs while asleep - touch the screen and rescan with 'i')\n");

  if (mmaAddr) {
    // Standby -> +-2g range -> active @ 100 Hz (DR=011).
    writeReg(mmaAddr, MMA_CTRL1, 0x00);
    writeReg(mmaAddr, MMA_XYZ_CFG, 0x00);
    writeReg(mmaAddr, MMA_CTRL1, (0b011 << 3) | 0x01);
  }
}

static void i2cDump() {
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (!present[a]) continue;
    Serial.printf("\n---- 0x%02X %s ----\n", a, nameOf(a));
    for (uint8_t row = 0; row < 0x40; row += 16) {
      uint8_t buf[16];
      Serial.printf("  %02X:", row);
      if (!readRegs(a, row, buf, 16)) { Serial.println(" NACK"); continue; }
      for (uint8_t i = 0; i < 16; i++) Serial.printf(" %02X", buf[i]);
      Serial.println();
    }
  }
  Serial.println();
}

// Appends the I2C part of the live line.
static int i2cLive(char *line, int n, int cap) {
  uint8_t b[7];
  if (!present[I2C_ADDR_RV3028]) {
    n += snprintf(line + n, cap - n, "RTC absent             ");
  } else if (readRegs(I2C_ADDR_RV3028, RV_SEC, b, 7)) {
    n += snprintf(line + n, cap - n, "RTC 20%02u-%02u-%02u %02u:%02u:%02u",
                  bcd(b[6]), bcd(b[5] & 0x1F), bcd(b[4] & 0x3F),
                  bcd(b[2] & 0x3F), bcd(b[1] & 0x7F), bcd(b[0] & 0x7F));
    uint8_t st;
    if (readRegs(I2C_ADDR_RV3028, RV_STATUS, &st, 1) && (st & 0x01))
      n += snprintf(line + n, cap - n, " PORF");   // power-on reset flag: time invalid
  } else {
    n += snprintf(line + n, cap - n, "RTC --NACK--           ");
  }

  if (mmaAddr && readRegs(mmaAddr, MMA_STATUS, b, 7)) {
    // 14-bit left-justified, +-2g -> 4096 counts/g.
    float g[3];
    for (int i = 0; i < 3; i++) g[i] = (int16_t)((b[1 + 2 * i] << 8) | b[2 + 2 * i]) / 4.0f / 4096.0f;
    float mag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    n += snprintf(line + n, cap - n, " | ACC %+5.2f %+5.2f %+5.2f |g|%4.2f", g[0], g[1], g[2], mag);
  } else {
    n += snprintf(line + n, cap - n, " | ACC --NACK--");
  }

  // Touch sleeps between touches, so keep polling it only if it was found.
  if (present[I2C_ADDR_TOUCH] && readRegs(I2C_ADDR_TOUCH, CST_GESTURE, b, 6)) {
    uint16_t x = ((b[2] & 0x0F) << 8) | b[3];
    uint16_t y = ((b[4] & 0x0F) << 8) | b[5];
    if (b[1]) n += snprintf(line + n, cap - n, " | TCH (%3u,%3u)", x, y);
    else      n += snprintf(line + n, cap - n, " | TCH none     ");
  } else {
    n += snprintf(line + n, cap - n, " | TCH asleep   ");
  }
  return n;
}

// ======================================================================
// WiFi
// ======================================================================
// Mirrors Storage::KnownEntry in AuroraOS/src/core/storage.cpp.
struct KnownEntry { char ssid[33]; char pass[65]; };
static const uint8_t KNOWN_MAX = 8;
static KnownEntry known[KNOWN_MAX];
static uint8_t    knownN = 0;

// On this board a full-power (20 dBm) join crashes the chip: the TX burst
// sags 3V3 and the XTAL-clocked systimer stops (interrupt WDT in
// systimer_hal_get_counter_value). Cap TX power; 11 dBm is ample for any
// AP in the same room. Raise with 't <dBm>' to find the ceiling.
#ifndef WIFI_TX_DBM
#define WIFI_TX_DBM 11
#endif
static float txDbm = WIFI_TX_DBM;

static void applyTxPower() {
  WiFi.setTxPower((wifi_power_t)(int)(txDbm * 4));
}

static const int WIN = 20;            // 20 samples @ 500 ms = 10 s window
static int8_t    hist[WIN];
static int       histN = 0, histI = 0;
static uint32_t  lastScan = 0, connectStart = 0;
static bool      wantConnected = true, connecting = false;
static bool      scanning = false, joinAfterScan = false;
static char      curSsid[33] = "";

static void loadKnown() {
  Preferences p;
  if (!p.begin("ewatch", true)) { Serial.println("nvs    : no \"ewatch\" namespace (AuroraOS never ran?)"); return; }
  knownN = p.getUChar("knownN", 0);
  if (knownN > KNOWN_MAX) knownN = KNOWN_MAX;
  size_t expect = knownN * sizeof(KnownEntry);
  if (knownN == 0 || p.getBytesLength("known") != expect) knownN = 0;
  else p.getBytes("known", known, expect);
  p.end();
  Serial.printf("nvs    : %u saved network(s)\n", knownN);
}

static const KnownEntry *findKnown(const String &ssid) {
  for (uint8_t i = 0; i < knownN; i++)
    if (ssid == known[i].ssid) return &known[i];
  return nullptr;
}

static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
  }
}

// Rough mapping commonly used for 2.4 GHz: -50 dBm or better = 100 %,
// -100 dBm = 0 %.
static int quality(int rssi) { return constrain(2 * (rssi + 100), 0, 100); }

static const char *verdict(int rssi) {
  if (rssi >= -55) return "excellent";
  if (rssi >= -67) return "good";
  if (rssi >= -75) return "fair";
  if (rssi >= -85) return "weak";
  return "unusable";
}

// Async so the I2C/battery readout keeps running during the ~2 s scan.
// Passive scans never transmit: if active scans crash the chip and passive
// ones don't, the TX current burst is what's upsetting it (3V3 sag).
static void startScan(bool thenJoin, bool passive = false) {
  if (scanning) { joinAfterScan |= thenJoin; return; }
  Serial.printf("wifi   : %s scan, tx %.1f dBm...\n", passive ? "passive" : "active",
                WiFi.getTxPower() / 4.0f);
  Serial.flush();                  // get the line out before a possible crash
  WiFi.scanNetworks(true, true, passive, passive ? 300 : 120);
  scanning = true;
  joinAfterScan = thenJoin;
  lastScan = millis();
}

// Copy of the last scan table (sorted, strongest first) so 'j #N' and
// 'j ssid' can pin the join to a known BSSID + channel after scanDelete().
struct ApRow { char ssid[33]; uint8_t bssid[6]; int32_t ch; int8_t rssi; wifi_auth_mode_t auth; };
static const int AP_MAX = 32;
static ApRow aps[AP_MAX];
static int   apN = 0;

static const ApRow *findAp(const char *ssid) {
  for (int i = 0; i < apN; i++)
    if (!strcmp(aps[i].ssid, ssid)) return &aps[i];   // table is sorted: first = strongest
  return nullptr;
}

// Join diagnostics, filled in by the WiFi event handler (runs on the event
// task; loop() prints them).
static volatile uint8_t  lastReason = 0;
static volatile uint16_t discCount = 0;
static volatile bool     assocSeen = false;
static uint16_t          discPrinted = 0;

static void onWifiEvent(arduino_event_id_t e, arduino_event_info_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_CONNECTED) assocSeen = true;
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastReason = info.wifi_sta_disconnected.reason;
    discCount++;
  }
}

// What each common disconnect reason means for *this* hunt.
static const char *reasonHint(uint8_t r) {
  switch (r) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "AP heard us but the key handshake failed: wrong password, or our frames are arriving corrupted";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
      return "authentication rejected: almost always the password (check case / length)";
    case WIFI_REASON_NO_AP_FOUND:
      return "AP not found on that channel: SSID typo, or it moved channel (rescan 'w')";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_BEACON_TIMEOUT:
      return "AP never answered our frames: it can't decode our TX (RF/crystal/power) - try 'm b' and 't' up/down";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "AP refused association (MAC filter / client limit / mode mismatch)";
    default:
      return "";
  }
}

// ---- crystal check --------------------------------------------------
// An AP stamps every beacon with its TSF (a 1 MHz counter from its own
// crystal); the WiFi MAC stamps every received frame with our local time
// (from our 40 MHz crystal). Over ~30 s the two drift apart by exactly the
// relative frequency error. AP crystals are +-20 ppm or better, so measure
// two different APs: if both give the same number, it's ours.
static volatile uint32_t xLocal0, xLocal, xN;
static volatile uint64_t xTsf0, xTsf;
static uint8_t xBssid[6];

static void IRAM_ATTR xSniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = pk->payload;
  if (pk->rx_ctrl.sig_len < 36 || f[0] != 0x80) return;       // beacons only
  if (memcmp(f + 16, xBssid, 6) != 0) return;                  // from our AP
  uint64_t tsf = 0;
  for (int i = 7; i >= 0; i--) tsf = (tsf << 8) | f[24 + i];
  if (xN == 0) { xTsf0 = tsf; xLocal0 = pk->rx_ctrl.timestamp; }
  xTsf = tsf; xLocal = pk->rx_ctrl.timestamp;
  xN++;
}

static void crystalCheck(int row) {
  if (apN == 0) { Serial.println("xtal   : no scan table yet; run 'w' first"); return; }
  if (row < 1 || row > apN) row = 1;
  const ApRow &ap = aps[row - 1];
  Serial.printf("\n---- crystal check vs \"%s\" (row %d, ch %ld, %d dBm), 30 s ----\n",
                ap.ssid[0] ? ap.ssid : "<hidden>", row, (long)ap.ch, ap.rssi);
  if (scanning) { esp_wifi_scan_stop(); WiFi.scanDelete(); scanning = joinAfterScan = false; }
  connecting = false;
  WiFi.disconnect();
  delay(200);
  memcpy(xBssid, ap.bssid, 6);
  xN = 0;
  wifi_promiscuous_filter_t flt = {WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(xSniff);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ap.ch, WIFI_SECOND_CHAN_NONE);

  for (int t = 5; t <= 30; t += 5) {
    delay(5000);
    uint32_t n = xN;
    if (n < 2) { Serial.printf("  %2d s: %lu beacon(s)\n", t, (unsigned long)n); continue; }
    double dTsf = (double)(xTsf - xTsf0), dLoc = (double)(uint32_t)(xLocal - xLocal0);
    Serial.printf("  %2d s: %4lu beacons  our clock vs AP %+7.1f ppm\n", t, (unsigned long)n,
                  (dLoc - dTsf) / dTsf * 1e6);
  }
  esp_wifi_set_promiscuous(false);
  lastScan = millis();

  uint32_t n = xN;
  if (n < 20) {
    Serial.println("  too few beacons for a result (pick a stronger row) - NOT a verdict\n");
    return;
  }
  double ppm = ((double)(uint32_t)(xLocal - xLocal0) - (double)(xTsf - xTsf0)) / (double)(xTsf - xTsf0) * 1e6;
  const char *v = fabs(ppm) <= 20 ? "within WiFi spec: the crystal is NOT why joins fail"
                : fabs(ppm) <= 40 ? "marginal: some APs will reject us - check C3/C4 (12 pF) vs the crystal's 9 pF load"
                                  : "OUT OF SPEC: APs can't decode our TX - fix the crystal load caps (C3/C4)";
  Serial.printf("  result: %+.1f ppm -> %s\n  (repeat on a different AP with 'x N' to rule out that AP's own error)\n\n",
                ppm, v);
}


// ---- speed test -----------------------------------------------------
// Plain HTTP to Cloudflare's speed endpoints, so TLS CPU cost doesn't cap
// the number: GET /__down?bytes=N streams N bytes, POST /__up swallows a body.
// Times include one internet round trip, so compare boards on the same AP,
// same spot, same minute - the absolute Mbps is the uplink's, not only ours.
static const char *SPEED_HOST = "speed.cloudflare.com";
static uint8_t speedBuf[4096];

static bool speedConnect(WiFiClient &c, uint32_t &ms) {
  uint32_t t0 = millis();
  bool ok = c.connect(SPEED_HOST, 80, 5000);
  ms = millis() - t0;
  if (ok) c.setNoDelay(true);
  return ok;
}

// Reads the status line + headers; returns the HTTP status (0 on timeout).
static int speedHeaders(WiFiClient &c, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  int status = 0, state = 0;           // state counts the \r\n\r\n terminator
  char line[64]; int ln = 0; bool first = true;
  while (millis() - t0 < timeoutMs) {
    if (!c.available()) { if (!c.connected()) return status; delay(1); continue; }
    char ch = c.read();
    if (first) {
      if (ch == '\n') { line[ln] = 0; sscanf(line, "HTTP/%*s %d", &status); first = false; }
      else if (ln < (int)sizeof line - 1) line[ln++] = ch;
    }
    state = (ch == '\r' || ch == '\n') ? state + 1 : 0;
    if (state == 4) return status;
  }
  return 0;
}

static void speedTest(float mb) {
  if (!WiFi.isConnected()) { Serial.println("speed  : not connected; join first ('j ssid,pass')"); return; }
  uint32_t n = (uint32_t)(mb * 1000000);
  wifi_ap_record_t ap = {};
  esp_wifi_sta_get_ap_info(&ap);
  Serial.printf("\n---- speed test, %.1f MB each way, host %s ----\n", mb, SPEED_HOST);
  Serial.printf("  link   : RSSI %d dBm, ch %d, tx cap %.1f dBm, phy %s%s%s\n", WiFi.RSSI(), WiFi.channel(),
                WiFi.getTxPower() / 4.0f, ap.phy_11b ? "b" : "", ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "");

  // Latency: TCP connect time (DNS cached after the first), 5 samples.
  uint32_t lat[5]; int latN = 0;
  for (int i = 0; i < 5; i++) {
    WiFiClient c; uint32_t ms;
    if (speedConnect(c, ms)) lat[latN++] = ms;
    c.stop();
  }
  if (latN) {
    uint32_t mn = lat[0], mx = lat[0], sum = 0;
    for (int i = 0; i < latN; i++) { mn = min(mn, lat[i]); mx = max(mx, lat[i]); sum += lat[i]; }
    Serial.printf("  latency: TCP connect %d/5 ok, min %lu / avg %lu / max %lu ms\n", latN,
                  (unsigned long)mn, (unsigned long)(sum / latN), (unsigned long)mx);
  } else {
    Serial.println("  latency: 0/5 connects - no internet path (DNS/gateway?)");
    Serial.println("  result : FAIL\n");
    return;
  }

  // Download
  {
    WiFiClient c; uint32_t cms;
    float mbps = -1; uint32_t got = 0, ms = 0;
    if (speedConnect(c, cms)) {
      uint32_t t0 = millis();
      c.printf("GET /__down?bytes=%lu HTTP/1.1\r\nHost: %s\r\nUser-Agent: ewatch-hwtest\r\nConnection: close\r\n\r\n",
               (unsigned long)n, SPEED_HOST);
      int st = speedHeaders(c, 5000);
      uint32_t last = millis();
      while (st == 200 && got < n && millis() - last < 5000 && millis() - t0 < 30000) {
        int a = c.available();
        if (a <= 0) { if (!c.connected()) break; delay(1); continue; }
        int r = c.read(speedBuf, min(a, (int)sizeof speedBuf));
        if (r > 0) { got += r; last = millis(); }
      }
      ms = millis() - t0;
      if (got) mbps = got * 8.0f / ms / 1000.0f;
      Serial.printf("  down   : HTTP %d, %lu / %lu bytes in %lu ms -> %.2f Mbit/s%s\n", st, (unsigned long)got,
                    (unsigned long)n, (unsigned long)ms, mbps, got < n ? "  (INCOMPLETE)" : "");
    } else Serial.println("  down   : connect failed");
    c.stop();
  }

  // Upload
  {
    WiFiClient c; uint32_t cms;
    memset(speedBuf, 'e', sizeof speedBuf);
    if (speedConnect(c, cms)) {
      uint32_t t0 = millis();
      c.printf("POST /__up HTTP/1.1\r\nHost: %s\r\nUser-Agent: ewatch-hwtest\r\nContent-Type: application/octet-stream\r\n"
               "Content-Length: %lu\r\nConnection: close\r\n\r\n", SPEED_HOST, (unsigned long)n);
      uint32_t sent = 0, last = millis();
      while (sent < n && millis() - last < 5000 && millis() - t0 < 30000 && c.connected()) {
        size_t w = c.write(speedBuf, min((uint32_t)sizeof speedBuf, n - sent));
        if (w > 0) { sent += w; last = millis(); } else delay(1);
      }
      int st = sent == n ? speedHeaders(c, 10000) : 0;   // reply only after the server has it all
      uint32_t ms = millis() - t0;
      Serial.printf("  up     : HTTP %d, %lu / %lu bytes in %lu ms -> %.2f Mbit/s%s\n", st, (unsigned long)sent,
                    (unsigned long)n, (unsigned long)ms, sent * 8.0f / ms / 1000.0f,
                    sent < n || st != 200 ? "  (INCOMPLETE)" : "");
    } else Serial.println("  up     : connect failed");
    c.stop();
  }
  Serial.printf("  after  : RSSI %d dBm, %s\n\n", WiFi.RSSI(), WiFi.isConnected() ? "still connected" : "LINK LOST");
}

static void startJoin(const char *ssid, const char *pass, int32_t ch = 0, const uint8_t *bssid = nullptr) {
  strncpy(curSsid, ssid, 32); curSsid[32] = '\0';
  histN = histI = 0;
  if (scanning) {                  // never associate on top of a running scan
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    scanning = joinAfterScan = false;
  }
  // disconnect() completes asynchronously; begin() issued while it's still
  // pending is silently dropped, so wait for the stack to go idle first.
  WiFi.disconnect(false, true);
  for (int i = 0; i < 50 && WiFi.status() == WL_CONNECTED; i++) delay(10);
  delay(100);

  applyTxPower();
  size_t plen = strlen(pass);
  Serial.printf("wifi   : joining \"%s\" (pass %u chars%s) ", curSsid, (unsigned)plen,
                plen && (plen < 8 || plen > 63) ? " - WPA needs 8..63!" : "");
  if (bssid) Serial.printf("ch %ld BSSID %02X:%02X:%02X:%02X:%02X:%02X ", (long)ch,
                           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  else       Serial.print("(not in last scan, any BSSID) ");
  Serial.printf("tx %.1f dBm\n", WiFi.getTxPower() / 4.0f);
  Serial.flush();                  // get the line out before a possible crash

  assocSeen = false;
  discPrinted = discCount;
  wl_status_t st = WiFi.begin(ssid, pass, ch, bssid);
  if (st == WL_CONNECT_FAILED) Serial.println("wifi   : begin() rejected by the stack");
  applyTxPower();                  // begin() can restore the default
  connecting = true;
  connectStart = millis();
}

// Prints the AP table; returns the index of the strongest saved network in
// range (-1 if none). Results stay valid until scanDelete().
static int printScan(int n) {
  lastScan = millis();
  if (n <= 0) { Serial.printf("\n---- WiFi scan: no networks (%d) ----\n", n); return -1; }

  int order[64];
  n = min(n, 64);
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = 1; i < n; i++)                       // insertion sort by RSSI
    for (int j = i; j > 0 && WiFi.RSSI(order[j]) > WiFi.RSSI(order[j - 1]); j--)
      std::swap(order[j], order[j - 1]);

  Serial.println("\n---- WiFi scan ----");
  Serial.println("   #  RSSI  qual  ch  auth    BSSID              SSID");
  int best = -1;
  apN = 0;
  for (int k = 0; k < n; k++) {
    int i = order[k];
    String ssid = WiFi.SSID(i);
    bool saved = findKnown(ssid) != nullptr;
    if (saved && best < 0) best = i;
    if (apN < AP_MAX) {
      ApRow &a = aps[apN++];
      strncpy(a.ssid, ssid.c_str(), 32); a.ssid[32] = '\0';
      memcpy(a.bssid, WiFi.BSSID(i), 6);
      a.ch = WiFi.channel(i); a.rssi = WiFi.RSSI(i); a.auth = WiFi.encryptionType(i);
    }
    Serial.printf("  %2d  %4d  %3d%%  %2d  %-6s  %s  %s%s\n", k + 1,
                  WiFi.RSSI(i), quality(WiFi.RSSI(i)), WiFi.channel(i),
                  authName(WiFi.encryptionType(i)), WiFi.BSSIDstr(i).c_str(),
                  ssid.length() ? ssid.c_str() : "<hidden>", saved ? "   [saved]" : "");
  }
  Serial.printf("  %d network(s)\n\n", n);
  return best;
}

static void pollScan() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  int best = printScan(n);
  if (joinAfterScan && !WiFi.isConnected() && !connecting) {
    if (best >= 0) {
      const KnownEntry *k = findKnown(WiFi.SSID(best));
      startJoin(k->ssid, k->pass, WiFi.channel(best), WiFi.BSSID(best));
    } else {
      Serial.println(knownN ? "wifi   : no saved network in range; retrying in 15 s ('j ssid,pass' to join another)"
                            : "wifi   : nothing saved on the watch; use 'j ssid,pass'");
    }
  }
  joinAfterScan = false;
  WiFi.scanDelete();
}

static void wifiTick(uint32_t now) {
  pollScan();
  if (discCount != discPrinted) {
    discPrinted = discCount;
    uint8_t r = lastReason;
    const char *hint = reasonHint(r);
    Serial.printf("wifi   : disconnect reason %u %s%s%s%s\n", r,
                  WiFi.disconnectReasonName((wifi_err_reason_t)r),
                  assocSeen ? " (after associating)" : " (never associated)",
                  *hint ? "\n         -> " : "", hint);
  }
  if (connecting) {
    if (WiFi.isConnected()) {
      connecting = false;
      applyTxPower();
      Serial.printf("wifi   : connected in %lu ms, IP %s, ch %d, BSSID %s, tx power %.1f dBm\n",
                    now - connectStart, WiFi.localIP().toString().c_str(), WiFi.channel(),
                    WiFi.BSSIDstr().c_str(), WiFi.getTxPower() / 4.0f);
    } else if (now - connectStart > 15000) {
      connecting = false;
      Serial.printf("wifi   : join gave up after 15 s (status %d, last reason %u %s)\n", WiFi.status(),
                    lastReason, discCount ? WiFi.disconnectReasonName((wifi_err_reason_t)lastReason) : "none");
      WiFi.disconnect();
      lastScan = now;
    }
  } else if (!WiFi.isConnected() && !scanning) {
    if (wantConnected) { if (now - lastScan >= 15000) startScan(true); }   // lost it, or nothing in range yet
    else if (now - lastScan >= 10000) startScan(false);
  }
}

// Appends the WiFi part of the live line.
static int wifiLive(char *line, int n, int cap) {
  if (!WiFi.isConnected())
    return n + snprintf(line + n, cap - n, " | WIFI %s", connecting ? "joining" : scanning ? "scanning" : "down");
  int8_t r = WiFi.RSSI();
  hist[histI] = r; histI = (histI + 1) % WIN; if (histN < WIN) histN++;
  int mn = 127, mx = -128, sum = 0;
  for (int i = 0; i < histN; i++) { mn = min(mn, (int)hist[i]); mx = max(mx, (int)hist[i]); sum += hist[i]; }
  return n + snprintf(line + n, cap - n, " | WIFI %4d dBm %3d%% %-9s 10s %d/%.0f/%d",
                      r, quality(r), verdict(r), mn, (float)sum / histN, mx);
}

// ======================================================================
// Live line + commands
// ======================================================================
static bool paused = false;

static void printLive() {
  char line[256];
  int n = snprintf(line, sizeof line, "[%7lu] ", millis());
  n = i2cLive(line, n, sizeof line);
  if (batOk) n += snprintf(line + n, sizeof line - n, " | BAT %.3f V %3d%% (tap %4lu mV)",
                           batV, batPct(batV), (unsigned long)batAdcMv);
  else       n += snprintf(line + n, sizeof line - n, " | BAT --no reading--");
  n = wifiLive(line, n, sizeof line);
  Serial.println(line);
}

static void help() {
  Serial.println("commands: i=I2C scan  d=dump regs  b=battery check  w=WiFi scan  wp=passive scan\n"
                 "          j ssid,pass | j N,pass=join row N  t <dBm>=tx power  m b|bg|bgn=modes\n"
                 "          x [N]=crystal ppm vs row N's beacons  s [MB]=speed test (connected)\n"
                 "          c=connect saved/disconnect  n=saved list  p=pause");
}

static void handleSerial() {
  static char buf[128];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (len < sizeof buf - 1) buf[len++] = c; continue; }
    buf[len] = '\0';
    if (len == 0) continue;
    len = 0;
    if      (!strcmp(buf, "i")) i2cScan();
    else if (!strcmp(buf, "d")) i2cDump();
    else if (!strcmp(buf, "b")) batCheck();
    else if (!strcmp(buf, "w")) startScan(false);
    else if (!strcmp(buf, "wp")) startScan(false, true);
    else if (buf[0] == 'x' && (buf[1] == '\0' || buf[1] == ' ')) crystalCheck(buf[1] ? atoi(buf + 2) : 1);
    else if (!strncmp(buf, "t ", 2)) {
      txDbm = constrain(atof(buf + 2), 2.0f, 20.0f);
      applyTxPower();
      Serial.printf("wifi   : tx power cap now %.1f dBm\n", WiFi.getTxPower() / 4.0f);
    }
    else if (buf[0] == 's' && (buf[1] == '\0' || buf[1] == ' ')) speedTest(buf[1] ? atof(buf + 2) : 2.0f);
    else if (!strcmp(buf, "p")) { paused = !paused; Serial.println(paused ? "-- paused --" : "-- resumed --"); }
    else if (!strcmp(buf, "n")) {
      for (uint8_t i = 0; i < knownN; i++) Serial.printf("  saved: \"%s\"\n", known[i].ssid);
      if (!knownN) Serial.println("  (none)");
    } else if (!strcmp(buf, "c")) {
      wantConnected = !(WiFi.isConnected() || connecting);
      if (wantConnected) startScan(true);
      else { WiFi.disconnect(); connecting = false; Serial.println("wifi   : disconnected (scan table every 10 s)"); }
    } else if (!strncmp(buf, "j ", 2)) {
      char *ssid = buf + 2, *comma = strchr(ssid, ',');
      const char *pass = "";
      if (comma) { *comma = '\0'; pass = comma + 1; }
      while (*ssid == ' ') ssid++;                               // terminal padding
      for (char *e = ssid + strlen(ssid); e > ssid && e[-1] == ' '; ) *--e = '\0';
      const ApRow *ap = nullptr;
      if (ssid[0] == '#') ssid++;
      bool numeric = ssid[0] && strspn(ssid, "0123456789") == strlen(ssid);
      if (!ssid[0]) { Serial.println("wifi   : no SSID given; use 'j 3,pass' (row from 'w') or 'j ssid,pass'"); continue; }
      if (numeric && !findAp(ssid)) {
        int row = atoi(ssid);
        if (row < 1 || row > apN) { Serial.printf("wifi   : no row %d in the last scan (run 'w')\n", row); continue; }
        ap = &aps[row - 1];
        if (!ap->ssid[0]) { Serial.println("wifi   : that row is a hidden network; use 'j ssid,pass'"); continue; }
        ssid = (char *)ap->ssid;
      } else {
        ap = findAp(ssid);
      }
      wantConnected = true;
      if (ap) startJoin(ssid, pass, ap->ch, ap->bssid);
      else    startJoin(ssid, pass);
    } else if (!strncmp(buf, "m ", 2)) {
      uint8_t proto = WIFI_PROTOCOL_11B;
      if (strchr(buf + 2, 'g')) proto |= WIFI_PROTOCOL_11G;
      if (strchr(buf + 2, 'n')) proto |= WIFI_PROTOCOL_11N;
      esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto);
      Serial.printf("wifi   : modes 802.11%s%s%s %s\n", "b", proto & WIFI_PROTOCOL_11G ? "g" : "",
                    proto & WIFI_PROTOCOL_11N ? "n" : "", err == ESP_OK ? "" : esp_err_to_name(err));
    } else {
      help();
    }
  }
}

void setup() {
  // Keep the LDO enabled or the watch powers off when the button is released.
  pinMode(PIN_LDO_LATCH, OUTPUT);
  digitalWrite(PIN_LDO_LATCH, HIGH);
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // wait for USB CDC host
  Serial.println("\n==== EWatch hardware reader (I2C + battery + WiFi) ====");
  Serial.printf("reset  : reason %d%s\n", (int)esp_reset_reason(),
                esp_reset_reason() == ESP_RST_PANIC || esp_reset_reason() == ESP_RST_INT_WDT
                    ? "  <-- crashed last run" : esp_reset_reason() == ESP_RST_BROWNOUT
                    ? "  <-- BROWNOUT last run" : "");

  // Bring the touch controller out of reset so it answers on the bus.
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(60);

  busRecover();
  if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ))
    Serial.println("ERROR: Wire.begin() failed");
  Serial.printf("bus    : SDA=GPIO%d SCL=GPIO%d %d Hz\n", PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  i2cScan();

  batCheck();                     // before WiFi, so the radio isn't loading the cell

  Serial.printf("mac    : %s\n", WiFi.macAddress().c_str());
  WiFi.persistent(false);         // don't write typed passwords into the driver's NVS
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // modem sleep makes RSSI readings jumpy
  WiFi.setAutoReconnect(false);   // retries are ours, so every failure reason gets printed
  wifi_country_t gb = {"GB", 1, 13, 20, WIFI_COUNTRY_POLICY_MANUAL};
  esp_wifi_set_country(&gb);      // UK: channels 1..13 (Vodafone AP is on 13)
  applyTxPower();
  loadKnown();
  help();
  // Passive first: proves the RF/clock path works without a TX current burst.
  // The follow-up active scan (and join) happens 15 s later from wifiTick.
  startScan(false, true);
  lastScan = millis();
}

void loop() {
  static uint32_t lastLive = 0;
  handleSerial();
  uint32_t now = millis();
  wifiTick(now);
  if (now - batLast >= BAT_PERIOD_MS) { batLast = now; batSample(); }
  if (!paused && now - lastLive >= 500) { lastLive = now; printLive(); }
  delay(10);
}
