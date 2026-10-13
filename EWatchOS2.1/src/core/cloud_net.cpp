// eWatch Cloud network layer: HTTPClient over WiFiClientSecure (verified TLS)
// or WiFiClient (plain http, local tests). See cloud_net.h.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>
#include <stdlib.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cloud_net.h"
#include "cloud_ca.h"
#include "ewlog.h"

bool cloudNetIsRelay(const char *base) { return base && strncmp(base, "usb:", 4) == 0; }
bool cloudNetUp(const char *base) { return cloudNetIsRelay(base) || WiFi.status() == WL_CONNECTED; }

namespace {
// Stream sink into a fixed buffer; HTTPClient::writeToStream handles both
// Content-Length and chunked bodies.
class BufSink : public Stream {
public:
  BufSink(uint8_t *b, size_t cap) : buf(b), cap(cap) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n) override {
    if (len + n > cap) { over = true; n = cap > len ? cap - len : 0; }
    memcpy(buf + len, p, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  uint8_t *buf;
  size_t cap, len = 0;
  bool over = false;
};
}  // namespace

// ---------------------------------------------------------------- USB relay
namespace {
uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {          // = zlib.crc32
  crc = ~crc;
  while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}

// The request in flight; filled by the console task (cloudNetRelayLine),
// read by the cloud task. One at a time (only the cloud task calls cloudHttp).
volatile bool     rBusy = false, rHead = false, rDone = false, rBad = false, rPend = false;
volatile uint32_t rId = 0;
volatile int      rStatus = 0;
volatile size_t   rLen = 0, rGot = 0;
uint8_t          *rOut = nullptr;
size_t            rCap = 0;
uint32_t          rCrc = 0;

int relayHttp(const char *base, const char *method, const char *path,
              const char *body, size_t bodyLen, const char *token,
              uint8_t *out, size_t cap, size_t *outLen) {
  static uint32_t nextId = 1;
  uint32_t id = (esp_random() & 0xFFFF) * 1000 + (nextId++ % 1000);
  rOut = out; rCap = cap ? cap - 1 : 0; rGot = 0; rLen = 0; rCrc = 0;
  rHead = rDone = rBad = rPend = false; rStatus = 0; rId = id; rBusy = true;
  uint32_t t0 = millis();
  // Print the request; again (same id) if no answer header within 1.5 s: a
  // line can be lost on USB. The host answers a repeated id from its cache,
  // so a POST never runs twice.
  static char b64[264];
  // CRC over "METHOD url len token" and the body: a line hit by lost bytes
  // (e.g. a cut token) never reaches the server.
  static char reqLine[256];
  int hn = snprintf(reqLine, sizeof reqLine, "%s %s%s %u %s", method, base + 4, path, (unsigned)bodyLen,
                    token && *token ? token : "-");
  uint32_t reqCrc = crc32(0, (const uint8_t *)reqLine, hn < (int)sizeof reqLine ? hn : sizeof reqLine - 1);
  if (body) reqCrc = crc32(reqCrc, (const uint8_t *)body, bodyLen);
  uint32_t last = 0;
  size_t seen = 0;
  bool head = false;
  for (int attempt = 0; attempt < 3 && !head; attempt++) {
    ewlogTxLock(true);
    Serial.printf("\n#WQ %lu %s\n", (unsigned long)id, reqLine);
    // Body: 256 base64 chars per line (192 raw bytes), so each printf is one
    // small write that other tasks' log lines can't split.
    for (size_t off = 0; body && off < bodyLen; off += 192) {
      size_t n = bodyLen - off < 192 ? bodyLen - off : 192, w = 0;
      mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &w, (const unsigned char *)body + off, n);
      b64[w] = 0;
      Serial.printf("#WB %lu %s\n", (unsigned long)id, b64);
      if ((off / 192) % 16 == 15) vTaskDelay(1);
    }
    Serial.printf("#WE %lu %08lx\n", (unsigned long)id, (unsigned long)reqCrc);
    ewlogTxLock(false);
    last = millis();
    while (!rHead && millis() - last < (rPend ? 20000u : 1500u)) vTaskDelay(1);   // #WP: host is on it
    head = rHead;
  }
  // Then until the body stops moving (6 s without a chunk = lost).
  while (head && !rDone && !rBad) {
    vTaskDelay(1);
    if (rGot != seen) { seen = rGot; last = millis(); }
    if (millis() - last > 6000u) break;
  }
  rBusy = false;
  *outLen = rGot > rCap ? rCap : rGot;
  if (cap) out[*outLen] = 0;
  EWLOGD("CLOUD", "relay %s %s status=%d bytes=%u ms=%lu", method, path, rStatus,
         (unsigned)rGot, (unsigned long)(millis() - t0));
  if (!rHead) return kNetConnect;
  if (rBad || !rDone) return rGot > rCap ? kNetTooBig : kNetTimeout;
  if (rLen > rCap) return kNetTooBig;
  return rStatus > 0 ? rStatus : kNetConnect;
}
}  // namespace

