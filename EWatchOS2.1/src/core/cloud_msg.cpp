// eWatch Cloud messages: the watch-side service (see cloud_msg.h and
// MESSAGES_DESIGN.md in the EwatchCloud repo).
//
// One low-priority task on core 0 does all network work, one request at a
// time (one TLS session ~45 KB). It holds the WiFi radio (wifiSvcHold, pinged
// while busy) only when it has something to do: pairing, the outbox, read
// marks, an image, the inbox, contacts, or a new-message check. The check
// runs whenever WiFi associates (any sync window) and every 60 s while it
// stays up. Views only read copies of the cached state (sMx).
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cloud_msg.h"
#include "cloud_net.h"
#include "wifi_svc.h"
#include "model.h"
#include "haptic.h"
#include "ewlog.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

namespace {

const char *kDefaultUrl = "https://api.ewatch.cloud";
constexpr size_t kRespCap = 40 * 1024;         // JSON or a <= 32 KB watch JPEG
constexpr uint32_t kSummaryEveryMs = 60000;
constexpr int kOutboxSlots = 2;
constexpr int kReadQueue = 8;

void *ps(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

struct PsAlloc : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return ps(n); }
  void deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
} sPsAlloc;

SemaphoreHandle_t sMx = nullptr;
struct Lock {
  Lock() { if (sMx) xSemaphoreTake(sMx, portMAX_DELAY); }
  ~Lock() { if (sMx) xSemaphoreGive(sMx); }
};

// ---- persistent (NVS "ewcloud") ----
char sToken[64] = "";
char sName[kCloudNameMax] = "";
char sSelfId[kCloudIdMax] = "";
char sUrl[96] = "";
char sLastNotified[kCloudIdMax] = "";

// ---- link / pairing ----
CloudLink sLink = CloudLink::Unlinked;
bool      sPairStart = false;
char      sPairCode[16] = "", sPairDevice[kCloudIdMax] = "", sPairSecret[72] = "";
uint32_t  sPairDeadline = 0, sPairNextPoll = 0, sPairInterval = 5000;
char      sPairErr[32] = "";

// ---- contacts ----
CloudContact sContacts[kCloudMaxContacts];
size_t       sNContacts = 0;
bool         sWantContacts = false, sContactsLoading = false;

// ---- inbox ----
CloudMsg  sInbox[kCloudInboxMax];
char     *sInboxText[kCloudInboxMax] = {};      // PSRAM, kCloudTextMax + 1 each
size_t    sNInbox = 0;
uint32_t  sInboxRev = 1;
bool      sWantInbox = false, sInboxLoading = false;
char      sOpenId[kCloudIdMax] = "";
char      sReads[kReadQueue][kCloudIdMax];
int       sNReads = 0;

// ---- summary / notification ----
bool      sWantSummary = false;
uint32_t  sLastSummaryMs = 0;
uint16_t  sUnread = 0;
bool      sNotifyPending = false;
CloudNotice sNotice = {};

// ---- image ----
char           sImgWant[kCloudIdMax] = "";
CloudImgState  sImgState = CloudImgState::Idle;
uint16_t      *sImgPx = nullptr;
uint16_t       sImgW = 0, sImgH = 0;

// ---- outbox ----
struct Outgoing {
  bool used;
  char to[kCloudIdMax];
  char *text;          // PSRAM, or nullptr
  uint16_t *px;        // PSRAM w*h RGB565, or nullptr
  uint16_t w, h;
};
Outgoing       sOut[kOutboxSlots] = {};
CloudSendState sSendState = CloudSendState::Idle;
char           sSendReason[40] = "";
uint32_t       sSendStateMs = 0;

// ---- reply target ----
CloudContact sReply = {};
bool         sHaveReply = false;

uint8_t *sResp = nullptr;              // task-only response buffer (PSRAM)
TaskHandle_t sTask = nullptr;
bool sHolding = false;
bool sWasConnected = false;

// Printable ASCII only (the watch fonts); UTF-8 sequences become '?'.
void asciiCopy(char *dst, size_t cap, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src && src[i] && j + 1 < cap; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 0x80) {
      if ((c & 0xC0) == 0xC0) dst[j++] = '?';   // lead byte only
      continue;
    }
    dst[j++] = (c < 32) ? ' ' : (char)c;
  }
  dst[j] = 0;
}

