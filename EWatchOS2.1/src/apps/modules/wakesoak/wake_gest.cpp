// Wake gesture traces — see wake_gest.h.
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include <stdio.h>

#include "wake_gest.h"
#include "wake_soak.h"
#include "ewlog.h"

static const char    *kGestPath     = "/wakegest.bin";
static const size_t   kGestCapBytes = 320 * 1024;   // ~250-400 wakes
static const size_t   kFsReserve    = 48 * 1024;    // never fill the FS (logs live there too)
static const int      kPreMax = 32, kChkMax = 150, kPostMax = 40;
static const uint32_t kPostMs = 1000;

static int16_t  sPre[kPreMax][3];
static int16_t  sChk[kChkMax][3];
static struct { uint16_t t; int16_t a[3]; } sPost[kPostMax];

static uint32_t sNextId = 0;                 // 0 = not loaded from the file yet
static uint32_t sId = 0;                     // current trace (0 = none)
static uint8_t  sSrc = 0, sWhy = 255, sOk = 0, sAnswer = 255;
static uint16_t sCheckMs = 0;
static int16_t  sBase[3] = { 0, 0, 0 };
static uint32_t sTrigMs = 0;
static uint8_t  sPreN = 0;
static volatile uint16_t sChkN = 0, sChkSkip = 0;
static volatile uint8_t  sPostN = 0;
static volatile bool     sPostOn = false, sPostDone = false, sPostStarted = false;
static uint32_t sPostStartMs = 0;
static bool     sFull = false;
static uint32_t sTraces = 0;                 // records in the file

static void loadCounts() {
  if (sNextId) return;
  sTraces = 0;
  uint32_t lastId = 0;
  File f = LittleFS.open(kGestPath, FILE_READ);
  if (f) {
    uint8_t h[28];
    while (f.available() >= (int)sizeof h) {
      if (f.read(h, sizeof h) != sizeof h || h[0] != 'W' || h[1] != 'G') break;
      memcpy(&lastId, h + 4, 4);
      uint8_t preN = h[24]; uint16_t chkN; memcpy(&chkN, h + 25, 2); uint8_t postN = h[27];
      size_t body = (size_t)preN * 6 + (size_t)chkN * 6 + (size_t)postN * 8;
      if (!f.seek(f.position() + body)) break;
      sTraces++;
    }
    sFull = f.size() >= kGestCapBytes;
    f.close();
  }
  sNextId = lastId + 1;
}

uint32_t gestCurrentId() { return sId; }

static void writeTrace() {
  if (!sId) return;
  sPostOn = false;
  uint8_t postN = sPostN;
  size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
  File f = LittleFS.open(kGestPath, FILE_APPEND);
  if (!f) { EWLOGW("GEST", "open failed"); sId = 0; return; }
  if (f.size() >= kGestCapBytes || freeB < kFsReserve) {
    if (!sFull) EWLOGW("GEST", "trace file full (%u B, fs free %u B): traces stop",
                       (unsigned)f.size(), (unsigned)freeB);
    sFull = true; f.close(); sId = 0; return;
  }
  uint8_t h[28];
  h[0] = 'W'; h[1] = 'G'; h[2] = 1; h[3] = sSrc;
  memcpy(h + 4, &sId, 4);
  memcpy(h + 8, &sTrigMs, 4);
  h[12] = sWhy; h[13] = sOk;
  memcpy(h + 14, &sCheckMs, 2);
  memcpy(h + 16, sBase, 6);
  h[22] = sAnswer; h[23] = 0;
  h[24] = sPreN;
  uint16_t chkN = sChkN; memcpy(h + 25, &chkN, 2);
  h[27] = postN;
  f.write(h, sizeof h);
  if (sPreN) f.write((const uint8_t *)sPre, (size_t)sPreN * 6);
  if (chkN)  f.write((const uint8_t *)sChk, (size_t)chkN * 6);
  if (postN) f.write((const uint8_t *)sPost, (size_t)postN * 8);
  f.close();
  sTraces++;
  EWLOGD("GEST", "id=%lu src=%u why=%u ans=%u pre=%u chk=%u post=%u",
         (unsigned long)sId, sSrc, sWhy, sAnswer, sPreN, chkN, postN);
  sId = 0;
}

uint32_t gestBegin(uint8_t src) {
  if (!soakEnabled()) return 0;
  loadCounts();
  if (sId) writeTrace();                    // an unanswered one: keep what it has
  if (sFull) return 0;
  sId = sNextId++;
  sSrc = src; sWhy = 255; sOk = 0; sAnswer = 255; sCheckMs = 0;
  sBase[0] = sBase[1] = sBase[2] = 0;
  sTrigMs = millis();
  sPreN = 0; sChkN = 0; sChkSkip = 0; sPostN = 0;
  sPostOn = false; sPostDone = false; sPostStarted = false;
  return sId;
}

