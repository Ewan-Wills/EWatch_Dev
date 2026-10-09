#include <LittleFS.h>
#include <Preferences.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include "ewlog.h"
#include "photo_store.h"

static const char *kDir = "/img";
static const int16_t W = 240, H = 280;

static SemaphoreHandle_t sMutex = nullptr;
static char sNames[PHOTO_MAX][16];
static int  sCount = 0;

static char     sBgName[16] = "";
static uint32_t sBgGen = 1;          // bumped on every bg change (decode trigger)

// Render-task decode state.
static uint16_t *sBgCache = nullptr; // 240x280 PSRAM, current bg decoded
static uint32_t  sBgCachedGen = 0;
static bool      sBgCacheValid = false;

struct Lk {
  Lk()  { if (sMutex) xSemaphoreTake(sMutex, portMAX_DELAY); }
  ~Lk() { if (sMutex) xSemaphoreGive(sMutex); }
};

void photoStoreInit() {
  if (!sMutex) sMutex = xSemaphoreCreateMutex();
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  Preferences p;
  p.begin("photo", true);
  String bg = p.getString("bg", "");
  p.end();
  strncpy(sBgName, bg.c_str(), sizeof(sBgName) - 1);
  photoRefresh();
  EWLOGI("PHOTO", "store init n=%d bg=\"%s\"", sCount, sBgName);
}

void photoRefresh() {
  Lk lk;
  sCount = 0;
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile()) && sCount < PHOTO_MAX) {
    const char *n = f.name();                  // basename on arduino-esp32 2.x
    const char *base = strrchr(n, '/');
    base = base ? base + 1 : n;
    if (strlen(base) < sizeof(sNames[0]) && strstr(base, ".jpg")) {
      strncpy(sNames[sCount], base, sizeof(sNames[0]) - 1);
      sNames[sCount][sizeof(sNames[0]) - 1] = '\0';
      sCount++;
    }
    f.close();
  }
}

int photoCount() { Lk lk; return sCount; }

bool photoName(int idx, char out[16]) {
  Lk lk;
  if (idx < 0 || idx >= sCount) return false;
  strncpy(out, sNames[idx], 16);
  out[15] = '\0';
  return true;
}

bool photoNextSlot(char out[16]) {
  Lk lk;
  for (int i = 0; i < PHOTO_MAX; i++) {
    char cand[16], path[24];
    snprintf(cand, sizeof cand, "p%02d.jpg", i);
    snprintf(path, sizeof path, "%s/%s", kDir, cand);
    if (!LittleFS.exists(path)) { strncpy(out, cand, 16); return true; }
  }
  return false;
}

bool photoDelete(const char *name) {
  if (!name || !name[0] || strstr(name, "..") || strchr(name, '/')) return false;
  char path[24];
  snprintf(path, sizeof path, "%s/%s", kDir, name);
  bool ok = LittleFS.remove(path);
  if (ok && strcmp(name, sBgName) == 0) photoSetBg("");
  photoRefresh();
  EWLOGI("PHOTO", "delete \"%s\" ok=%d", name, ok ? 1 : 0);
  return ok;
}

size_t photoFsFree() {
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}

void photoSetBg(const char *name) {
  {
    Lk lk;
    strncpy(sBgName, (name && name[0]) ? name : "", sizeof(sBgName) - 1);
    sBgName[sizeof(sBgName) - 1] = '\0';
    sBgGen++;
  }
  Preferences p;
  p.begin("photo", false);
  p.putString("bg", sBgName);
  p.end();
  EWLOGI("PHOTO", "bg=\"%s\"", sBgName);
}

const char *photoBgName() { return sBgName; }

// ---------------------------------------------------------------------------
// Decode (render task only)
// ---------------------------------------------------------------------------
static JPEGDEC sJpeg;
static uint16_t *sDst = nullptr;
static int sOffX = 0, sOffY = 0;      // centering offset (can be negative = crop)

static int decodeDraw(JPEGDRAW *p) {
  for (int row = 0; row < p->iHeight; row++) {
    int dy = sOffY + p->y + row;
    if (dy < 0 || dy >= H) continue;
    int sx0 = 0, sx1 = p->iWidth;
    int dx = sOffX + p->x;
    if (dx < 0)            sx0 = -dx;
    if (dx + p->iWidth > W) sx1 = W - dx;
    if (sx0 >= sx1) continue;
    memcpy(sDst + (size_t)dy * W + dx + sx0,
           &p->pPixels[row * p->iWidth + sx0],
           (size_t)(sx1 - sx0) * 2);
  }
  return 1;
}

bool photoDecodeTo(const char *name, uint16_t *dst) {
  if (!name || !name[0] || !dst) return false;
  char path[24];
  snprintf(path, sizeof path, "%s/%s", kDir, name);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t len = f.size();
  if (len == 0 || len > PHOTO_MAX_BYTES) { f.close(); return false; }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) { f.close(); return false; }
  size_t rd = f.read(buf, len);
  f.close();
  bool ok = false;
  if (rd == len && sJpeg.openRAM(buf, len, decodeDraw)) {
    sJpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    int sw = sJpeg.getWidth(), sh = sJpeg.getHeight();
    // Fill the frame with black first so undersized images letterbox cleanly.
    memset(dst, 0, (size_t)W * H * 2);
    sDst  = dst;
    sOffX = (W - sw) / 2;
    sOffY = (H - sh) / 2;
    ok = sJpeg.decode(0, 0, 0) == 1;
    sJpeg.close();
  }
  heap_caps_free(buf);
  if (!ok) EWLOGW("PHOTO", "decode failed \"%s\"", name);
  return ok;
}

const uint16_t *photoBgGet() {
  uint32_t gen;
  char name[16];
  {
    Lk lk;
    gen = sBgGen;
    strncpy(name, sBgName, sizeof name);
    name[sizeof(name) - 1] = '\0';
  }
  if (sBgCachedGen == gen) return sBgCacheValid ? sBgCache : nullptr;

  // Background changed — (re)decode into the PSRAM cache.
  sBgCachedGen = gen;
  sBgCacheValid = false;
  if (!name[0]) return nullptr;
  if (!sBgCache) {
    sBgCache = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    if (!sBgCache) return nullptr;
  }
  sBgCacheValid = photoDecodeTo(name, sBgCache);
  return sBgCacheValid ? sBgCache : nullptr;
}