void copyStr(char *dst, size_t cap, const char *src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

void saveNvs() {
  Preferences p;
  if (!p.begin("ewcloud", false)) return;
  p.putString("token", sToken);
  p.putString("name", sName);
  p.putString("uid", sSelfId);
  p.putString("url", sUrl);
  p.putString("lastN", sLastNotified);
  p.end();
}

const char *baseUrl() { return sUrl[0] ? sUrl : kDefaultUrl; }

bool wifiClientOn() {
  ModelLock lk;
  return model.wifiEnabled && model.wifiMode == WifiMode::Client;
}

void dropToken(const char *why) {
  {
    Lock l;
    sToken[0] = 0; sName[0] = 0; sSelfId[0] = 0;
    sLink = CloudLink::Unlinked;
    copyStr(sPairErr, sizeof sPairErr, why);
    sNContacts = 0; sNInbox = 0; sInboxRev++;
  }
  saveNvs();
  EWLOGW("CLOUD", "unlinked reason=%s", why);
}

// A request with the device token; handles 401 (token revoked on the site).
int api(const char *method, const char *path, const char *body, size_t bodyLen, size_t *len) {
  char tok[64];
  { Lock l; copyStr(tok, sizeof tok, sToken); }
  int st = cloudHttp(baseUrl(), method, path, body, bodyLen, tok[0] ? tok : nullptr,
                     sResp, kRespCap, len);
  if (st == 401 && tok[0]) dropToken("Unlinked on ewatch.cloud");
  return st;
}

const char *netReason(int st) {
  switch (st) {
    case kNetNoWifi: return "No WiFi";
    case kNetConnect: return "Can't reach eWatch Cloud";
    case kNetTimeout: return "Timed out";
    case kNetTooBig: return "Reply too big";
    case 401: return "Not linked";
    case 403: return "Not a contact";
    case 413: return "Too big to send";
    case 429: return "Too many, try later";
    default: return "Server error";
  }
}

const char *personName(JsonVariantConst v) {
  if (v.is<const char *>()) return v.as<const char *>();
  const char *n = v["displayName"] | (const char *)nullptr;
  return n ? n : (v["username"] | "?");
}

// ---- base64 (for the EWI1 image) ----
size_t b64(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? T[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

// JSON string escape into dst (returns chars written, excl. NUL).
size_t jsonEsc(char *dst, size_t cap, const char *s) {
  size_t o = 0;
  for (; s && *s && o + 7 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
    else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
    else if (c < 32) o += snprintf(dst + o, cap - o, "\\u%04x", c);
    else dst[o++] = (char)c;
  }
  dst[o] = 0;
  return o;
}

// EWI1 (design §5): "EWI1", u16 w, u16 h, then (u16 count, u16 rgb565) runs.
uint8_t *ewi1(const uint16_t *px, uint16_t w, uint16_t h, size_t *len) {
  size_t n = (size_t)w * h, runs = 0;
  for (size_t i = 0; i < n;) {
    size_t j = i + 1;
    while (j < n && px[j] == px[i] && j - i < 65535) j++;
    runs++; i = j;
  }
  *len = 8 + runs * 4;
  uint8_t *b = (uint8_t *)ps(*len);
  if (!b) return nullptr;
  memcpy(b, "EWI1", 4);
  b[4] = w & 0xFF; b[5] = w >> 8; b[6] = h & 0xFF; b[7] = h >> 8;
  uint8_t *p = b + 8;
  for (size_t i = 0; i < n;) {
    size_t j = i + 1;
    while (j < n && px[j] == px[i] && j - i < 65535) j++;
    uint16_t cnt = (uint16_t)(j - i), c = px[i];
    *p++ = cnt & 0xFF; *p++ = cnt >> 8; *p++ = c & 0xFF; *p++ = c >> 8;
    i = j;
  }
  return b;
}

void setSend(CloudSendState s, const char *why) {
  Lock l;
  sSendState = s;
  copyStr(sSendReason, sizeof sSendReason, why);
  sSendStateMs = millis();
}

// ---------------------------------------------------------------- jobs
void doPairStart() {
  char body[96];
  snprintf(body, sizeof body, "{\"name\":\"EWatch\",\"hw\":\"v%d.%d\"}", EWATCH_HW_REV / 10, EWATCH_HW_REV % 10);
  size_t len;
  int st = cloudHttp(baseUrl(), "POST", "/devices/pair/start", body, strlen(body), nullptr, sResp, kRespCap, &len);
  JsonDocument d(&sPsAlloc);
  if ((st == 200 || st == 201) && !deserializeJson(d, sResp, len)) {
    Lock l;
    copyStr(sPairCode, sizeof sPairCode, d["code"] | "");
    copyStr(sPairDevice, sizeof sPairDevice, d["deviceId"] | "");
    copyStr(sPairSecret, sizeof sPairSecret, d["secret"] | "");
    uint32_t exp = d["expiresIn"] | 600, iv = d["interval"] | 5;
    sPairDeadline = millis() + exp * 1000;
    sPairInterval = (iv < 2 ? 2 : iv) * 1000;
    sPairNextPoll = millis() + sPairInterval;
    sPairErr[0] = 0;
    EWLOGI("CLOUD", "pair code=%s expires_s=%lu", sPairCode, (unsigned long)exp);
  } else {
    Lock l;
    sLink = CloudLink::Unlinked;
    copyStr(sPairErr, sizeof sPairErr, st == 429 ? "Too many tries, wait" : netReason(st));
    EWLOGW("CLOUD", "pair start status=%d", st);
  }
}

void doPairPoll() {
  char body[160];
  {
    Lock l;
    snprintf(body, sizeof body, "{\"deviceId\":\"%s\",\"secret\":\"%s\"}", sPairDevice, sPairSecret);
    sPairNextPoll = millis() + sPairInterval;
  }
  size_t len;
  int st = cloudHttp(baseUrl(), "POST", "/devices/pair/poll", body, strlen(body), nullptr, sResp, kRespCap, &len);
  if (st == 202) return;                                   // pending
  JsonDocument d(&sPsAlloc);
  if (st == 200 && !deserializeJson(d, sResp, len) && d["token"].is<const char *>()) {
    {
      Lock l;
      copyStr(sToken, sizeof sToken, d["token"]);
      asciiCopy(sName, sizeof sName, personName(d["account"]));
      copyStr(sSelfId, sizeof sSelfId, d["account"]["id"] | "");
      sLink = CloudLink::Linked;
      sPairCode[0] = 0; sPairErr[0] = 0;
      sWantContacts = sWantSummary = sWantInbox = true;
    }
    saveNvs();
    EWLOGI("CLOUD", "linked name=%s", sName);
    hapticBuzz(140, 60);
    return;
  }
  if (st == 410 || st == 404) {
    Lock l;
    sLink = CloudLink::Unlinked;
    sPairCode[0] = 0;
    copyStr(sPairErr, sizeof sPairErr, st == 410 ? "Code expired" : "Pairing failed");
    EWLOGW("CLOUD", "pair poll status=%d", st);
  }
  // network errors: keep polling until the deadline
}

void doContacts() {
  size_t len;
  int st = api("GET", "/contacts", nullptr, 0, &len);
  JsonDocument d(&sPsAlloc);
  if (st == 200 && !deserializeJson(d, sResp, len)) {
    Lock l;
    sNContacts = 0;
    if (sSelfId[0]) {
      CloudContact &me = sContacts[sNContacts++];
      copyStr(me.id, sizeof me.id, sSelfId);
      copyStr(me.name, sizeof me.name, "Me");
      me.self = true;
    }
    for (JsonVariantConst c : d["contacts"].as<JsonArrayConst>()) {
      if (sNContacts >= kCloudMaxContacts) break;
      CloudContact &k = sContacts[sNContacts++];
      copyStr(k.id, sizeof k.id, c["userId"] | "");
      asciiCopy(k.name, sizeof k.name, personName(c));
      k.self = false;
    }
    EWLOGI("CLOUD", "contacts n=%u", (unsigned)sNContacts);
  } else {
    EWLOGW("CLOUD", "contacts status=%d", st);
  }
}

// Fill in our own user id if the pairing reply didn't carry it.
void doWhoami() {
  size_t len;
  int st = api("GET", "/whoami", nullptr, 0, &len);
  JsonDocument d(&sPsAlloc);
  if (st == 200 && !deserializeJson(d, sResp, len)) {
    { Lock l; copyStr(sSelfId, sizeof sSelfId, d["user"]["id"] | ""); }
    saveNvs();
  }
}

void doSummary() {
  size_t len;
  int st = api("GET", "/messages/summary", nullptr, 0, &len);
  { Lock l; sLastSummaryMs = millis(); }
  JsonDocument d(&sPsAlloc);
  if (st != 200 || deserializeJson(d, sResp, len)) {
    EWLOGW("CLOUD", "summary status=%d", st);
    return;
  }
  uint16_t unread = d["unread"] | 0;
  JsonVariantConst lt = d["latest"];
  const char *id = lt["id"] | "";
  bool fresh;
  {
    Lock l;
    sUnread = unread;
    fresh = unread > 0 && id[0] && strcmp(id, sLastNotified) != 0;
    if (fresh) {
      copyStr(sNotice.id, sizeof sNotice.id, id);
      asciiCopy(sNotice.from, sizeof sNotice.from, personName(lt["from"]));
      asciiCopy(sNotice.preview, sizeof sNotice.preview, lt["preview"] | "");
      const char *k = lt["kind"] | "TEXT";
      sNotice.image = strcmp(k, "IMAGE") == 0;
      copyStr(sLastNotified, sizeof sLastNotified, id);
      sNotifyPending = true;
      sWantInbox = true;
    }
  }
  EWLOGI("CLOUD", "summary unread=%u latest=%s new=%d", unread, id, fresh ? 1 : 0);
  if (fresh) { saveNvs(); hapticBuzz(150, 80); }
}

void doInbox() {
  size_t len;
  int st = api("GET", "/messages?compact=1&box=in&limit=10", nullptr, 0, &len);
  JsonDocument d(&sPsAlloc);
  if (st != 200 || deserializeJson(d, sResp, len)) {
    EWLOGW("CLOUD", "inbox status=%d", st);
    return;
  }
  Lock l;
  sNInbox = 0;
  uint16_t unread = 0;
  for (JsonVariantConst m : d["messages"].as<JsonArrayConst>()) {
    if (sNInbox >= kCloudInboxMax) break;
    size_t i = sNInbox++;
    CloudMsg &c = sInbox[i];
    copyStr(c.id, sizeof c.id, m["id"] | "");
    asciiCopy(c.from, sizeof c.from, m["f"] | "?");
    copyStr(c.fromId, sizeof c.fromId, m["fu"] | "");
    c.image = strcmp(m["k"] | "t", "i") == 0;
    c.read = (m["r"] | 0) != 0;
    c.ts = m["ts"] | 0;
    c.iw = m["iw"] | 0; c.ih = m["ih"] | 0;
    if (!c.read) unread++;
    const char *t = m["t"] | "";
    if (!sInboxText[i]) sInboxText[i] = (char *)ps(kCloudTextMax + 1);
    if (sInboxText[i]) asciiCopy(sInboxText[i], kCloudTextMax + 1, t);
    size_t k = 0;
    for (; t[k] && t[k] != '\n' && k + 1 < sizeof c.preview; k++) c.preview[k] = (t[k] >= 32 && t[k] < 127) ? t[k] : ' ';
    c.preview[k] = 0;
    if (!c.preview[0]) copyStr(c.preview, sizeof c.preview, c.image ? "Photo" : "");
  }
  sUnread = unread;
  sInboxRev++;
  EWLOGI("CLOUD", "inbox n=%u unread=%u", (unsigned)sNInbox, unread);
}

void doRead(const char *id) {
  char path[80];
  snprintf(path, sizeof path, "/messages/%s/read", id);
  size_t len;
  int st = api("POST", path, "{}", 2, &len);
  EWLOGD("CLOUD", "read id=%s status=%d", id, st);
}

// JPEGDEC draws MCU blocks; copy them into the RGB565 frame.
int jpegDraw(JPEGDRAW *p) {
  for (int y = 0; y < p->iHeight; y++) {
    int dy = p->y + y;
    if (dy >= sImgH) break;
    int w = p->iWidth;
    if (p->x + w > sImgW) w = sImgW - p->x;
    if (w > 0) memcpy(sImgPx + (size_t)dy * sImgW + p->x, p->pPixels + (size_t)y * p->iWidth, (size_t)w * 2);
  }
  return 1;
}

void doImage(const char *id) {
  char path[80];
  snprintf(path, sizeof path, "/messages/%s/image?v=watch", id);
  size_t len;
  int st = api("GET", path, nullptr, 0, &len);
  bool ok = false;
  if (st == 200 && len > 4) {
    static JPEGDEC jpg;
    if (jpg.openRAM(sResp, (int)len, jpegDraw)) {
      int w = jpg.getWidth(), h = jpg.getHeight();
      if (w > 0 && h > 0 && w <= 320 && h <= 320) {
        if (!sImgPx) sImgPx = (uint16_t *)ps(320 * 320 * 2);
        if (sImgPx) {
          sImgW = (uint16_t)w; sImgH = (uint16_t)h;
          memset(sImgPx, 0, (size_t)w * h * 2);
          jpg.setPixelType(RGB565_LITTLE_ENDIAN);
          ok = jpg.decode(0, 0, 0) != 0;
        }
      }
      jpg.close();
    }
  }
  Lock l;
  if (strcmp(sImgWant, id) == 0) sImgState = ok ? CloudImgState::Ready : CloudImgState::Failed;
  EWLOGI("CLOUD", "image id=%s status=%d bytes=%u ok=%d %ux%u", id, st, (unsigned)len, ok ? 1 : 0, sImgW, sImgH);
}

void doSend(Outgoing &o) {
  size_t cap = 256 + (o.text ? kCloudTextMax * 6 : 0);
  uint8_t *img = nullptr;
  size_t imgLen = 0;
  if (o.px) {
    img = ewi1(o.px, o.w, o.h, &imgLen);
    if (!img || imgLen > 48 * 1024) {
      free(img);
      setSend(CloudSendState::Failed, img ? "Drawing too detailed" : "Out of memory");
      return;
    }
    cap += imgLen * 4 / 3 + 8;
  }
  char *body = (char *)ps(cap);
  if (!body) { free(img); setSend(CloudSendState::Failed, "Out of memory"); return; }
  size_t n = snprintf(body, cap, "{\"to\":\"%s\"", o.to);
  if (o.text && o.text[0]) {
    n += snprintf(body + n, cap - n, ",\"text\":\"");
    n += jsonEsc(body + n, cap - n, o.text);
    n += snprintf(body + n, cap - n, "\"");
  }
  if (img) {
    n += snprintf(body + n, cap - n, ",\"image\":\"ewi1:");
    n += b64(img, imgLen, body + n);
    n += snprintf(body + n, cap - n, "\"");
    free(img);
  }
  n += snprintf(body + n, cap - n, "}");
  size_t len;
  int st = api("POST", "/messages", body, n, &len);
  free(body);
  EWLOGI("CLOUD", "send to=%s kind=%s bytes=%u status=%d", o.to, o.px ? "image" : "text", (unsigned)n, st);
  if (st == 200 || st == 201) setSend(CloudSendState::Sent, "");
  else setSend(CloudSendState::Failed, netReason(st));
}

// ---------------------------------------------------------------- task
void hold(bool on) {
  if (on) { wifiSvcHold(true); sHolding = true; }
  else if (sHolding) { wifiSvcHold(false); sHolding = false; }
}

// Fail whatever was waiting for the network.
void failAll(const char *why) {
  for (auto &o : sOut) {
    if (!o.used) continue;
    free(o.text); free(o.px);
    o = Outgoing{};
    setSend(CloudSendState::Failed, why);
  }
  Lock l;
  if (sLink == CloudLink::Pairing) {
    sLink = sToken[0] ? CloudLink::Linked : CloudLink::Unlinked;
    sPairStart = false;
    sPairCode[0] = 0;
    copyStr(sPairErr, sizeof sPairErr, why);
  }
  if (sImgState == CloudImgState::Loading) sImgState = CloudImgState::Failed;
  sWantInbox = sWantContacts = sWantSummary = false;
  sInboxLoading = sContactsLoading = false;
  sNReads = 0;
}

bool workPending() {
  Lock l;
  bool linked = sLink == CloudLink::Linked;
  if (sLink == CloudLink::Pairing) return true;
  if (!linked) return false;
  for (auto &o : sOut) if (o.used) return true;
  return sNReads || sImgState == CloudImgState::Loading || sWantInbox || sWantContacts || sWantSummary;
}

void cloudTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(150));
    bool up = cloudNetUp();
    {
      Lock l;
      if (up && !sWasConnected && sLink == CloudLink::Linked) { sWantSummary = true; sWantContacts = true; }
      if (up && sLink == CloudLink::Linked && millis() - sLastSummaryMs > kSummaryEveryMs) sWantSummary = true;
    }
    sWasConnected = up;
    if (!workPending()) { hold(false); continue; }

    if (!wifiClientOn()) { failAll("Turn on WiFi"); hold(false); continue; }
    hold(true);
    if (!up) {
      uint32_t t0 = millis();
      while (!cloudNetUp() && millis() - t0 < 30000) { wifiSvcHold(true); vTaskDelay(pdMS_TO_TICKS(250)); }
      if (!cloudNetUp()) { failAll("No WiFi"); hold(false); continue; }
      sWasConnected = true;
    }

    // One unit of work per pass, most urgent first.
    CloudLink link;
    bool pairStart, pairPoll;
    { Lock l; link = sLink; pairStart = sPairStart; pairPoll = link == CloudLink::Pairing && sPairCode[0] && (int32_t)(millis() - sPairNextPoll) >= 0; }
    if (link == CloudLink::Pairing) {
      if (pairStart) { { Lock l; sPairStart = false; } doPairStart(); continue; }
      if ((int32_t)(millis() - sPairDeadline) >= 0) {
        Lock l; sLink = CloudLink::Unlinked; sPairCode[0] = 0; copyStr(sPairErr, sizeof sPairErr, "Code expired");
        continue;
      }
      if (pairPoll) doPairPoll();
      continue;
    }
    if (link != CloudLink::Linked) continue;

    Outgoing *o = nullptr;
    for (auto &x : sOut) if (x.used) { o = &x; break; }
    if (o) {
      doSend(*o);
      free(o->text); free(o->px);
      *o = Outgoing{};
      continue;
    }
    char id[kCloudIdMax] = "";
    { Lock l; if (sNReads) { copyStr(id, sizeof id, sReads[0]); memmove(sReads, sReads + 1, sizeof sReads[0] * --sNReads); } }
    if (id[0]) { doRead(id); continue; }
    { Lock l; if (sImgState == CloudImgState::Loading) copyStr(id, sizeof id, sImgWant); }
    if (id[0]) { doImage(id); continue; }
    bool want;
    { Lock l; want = sWantSummary; sWantSummary = false; }
    if (want) { doSummary(); continue; }
    { Lock l; want = sWantInbox; sWantInbox = false; sInboxLoading = want; }
    if (want) { doInbox(); { Lock l; sInboxLoading = false; } continue; }
    { Lock l; want = sWantContacts; sWantContacts = false; sContactsLoading = want; }
    if (want) {
      bool needSelf;
      { Lock l; needSelf = !sSelfId[0]; }
      if (needSelf) doWhoami();
      doContacts();
      { Lock l; sContactsLoading = false; }
      continue;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- API
void cloudInit() {
  if (sMx) return;
  sMx = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin("ewcloud", true)) {
    copyStr(sToken, sizeof sToken, p.getString("token", "").c_str());
    copyStr(sName, sizeof sName, p.getString("name", "").c_str());
    copyStr(sSelfId, sizeof sSelfId, p.getString("uid", "").c_str());
    copyStr(sUrl, sizeof sUrl, p.getString("url", "").c_str());
    copyStr(sLastNotified, sizeof sLastNotified, p.getString("lastN", "").c_str());
    p.end();
  }
  sLink = sToken[0] ? CloudLink::Linked : CloudLink::Unlinked;
  sResp = (uint8_t *)ps(kRespCap);
  if (sResp) xTaskCreatePinnedToCore(cloudTask, "cloud", 10240, nullptr, 1, &sTask, 0);
  EWLOGI("CLOUD", "init linked=%d name=%s url=%s", sLink == CloudLink::Linked ? 1 : 0, sName, baseUrl());
}

CloudLink cloudLinkState() { Lock l; return sLink; }
const char *cloudAccountName() { return sName; }

void cloudStartPairing() {
  Lock l;
  sLink = CloudLink::Pairing;
  sPairStart = true;
  sPairCode[0] = 0; sPairErr[0] = 0;
  sPairDeadline = millis() + 600000;
}

void cloudCancelPairing() {
  Lock l;
  if (sLink != CloudLink::Pairing) return;
  sLink = sToken[0] ? CloudLink::Linked : CloudLink::Unlinked;
  sPairStart = false; sPairCode[0] = 0;
}

bool cloudPairingCode(char *out, size_t cap, uint32_t *secsLeft) {
  Lock l;
  if (sLink != CloudLink::Pairing || !sPairCode[0]) return false;
  // Shown as two groups of 4 for reading out ("K7QX-4M2P").
  size_t n = strlen(sPairCode);
  if (n == 8 && cap > 9) snprintf(out, cap, "%.4s-%.4s", sPairCode, sPairCode + 4);
  else copyStr(out, cap, sPairCode);
  int32_t left = (int32_t)(sPairDeadline - millis());
  if (secsLeft) *secsLeft = left > 0 ? (uint32_t)left / 1000 : 0;
  return true;
}

const char *cloudPairingError() { return sPairErr; }

void cloudUnlink() {
  {
    Lock l;
    sToken[0] = 0; sName[0] = 0; sSelfId[0] = 0; sLastNotified[0] = 0;
    sLink = CloudLink::Unlinked; sPairErr[0] = 0;
    sNContacts = 0; sNInbox = 0; sInboxRev++; sUnread = 0;
  }
  saveNvs();
  EWLOGI("CLOUD", "unlink");
}

size_t cloudContacts(CloudContact *out, size_t max) {
  Lock l;
  size_t n = sNContacts < max ? sNContacts : max;
  memcpy(out, sContacts, sizeof(CloudContact) * n);
  return n;
}
void cloudRefreshContacts() { Lock l; sWantContacts = true; sContactsLoading = true; }
bool cloudContactsLoading() { Lock l; return sContactsLoading || sWantContacts; }

bool cloudSendText(const char *to, const char *text) {
  if (!to || !*to || !text) return false;
  Lock l;
  if (sLink != CloudLink::Linked) return false;
  for (auto &o : sOut) {
    if (o.used) continue;
    size_t n = strlen(text);
    if (n > kCloudTextMax) n = kCloudTextMax;
    o.text = (char *)ps(n + 1);
    if (!o.text) return false;
    memcpy(o.text, text, n); o.text[n] = 0;
    copyStr(o.to, sizeof o.to, to);
    o.px = nullptr;
    o.used = true;
    sSendState = CloudSendState::Sending; sSendReason[0] = 0; sSendStateMs = millis();
    return true;
  }
  return false;
}

bool cloudSendImage(const char *to, const uint16_t *pixels, uint16_t w, uint16_t h, const char *caption) {
  if (!to || !*to || !pixels || !w || !h || w > 240 || h > 280) return false;
  Lock l;
  if (sLink != CloudLink::Linked) return false;
  for (auto &o : sOut) {
    if (o.used) continue;
    o.px = (uint16_t *)ps((size_t)w * h * 2);
    if (!o.px) return false;
    memcpy(o.px, pixels, (size_t)w * h * 2);
    o.w = w; o.h = h;
    o.text = nullptr;
    if (caption && *caption) {
      size_t n = strlen(caption);
      if (n > kCloudTextMax) n = kCloudTextMax;
      o.text = (char *)ps(n + 1);
      if (o.text) { memcpy(o.text, caption, n); o.text[n] = 0; }
    }
    copyStr(o.to, sizeof o.to, to);
    o.used = true;
    sSendState = CloudSendState::Sending; sSendReason[0] = 0; sSendStateMs = millis();
    return true;
  }
  return false;
}

CloudSendState cloudSendState(char *reason, size_t cap) {
  Lock l;
  if (reason && cap) copyStr(reason, cap, sSendReason);
  return sSendState;
}

uint32_t cloudSendStateMs() { Lock l; return sSendStateMs; }

uint16_t cloudUnreadCount() { Lock l; return sUnread; }

void cloudRefreshInbox() { Lock l; if (sLink == CloudLink::Linked) { sWantInbox = true; sInboxLoading = true; } }
bool cloudInboxLoading() { Lock l; return sInboxLoading || sWantInbox; }
uint32_t cloudInboxRevision() { Lock l; return sInboxRev; }

size_t cloudInbox(CloudMsg *out, size_t max) {
  Lock l;
  size_t n = sNInbox < max ? sNInbox : max;
  memcpy(out, sInbox, sizeof(CloudMsg) * n);
  return n;
}

bool cloudInboxText(const char *id, char *out, size_t cap) {
  Lock l;
  for (size_t i = 0; i < sNInbox; i++) {
    if (strcmp(sInbox[i].id, id) != 0) continue;
    copyStr(out, cap, sInboxText[i] ? sInboxText[i] : "");
    return true;
  }
  return false;
}

void cloudMarkRead(const char *id) {
  Lock l;
  for (size_t i = 0; i < sNInbox; i++) {
    if (strcmp(sInbox[i].id, id) != 0 || sInbox[i].read) continue;
    sInbox[i].read = true;
    if (sUnread) sUnread--;
    sInboxRev++;
    if (sNReads < kReadQueue) copyStr(sReads[sNReads++], kCloudIdMax, id);
  }
}

void cloudLoadImage(const char *id) {
  Lock l;
  copyStr(sImgWant, sizeof sImgWant, id);
  sImgState = CloudImgState::Loading;
}

CloudImgState cloudImage(const uint16_t **px, uint16_t *w, uint16_t *h) {
  Lock l;
  if (px) *px = sImgPx;
  if (w) *w = sImgW;
  if (h) *h = sImgH;
  return sImgState;
}

bool cloudTakeNotify() {
  Lock l;
  bool n = sNotifyPending;
  sNotifyPending = false;
  return n;
}

void cloudNotice(CloudNotice *out) { Lock l; *out = sNotice; }

void cloudOpenWhenLoaded(const char *id) { Lock l; copyStr(sOpenId, sizeof sOpenId, id); }
bool cloudTakeOpenId(char *out, size_t cap) {
  Lock l;
  if (!sOpenId[0]) return false;
  for (size_t i = 0; i < sNInbox; i++) {
    if (strcmp(sInbox[i].id, sOpenId) == 0) {
      copyStr(out, cap, sOpenId);
      sOpenId[0] = 0;
      return true;
    }
  }
  return false;
}

void cloudSetReplyTo(const CloudContact *c) {
  Lock l;
  sHaveReply = c != nullptr;
  if (c) sReply = *c;
}
bool cloudReplyTo(CloudContact *out) {
  Lock l;
  if (sHaveReply && out) *out = sReply;
  return sHaveReply;
}

void cloudCheckNow() {
  Lock l;
  if (sLink != CloudLink::Linked) return;
  sWantSummary = true; sWantInbox = true; sInboxLoading = true;
}

const char *cloudBaseUrl() { return baseUrl(); }
void cloudSetBaseUrl(const char *url) {
  { Lock l; copyStr(sUrl, sizeof sUrl, url && strcmp(url, kDefaultUrl) ? url : ""); }
  saveNvs();
}

bool cloudConsole(const char *line) {
  if (strncmp(line, "cloud", 5) != 0) return false;
  const char *a = line + 5;
  while (*a == ' ') a++;
  if (!strncmp(a, "url ", 4)) { cloudSetBaseUrl(a + 4); Serial.printf("cloud url=%s\n", baseUrl()); return true; }
  if (!strcmp(a, "check")) { cloudCheckNow(); Serial.println("cloud check queued"); return true; }
  if (!strcmp(a, "pair")) { cloudStartPairing(); Serial.println("cloud pairing"); return true; }
  if (!strcmp(a, "unlink")) { cloudUnlink(); Serial.println("cloud unlinked"); return true; }
  char code[16] = "";
  uint32_t left = 0;
  cloudPairingCode(code, sizeof code, &left);
  Lock l;
  Serial.printf("cloud link=%d name=%s self=%s url=%s code=%s left=%lu err=%s contacts=%u inbox=%u unread=%u send=%d\n",
                (int)sLink, sName, sSelfId, baseUrl(), code, (unsigned long)left, sPairErr,
                (unsigned)sNContacts, (unsigned)sNInbox, sUnread, (int)sSendState);
  return true;
}

#else  // no WiFi: the API links and does nothing.
void cloudInit() {}
CloudLink cloudLinkState() { return CloudLink::Unlinked; }
const char *cloudAccountName() { return ""; }
void cloudStartPairing() {}
void cloudCancelPairing() {}
bool cloudPairingCode(char *, size_t, uint32_t *) { return false; }
const char *cloudPairingError() { return "No WiFi in this build"; }
void cloudUnlink() {}
size_t cloudContacts(CloudContact *, size_t) { return 0; }
void cloudRefreshContacts() {}
bool cloudContactsLoading() { return false; }
bool cloudSendText(const char *, const char *) { return false; }
bool cloudSendImage(const char *, const uint16_t *, uint16_t, uint16_t, const char *) { return false; }
CloudSendState cloudSendState(char *r, size_t c) { if (r && c) r[0] = 0; return CloudSendState::Idle; }
uint32_t cloudSendStateMs() { return 0; }
uint16_t cloudUnreadCount() { return 0; }
void cloudRefreshInbox() {}
bool cloudInboxLoading() { return false; }
uint32_t cloudInboxRevision() { return 0; }
size_t cloudInbox(CloudMsg *, size_t) { return 0; }
bool cloudInboxText(const char *, char *, size_t) { return false; }
void cloudMarkRead(const char *) {}
void cloudLoadImage(const char *) {}
CloudImgState cloudImage(const uint16_t **, uint16_t *, uint16_t *) { return CloudImgState::Failed; }
bool cloudTakeNotify() { return false; }
void cloudNotice(CloudNotice *o) { *o = CloudNotice{}; }
void cloudOpenWhenLoaded(const char *) {}
bool cloudTakeOpenId(char *, size_t) { return false; }
void cloudSetReplyTo(const CloudContact *) {}
bool cloudReplyTo(CloudContact *) { return false; }
void cloudCheckNow() {}
const char *cloudBaseUrl() { return ""; }
void cloudSetBaseUrl(const char *) {}
bool cloudConsole(const char *) { return false; }
#endif
