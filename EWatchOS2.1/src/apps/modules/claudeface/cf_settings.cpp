// NVS settings. See settings.h.
#include <Arduino.h>
#include <Preferences.h>
#include "cf_settings.h"

static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static CFSettings   sCur = { 23, 8, 15, true, 0 };
static const char  *NS = "claudeface";

static void sanitize(CFSettings &s) {
  if (s.quietStart > 23) s.quietStart = 23;
  if (s.quietEnd > 23)   s.quietEnd = 8;
  if (s.ambientPct > 100) s.ambientPct = 15;
  if (s.tzOffsetMin < -720 || s.tzOffsetMin > 840) s.tzOffsetMin = 0;
}

static void save(const CFSettings &s) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putUChar("qStart", s.quietStart);
  p.putUChar("qEnd", s.quietEnd);
  p.putUChar("ambient", s.ambientPct);
  p.putBool("haptics", s.haptics);
  p.putShort("tzOffMin", s.tzOffsetMin);
  p.putBool("init", true);
  p.end();
}

void settingsLoad() {
  CFSettings s = sCur;
  Preferences p;
  bool have = p.begin(NS, true) && p.getBool("init", false);
  if (have) {
    s.quietStart  = p.getUChar("qStart", s.quietStart);
    s.quietEnd    = p.getUChar("qEnd", s.quietEnd);
    s.ambientPct  = p.getUChar("ambient", s.ambientPct);
    s.haptics     = p.getBool("haptics", s.haptics);
    s.tzOffsetMin = p.getShort("tzOffMin", s.tzOffsetMin);
  }
  p.end();
  if (!have) {
    // One-time migration from AuroraOS' namespace (read-only).
    Preferences a;
    if (a.begin("ewatch", true)) {
      s.quietStart  = a.getUChar("cfQStart", s.quietStart);
      s.quietEnd    = a.getUChar("cfQEnd", s.quietEnd);
      s.ambientPct  = a.getUChar("cfAmbient", s.ambientPct);
      s.haptics     = a.getBool("cfHaptics", s.haptics);
      s.tzOffsetMin = a.getShort("tzOffMin", s.tzOffsetMin);
      a.end();
    }
    sanitize(s);
    save(s);
  }
  sanitize(s);
  portENTER_CRITICAL(&sMux);
  sCur = s;
  portEXIT_CRITICAL(&sMux);
}

CFSettings settingsGet() {
  portENTER_CRITICAL(&sMux);
  CFSettings s = sCur;
  portEXIT_CRITICAL(&sMux);
  return s;
}

// Apply one change under the lock, then persist outside it (NVS writes are
// slow and must not run in a critical section).
template <typename F>
static void change(F f) {
  portENTER_CRITICAL(&sMux);
  f(sCur);
  CFSettings s = sCur;
  portEXIT_CRITICAL(&sMux);
  save(s);
}

void settingsSetQuiet(uint8_t start, uint8_t end) {
  change([=](CFSettings &s) { s.quietStart = start; s.quietEnd = end; sanitize(s); });
}
void settingsSetAmbient(uint8_t pct) {
  change([=](CFSettings &s) { s.ambientPct = pct; sanitize(s); });
}
void settingsSetHaptics(bool on) {
  change([=](CFSettings &s) { s.haptics = on; });
}
void settingsSetTz(int16_t offsetMin) {
  if (settingsGet().tzOffsetMin == offsetMin) return;
  change([=](CFSettings &s) { s.tzOffsetMin = offsetMin; sanitize(s); });
}
