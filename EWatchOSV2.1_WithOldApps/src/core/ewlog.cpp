#include <stdarg.h>
#include <string.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "ewlog.h"
#include "power_mgr.h"     // powerStatsPrint / powerSelfTest for the console

// ---------------------------------------------------------------------------
// Sinks + shared state
// ---------------------------------------------------------------------------
static SemaphoreHandle_t sLogMutex = nullptr;
static volatile uint8_t  sRuntimeLevel = EW_LOG_LEVEL;
static char              sState[6] = "BOOT";
static bool              sFsReady = false;

// FS ring: two halves, append to the active one, rotate at half-cap.
// 2 x 128 KB = 256 KB total, oldest-overwrite by truncating the other half.
static const char  *kRing0 = "/ewlog0.txt";
static const char  *kRing1 = "/ewlog1.txt";
static const size_t kRingHalfCap = 128 * 1024;
static int          sActiveRing = 0;      // 0 or 1
static size_t       sActiveSize = 0;

// RTC-RAM mirror: survives crash resets and deep sleep (not power loss).
// Simple wrap-around ring of raw line bytes.
static const uint32_t kRtcMagic = 0x45574C47;   // "EWLG"
struct RtcMirror {
  uint32_t magic;
  uint16_t head;         // next write offset
  uint16_t used;         // bytes valid (<= size)
  char     buf[2048];
};
static RTC_NOINIT_ATTR RtcMirror sRtc;

static void rtcMirrorAppend(const char *line, size_t n) {
  for (size_t i = 0; i < n; i++) {
    sRtc.buf[sRtc.head] = line[i];
    sRtc.head = (uint16_t)((sRtc.head + 1) % sizeof(sRtc.buf));
    if (sRtc.used < sizeof(sRtc.buf)) sRtc.used++;
  }
}

// Recovered mirror content, staged in plain RAM at boot so the RTC copy can
// be wiped IMMEDIATELY. In a crash loop the wipe used to wait for the FS
// mount — which the crash never reached — so the mirror grew a line per
// reboot and replayed the whole history every cycle, forever.
static char     sMirrorCopy[sizeof(sRtc.buf)];
static uint16_t sMirrorCopyLen = 0;

void ewlogWipeRtcMirror() {
  sRtc.magic = kRtcMagic;
  sRtc.head = 0;
  sRtc.used = 0;
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------
static const char kLevelCh[4] = { 'E', 'W', 'I', 'D' };

static void fsAppend(const char *line, size_t n) {
  if (!sFsReady) return;
  const char *path = sActiveRing ? kRing1 : kRing0;
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) { sFsReady = false; return; }     // don't loop on a broken FS
  f.write((const uint8_t *)line, n);
  sActiveSize = f.size();
  f.close();
  if (sActiveSize >= kRingHalfCap) {
    // Rotate: the other half becomes active and is truncated (oldest data
    // dropped). Worst case we always retain the most recent 128 KB.
    sActiveRing ^= 1;
    LittleFS.remove(sActiveRing ? kRing1 : kRing0);
    sActiveSize = 0;
  }
}

void ewlogWrite(uint8_t level, const char *tag, const char *fmt, ...) {
  if (level > sRuntimeLevel) return;

  char line[224];
  int off = snprintf(line, sizeof(line), "[%lu|%s] %c/%s: ",
                     (unsigned long)millis(), sState, kLevelCh[level & 3], tag);
  if (off < 0) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line + off, sizeof(line) - off - 2, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  size_t len = (size_t)off + (size_t)n;
  if (len > sizeof(line) - 2) len = sizeof(line) - 2;
  line[len++] = '\n';
  line[len] = '\0';

  if (sLogMutex) xSemaphoreTake(sLogMutex, portMAX_DELAY);
  if (Serial) Serial.write((const uint8_t *)line, len);
  rtcMirrorAppend(line, len);
  fsAppend(line, len);
  if (sLogMutex) xSemaphoreGive(sLogMutex);
}

void ewlogSetState(const char *stateAbbrev) {
  strncpy(sState, stateAbbrev, sizeof(sState) - 1);
  sState[sizeof(sState) - 1] = '\0';
}

void ewlogSetRuntimeLevel(uint8_t level) {
  if (level > EWLOG_DEBUG) level = EWLOG_DEBUG;
  sRuntimeLevel = level;
  Preferences p;
  p.begin("ewatch", false);
  p.putUChar("logLvl", level);
  p.end();
}

uint8_t ewlogRuntimeLevel() { return sRuntimeLevel; }

// ---------------------------------------------------------------------------
// Init / recovery
// ---------------------------------------------------------------------------
void ewlogInit() {
  sLogMutex = xSemaphoreCreateMutex();

  Preferences p;
  p.begin("ewatch", true);
  sRuntimeLevel = p.getUChar("logLvl", EW_LOG_LEVEL);
  p.end();
  if (sRuntimeLevel > EWLOG_DEBUG) sRuntimeLevel = EWLOG_DEBUG;

  // A valid mirror from before this boot? Stage it into RAM, replay it to
  // serial, and wipe the RTC copy RIGHT NOW — never leave it to a later
  // init stage a crash loop might not reach. ewlogMountFs() persists the
  // staged copy into the ring.
  if (sRtc.magic != kRtcMagic || sRtc.used > sizeof(sRtc.buf) ||
      sRtc.head >= sizeof(sRtc.buf)) {
    ewlogWipeRtcMirror();
  } else if (sRtc.used > 0) {
    uint16_t start = (uint16_t)((sRtc.head + sizeof(sRtc.buf) - sRtc.used)
                                % sizeof(sRtc.buf));
    sMirrorCopyLen = sRtc.used;
    for (uint16_t i = 0; i < sMirrorCopyLen; i++) {
      sMirrorCopy[i] = sRtc.buf[(start + i) % sizeof(sRtc.buf)];
    }
    ewlogWipeRtcMirror();
    if (Serial) {
      Serial.println("--- ewlog: RTC mirror from previous run ---");
      Serial.write((const uint8_t *)sMirrorCopy, sMirrorCopyLen);
      Serial.println("--- end RTC mirror ---");
    }
  }
}