bool cloudNetRelayBusy() { return rBusy; }

bool cloudNetRelayLine(const char *line) {
  if (line[0] != '#' || line[1] != 'W') return false;
  char kind = line[2];
  char *e;
  uint32_t id = strtoul(line + 3, &e, 10);
  if (!rBusy || id != rId) return true;                  // stale or not ours
  while (*e == ' ') e++;
  if (kind == 'P') {
    rPend = true;
  } else if (kind == 'R') {
    rStatus = (int)strtol(e, &e, 10);
    rLen = strtoul(e, &e, 10);
    rHead = true;
  } else if (kind == 'D' && rHead) {
    static uint8_t raw[256];
    size_t n = 0;
    if (mbedtls_base64_decode(raw, sizeof raw, &n, (const unsigned char *)e, strlen(e)) != 0) { rBad = true; return true; }
    if (rGot + n <= rCap) memcpy(rOut + rGot, raw, n);
    else if (rGot < rCap) memcpy(rOut + rGot, raw, rCap - rGot);
    rCrc = crc32(rCrc, raw, n);
    rGot = rGot + n;
    ewlogTxLock(true);
    Serial.printf("#WA %lu %u\n", (unsigned long)id, (unsigned)rGot);
    ewlogTxLock(false);
  } else if (kind == 'Z' && rHead) {
    uint32_t want = strtoul(e, nullptr, 16);
    if (want != rCrc || rGot != rLen) { rBad = true; EWLOGW("CLOUD", "relay crc/len mismatch got=%u want=%u", (unsigned)rGot, (unsigned)rLen); }
    else rDone = true;
  }
  return true;
}

int cloudHttp(const char *base, const char *method, const char *path,
              const char *body, size_t bodyLen, const char *token,
              uint8_t *out, size_t cap, size_t *outLen) {
  *outLen = 0;
  if (cap) out[0] = 0;
  if (cloudNetIsRelay(base)) return relayHttp(base, method, path, body, bodyLen, token, out, cap, outLen);
  if (!cloudNetUp(base)) return kNetNoWifi;
  char url[192];
  snprintf(url, sizeof url, "%s%s", base, path);
  bool tls = strncmp(base, "https://", 8) == 0;
  WiFiClientSecure sec;
  WiFiClient plain;
  if (tls) {
    sec.setCACert(kCloudRootCA);
    sec.setHandshakeTimeout(8);              // s; well under the 20 s task WDT
  }
  HTTPClient h;
  h.setConnectTimeout(6000);
  h.setTimeout(8000);
  h.setReuse(false);
  if (!(tls ? h.begin(sec, url) : h.begin(plain, url))) return kNetConnect;
  if (token && *token) {
    char auth[80];
    snprintf(auth, sizeof auth, "Bearer %s", token);
    h.addHeader("Authorization", auth);
  }
  if (body) h.addHeader("Content-Type", "application/json");
  uint32_t t0 = millis();
  int code = h.sendRequest(method, (uint8_t *)body, body ? bodyLen : 0);
  if (code <= 0) {
    EWLOGW("CLOUD", "http %s %s err=%d (%s) ms=%lu", method, path, code,
           HTTPClient::errorToString(code).c_str(), (unsigned long)(millis() - t0));
    h.end();
    return code == HTTPC_ERROR_READ_TIMEOUT ? kNetTimeout : kNetConnect;
  }
  BufSink sink(out, cap ? cap - 1 : 0);
  int w = h.writeToStream(&sink);
  h.end();
  *outLen = sink.len;
  if (cap) out[sink.len] = 0;
  EWLOGD("CLOUD", "http %s %s status=%d bytes=%u ms=%lu", method, path, code,
         (unsigned)sink.len, (unsigned long)(millis() - t0));
  if (sink.over) return kNetTooBig;
  if (w < 0 && sink.len == 0 && code != 204) return kNetTimeout;
  return code;
}