void gestPre(const int16_t (*xyz)[3], int n) {
  if (!sId) return;
  if (n > kPreMax) { xyz += n - kPreMax; n = kPreMax; }
  memcpy(sPre, xyz, (size_t)n * 6);
  sPreN = (uint8_t)n;
}

void gestBase(int16_t x, int16_t y, int16_t z) {
  if (!sId) return;
  sBase[0] = x; sBase[1] = y; sBase[2] = z;
}

void gestCheckSample(int16_t x, int16_t y, int16_t z) {
  if (!sId) return;
  if (sChkSkip++ & 1) return;               // 200 Hz -> keep 100 Hz
  uint16_t n = sChkN;
  if (n >= kChkMax) return;
  sChk[n][0] = x; sChk[n][1] = y; sChk[n][2] = z;
  sChkN = n + 1;
}

void gestVerdict(uint8_t why, bool ok, uint16_t checkMs) {
  if (!sId) return;
  sWhy = why; sOk = ok ? 1 : 0; sCheckMs = checkMs;
}

void gestPostStart() {
  if (!sId) return;
  sPostN = 0; sPostStartMs = millis();
  sPostStarted = true; sPostDone = false; sPostOn = true;
}

void gestFeed(int16_t x, int16_t y, int16_t z) {
  if (!sPostOn) return;
  uint32_t now = millis();
  uint8_t n = sPostN;
  if (n >= kPostMax || now - sPostStartMs >= kPostMs) { sPostOn = false; sPostDone = true; return; }
  sPost[n].t = (uint16_t)(now - sTrigMs);
  sPost[n].a[0] = x; sPost[n].a[1] = y; sPost[n].a[2] = z;
  sPostN = n + 1;
}

void gestAnswer(uint32_t id, const char *a) {
  if (!sId || id != sId) return;
  sAnswer = !strcmp(a, "yes") ? 0 : !strcmp(a, "no") ? 1 : !strcmp(a, "timeout") ? 2
          : !strcmp(a, "rejected") ? 3 : !strcmp(a, "auto_yes") ? 4 : 5;
  if (!sPostStarted) writeTrace();          // rejected: no screen, no post phase
}

void gestPoll() {
  if (!sId || sAnswer == 255 || !sPostStarted) return;
  if (!sPostDone && millis() - sPostStartMs < kPostMs + 200) return;
  writeTrace();
}

void gestAbandon() { sId = 0; sPostOn = false; }

void gestClear() {
  LittleFS.remove(kGestPath);
  sId = 0; sPostOn = false; sFull = false; sTraces = 0; sNextId = 1;
}

void gestStats(char *out, int n) {
  loadCounts();
  size_t sz = 0;
  File f = LittleFS.open(kGestPath, FILE_READ);
  if (f) { sz = f.size(); f.close(); }
  snprintf(out, n, "traces=%lu bytes=%u cap=%u full=%d fs_free=%u",
           (unsigned long)sTraces, (unsigned)sz, (unsigned)kGestCapBytes, sFull ? 1 : 0,
           (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
}

// `soak gget FROM COUNT`: the file as hex, 96 bytes per line, each hashed
//   GT <idx> <fnv1a32 hex> <hex>       then   GT end <total lines> <bytes>
// (tools/soak_pull.py re-asks damaged lines, as with `soak get`).
bool gestHandleLine(const char *a) {
  if (strncmp(a, "gget", 4)) return false;
  unsigned long from = 0, cnt = 1000000;
  sscanf(a + 4, "%lu %lu", &from, &cnt);
  File f = LittleFS.open(kGestPath, FILE_READ);
  size_t size = f ? f.size() : 0;
  uint32_t lines = (uint32_t)((size + 95) / 96);
  Serial.setTxTimeoutMs(1000);
  static const char hx[] = "0123456789abcdef";
  uint8_t b[96]; char out[16 + 8 + 2 * 96 + 8];
  for (uint32_t i = from; f && i < lines && i < from + cnt; i++) {
    if (!f.seek((size_t)i * 96)) break;
    int n = f.read(b, sizeof b);
    if (n <= 0) break;
    uint32_t h = 2166136261u;
    char hexs[2 * 96 + 1];
    for (int k = 0; k < n; k++) { hexs[2 * k] = hx[b[k] >> 4]; hexs[2 * k + 1] = hx[b[k] & 15]; }
    hexs[2 * n] = 0;
    for (int k = 0; k < 2 * n; k++) { h ^= (uint8_t)hexs[k]; h *= 16777619u; }
    int m = snprintf(out, sizeof out, "GT %lu %08lx %s\n", (unsigned long)i, (unsigned long)h, hexs);
    Serial.write((const uint8_t *)out, m);
    if ((i & 7) == 7) vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (f) f.close();
  Serial.printf("GT end %lu %u\n", (unsigned long)lines, (unsigned)size);
  Serial.flush();
  Serial.setTxTimeoutMs(100);
  return true;
}