void ewlogMountFs() {
  // formatOnFail: first boot (or corruption) formats the data partition.
  if (!LittleFS.begin(true)) {
    EWLOGE("ERR", "littlefs mount failed - fs sink disabled");
    return;
  }
  // Resume the fuller half so rotation order stays sane across boots.
  // (exists() first — opening a missing file for read logs a core error.)
  size_t s0 = 0, s1 = 0;
  if (LittleFS.exists(kRing0)) {
    File f0 = LittleFS.open(kRing0, FILE_READ);
    if (f0) { s0 = f0.size(); f0.close(); }
  }
  if (LittleFS.exists(kRing1)) {
    File f1 = LittleFS.open(kRing1, FILE_READ);
    if (f1) { s1 = f1.size(); f1.close(); }
  }
  sActiveRing = (s1 > s0) ? 1 : 0;
  sActiveSize = (s1 > s0) ? s1 : s0;
  sFsReady = true;

  // Persist the mirror content staged by ewlogInit() (the RTC copy is
  // already wiped — see the crash-loop note above).
  if (sMirrorCopyLen > 0) {
    if (sLogMutex) xSemaphoreTake(sLogMutex, portMAX_DELAY);
    const char *hdr = "--- RTC mirror (previous run) ---\n";
    fsAppend(hdr, strlen(hdr));
    fsAppend(sMirrorCopy, sMirrorCopyLen);
    const char *ftr = "--- end RTC mirror ---\n";
    fsAppend(ftr, strlen(ftr));
    sMirrorCopyLen = 0;
    if (sLogMutex) xSemaphoreGive(sLogMutex);
  }
}

// ---------------------------------------------------------------------------
// Serial console
// ---------------------------------------------------------------------------
static void dumpFile(const char *path) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  uint8_t buf[256];
  while (true) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    Serial.write(buf, n);
  }
  f.close();
}

static void cmdLogDump() {
  if (!sFsReady) { Serial.println("(fs sink not mounted)"); return; }
  if (sLogMutex) xSemaphoreTake(sLogMutex, portMAX_DELAY);
  Serial.println("=== ewlog ring dump (oldest first) ===");
  // Inactive half holds the older data.
  dumpFile(sActiveRing ? kRing0 : kRing1);
  dumpFile(sActiveRing ? kRing1 : kRing0);
  Serial.println("=== end dump ===");
  if (sLogMutex) xSemaphoreGive(sLogMutex);
}

static void cmdLogClear() {
  if (sLogMutex) xSemaphoreTake(sLogMutex, portMAX_DELAY);
  if (sFsReady) { LittleFS.remove(kRing0); LittleFS.remove(kRing1); }
  sActiveRing = 0;
  sActiveSize = 0;
  sRtc.head = 0;
  sRtc.used = 0;
  if (sLogMutex) xSemaphoreGive(sLogMutex);
  Serial.println("log cleared");
}

static void handleLine(char *line) {
  // Trim trailing CR/LF and leading spaces.
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
  while (*line == ' ') line++;
  if (!*line) return;

  if (strcmp(line, "help") == 0) {
    Serial.println("commands: log dump | log clear | log level <0-3> | "
                   "pwr stats | pwr selftest | pwr set <dim|off|deep|sync> <n>");
  } else if (strcmp(line, "log dump") == 0) {
    cmdLogDump();
  } else if (strcmp(line, "log clear") == 0) {
    cmdLogClear();
  } else if (strncmp(line, "log level ", 10) == 0) {
    int lvl = atoi(line + 10);
    ewlogSetRuntimeLevel((uint8_t)(lvl < 0 ? 0 : lvl));
    Serial.printf("log level = %u\n", (unsigned)ewlogRuntimeLevel());
  } else if (strcmp(line, "pwr stats") == 0) {
    powerStatsPrint(Serial);
  } else if (strcmp(line, "pwr selftest") == 0) {
    powerRequestSelfTest();
    Serial.println("selftest queued (runs on the render task; watch WAKE logs)");
  } else if (strncmp(line, "pwr set ", 8) == 0) {
    char key[8] = "";
    int  val = -1;
    if (sscanf(line + 8, "%7s %d", key, &val) == 2 && val >= 0 &&
        val <= 65535 && powerSetConfig(key, (uint16_t)val)) {
      Serial.printf("pwr %s = %d\n", key, val);
    } else {
      Serial.println("usage: pwr set <dim|off|deep|sync> <n>");
    }
  } else {
    Serial.printf("unknown: '%s' (try help)\n", line);
  }
}

static void consoleTask(void *) {
  static char line[96];
  size_t pos = 0;
  for (;;) {
    while (Serial && Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        line[pos] = '\0';
        if (pos) handleLine(line);
        pos = 0;
      } else if (pos < sizeof(line) - 1) {
        line[pos++] = c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void ewlogStartConsole() {
  // Low priority, core 0 — the console is a debug convenience and must never
  // compete with the render/I-O tasks.
  xTaskCreatePinnedToCore(consoleTask, "ewcon", 4096, nullptr, 1, nullptr, 0);
}
