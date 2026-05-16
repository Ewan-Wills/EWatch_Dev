#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include "model.h"
#include "storage.h"

static Preferences prefs;
static const char *NS = "ewatch";

// Known networks: in-memory cache. All mutators take knownMutex.
namespace Storage {
struct KnownEntry {
  char ssid[KNOWN_SSID];
  char pass[KNOWN_PASS];
};
}
static Storage::KnownEntry known[Storage::KNOWN_MAX];
static uint8_t             knownN = 0;
static SemaphoreHandle_t   knownMutex = nullptr;

namespace {
struct KnownLock {
  KnownLock()  { if (knownMutex) xSemaphoreTake(knownMutex, portMAX_DELAY); }
  ~KnownLock() { if (knownMutex) xSemaphoreGive(knownMutex); }
};
}

void Storage::begin() {
  prefs.begin(NS, /*readOnly=*/false);
  if (!knownMutex) knownMutex = xSemaphoreCreateMutex();
}

void Storage::load() {
  ModelLock lk;
  model.wakeOnTouch      = prefs.getBool  ("wkTouch",   model.wakeOnTouch);
  model.wakeOnButton     = prefs.getBool  ("wkButton",  model.wakeOnButton);
  model.wakeOnImu        = prefs.getBool  ("wkImu",     model.wakeOnImu);
  model.sleepTimeoutSec  = prefs.getUShort("sleepSec",  model.sleepTimeoutSec);
  model.sleepToOffSec    = prefs.getUShort("sleepOff",  model.sleepToOffSec);
  model.imuWakeThreshold = prefs.getUChar ("imuThresh", model.imuWakeThreshold);
  model.brightness       = prefs.getUChar ("bright",    model.brightness);
  model.bgColor          = prefs.getUShort("bgColor",   model.bgColor);
  model.fgColor          = prefs.getUShort("fgColor",   model.fgColor);
  model.accentColor      = prefs.getUShort("accColor",  model.accentColor);
  model.lineColor        = prefs.getUShort("lineColor", model.lineColor);

  model.wifiEnabled      = prefs.getBool  ("wifiOn",    model.wifiEnabled);
  uint8_t modeRaw        = prefs.getUChar ("wifiMode",  (uint8_t)model.wifiMode);
  model.wifiMode         = (modeRaw <= 1) ? (WifiMode)modeRaw : WifiMode::AP;
  model.tzOffsetMin      = prefs.getShort ("tzOffMin",  model.tzOffsetMin);
  model.watchFaceStyle   = prefs.getUChar ("wfStyle",   model.watchFaceStyle);
  model.hapticStrength   = prefs.getUChar ("haptStr",   model.hapticStrength);

  // Known networks blob: read via getBytes into the cache.
  KnownLock klk;
  knownN = prefs.getUChar("knownN", 0);
  if (knownN > KNOWN_MAX) knownN = KNOWN_MAX;
  memset(known, 0, sizeof(known));
  size_t expect = (size_t)knownN * sizeof(KnownEntry);
  if (knownN > 0 && prefs.getBytesLength("known") == expect) {
    prefs.getBytes("known", known, expect);
  } else {
    knownN = 0;
  }
}

void Storage::save() {
  bool t, b, i, wEn;
  uint16_t to, off, bg, fg, ac, ln;
  uint8_t  th, br;
  int16_t  tz;
  uint8_t  wfs, hap;
  WifiMode wMode;
  { ModelLock lk;
    t   = model.wakeOnTouch;
    b   = model.wakeOnButton;
    i   = model.wakeOnImu;
    to  = model.sleepTimeoutSec;
    off = model.sleepToOffSec;
    th  = model.imuWakeThreshold;
    br  = model.brightness;
    bg  = model.bgColor;
    fg  = model.fgColor;
    ac  = model.accentColor;
    ln  = model.lineColor;
    wEn = model.wifiEnabled;
    wMode = model.wifiMode;
    tz  = model.tzOffsetMin;
    wfs = model.watchFaceStyle;
    hap = model.hapticStrength; }
  prefs.putBool  ("wkTouch",   t);
  prefs.putBool  ("wkButton",  b);
  prefs.putBool  ("wkImu",     i);
  prefs.putUShort("sleepSec",  to);
  prefs.putUShort("sleepOff",  off);
  prefs.putUChar ("imuThresh", th);
  prefs.putUChar ("bright",    br);
  prefs.putUShort("bgColor",   bg);
  prefs.putUShort("fgColor",   fg);
  prefs.putUShort("accColor",  ac);
  prefs.putUShort("lineColor", ln);
  prefs.putBool  ("wifiOn",    wEn);
  prefs.putUChar ("wifiMode",  (uint8_t)wMode);
  prefs.putShort ("tzOffMin",  tz);
  prefs.putUChar ("wfStyle",   wfs);
  prefs.putUChar ("haptStr",   hap);
}

uint8_t Storage::knownCount() { KnownLock klk; return knownN; }

bool Storage::knownAt(uint8_t i, char *ssidOut, char *passOut) {
  KnownLock klk;
  if (i >= knownN) return false;
  strncpy(ssidOut, known[i].ssid, KNOWN_SSID); ssidOut[KNOWN_SSID - 1] = '\0';
  strncpy(passOut, known[i].pass, KNOWN_PASS); passOut[KNOWN_PASS - 1] = '\0';
  return true;
}

bool Storage::knownUpsert(const char *ssid, const char *password) {
  if (!ssid || !*ssid) return false;
  size_t slen = strlen(ssid);
  if (slen >= KNOWN_SSID) return false;
  const char *p = password ? password : "";
  if (strlen(p) >= KNOWN_PASS) return false;

  KnownLock klk;
  for (uint8_t i = 0; i < knownN; i++) {
    if (strncmp(known[i].ssid, ssid, KNOWN_SSID) == 0) {
      strncpy(known[i].pass, p, KNOWN_PASS); known[i].pass[KNOWN_PASS - 1] = '\0';
      return true;
    }
  }
  if (knownN >= KNOWN_MAX) return false;
  strncpy(known[knownN].ssid, ssid, KNOWN_SSID); known[knownN].ssid[KNOWN_SSID - 1] = '\0';
  strncpy(known[knownN].pass, p,    KNOWN_PASS); known[knownN].pass[KNOWN_PASS - 1] = '\0';
  knownN++;
  return true;
}

bool Storage::knownRemove(uint8_t i) {
  KnownLock klk;
  if (i >= knownN) return false;
  for (uint8_t j = i; j + 1 < knownN; j++) known[j] = known[j + 1];
  knownN--;
  memset(&known[knownN], 0, sizeof(KnownEntry));
  return true;
}

bool Storage::knownLookup(const char *ssid, char *passOut) {
  if (!ssid) return false;
  KnownLock klk;
  for (uint8_t i = 0; i < knownN; i++) {
    if (strncmp(known[i].ssid, ssid, KNOWN_SSID) == 0) {
      strncpy(passOut, known[i].pass, KNOWN_PASS); passOut[KNOWN_PASS - 1] = '\0';
      return true;
    }
  }
  return false;
}

void Storage::knownSave() {
  uint8_t n;
  KnownEntry copy[KNOWN_MAX];
  { KnownLock klk; n = knownN; memcpy(copy, known, sizeof(copy)); }
  prefs.putUChar("knownN", n);
  if (n > 0) prefs.putBytes("known", copy, (size_t)n * sizeof(KnownEntry));
  else       prefs.remove("known");
}
