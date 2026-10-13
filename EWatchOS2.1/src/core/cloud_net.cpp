// eWatch Cloud network layer: HTTPClient over WiFiClientSecure (verified TLS)
// or WiFiClient (plain http, local tests). See cloud_net.h.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>

#include "cloud_net.h"
#include "cloud_ca.h"
#include "ewlog.h"

bool cloudNetUp() { return WiFi.status() == WL_CONNECTED; }

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

int cloudHttp(const char *base, const char *method, const char *path,
              const char *body, size_t bodyLen, const char *token,
              uint8_t *out, size_t cap, size_t *outLen) {
  *outLen = 0;
  if (cap) out[0] = 0;
  if (!cloudNetUp()) return kNetNoWifi;
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
