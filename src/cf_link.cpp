// Claude Face — USB-CDC link service. See cf_link.h for the contract.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "cf_link.h"
#include "cf_logic.h"
#include "cf_util.h"
#include "cf_policy.h"
#include "claude_face.h"
#include "board.h"          // rtcRequestSet (the UI loop owns Wire)
#include "settings.h"
#include "serial_io.h"

// ---------------------------------------------------------------------------
// Snapshot: single writer (link task) / UI-loop reader.
// ---------------------------------------------------------------------------
static portMUX_TYPE sSnapMux = portMUX_INITIALIZER_UNLOCKED;
struct CFPortMux {
  void lock()   { portENTER_CRITICAL(&sSnapMux); }
  void unlock() { portEXIT_CRITICAL(&sSnapMux); }
};
static CFPortMux sMux;
static CFDoubleBuffer<CFLinkSnap, CFPortMux> sBuf(sMux);

uint32_t cfLinkRead(CFLinkSnap &out) { return sBuf.read(out); }

bool cfLinkLive(const CFLinkSnap &s, uint32_t nowMs) {
  return s.everValid && (uint32_t)(nowMs - s.lastValidMs) < CF_LINK_LIVE_MS;
}
bool cfLinkVersionMismatch(const CFLinkSnap &s, uint32_t nowMs) {
  return s.mismatch && (uint32_t)(nowMs - s.mismatchMs) < CF_LINK_LIVE_MS;
}

// ---------------------------------------------------------------------------
// Counters (link task writes; `cf stats` reads — same task).
// ---------------------------------------------------------------------------
static uint32_t sFramesOk = 0, sFramesBad = 0, sFramesVer = 0;
static uint32_t sStateFrames = 0, sSessFrames = 0, sTimeFrames = 0;
static uint32_t sQueries = 0, sHellos = 0, sTimeSets = 0;
static volatile uint32_t sEvSent = 0, sEvDropped = 0;
static uint16_t sLastStateSeq = 0;
static bool     sWasLive = false;

// ---------------------------------------------------------------------------
// Watch -> host
// ---------------------------------------------------------------------------
static QueueHandle_t sEvQ = nullptr;
static uint16_t      sEvSeq = 0;
static bool          sWasConn = false;
static bool          sHelloSent = false;

void cfLinkInit() {
  if (!sEvQ) sEvQ = xQueueCreate(8, sizeof(CFEventMsg));
}

static void writeFrame(char type, const void *payload, size_t len) {
  char line[96];
  size_t n = cfEncodeFrame(type, payload, len, line, sizeof line);
  if (n == 0 || !Serial) return;
  serialWriteRaw(line, n);         // one write under the Serial mutex
}

static void sendHello() {
  CFHelloMsg h;
  memset(&h, 0, sizeof h);
  h.version = CF_PROTO_VERSION;
  strncpy(h.fw, CF_FW_ID, sizeof(h.fw) - 1);
  writeFrame(CF_TYPE_HELLO, &h, sizeof h);
  sHellos++;
}

void cfLinkSendEvent(uint8_t code, uint8_t arg) {
  if (!sEvQ) { sEvDropped++; return; }
  CFEventMsg ev;
  ev.version = CF_PROTO_VERSION;
  ev.code = code;
  ev.arg = arg;
  ev.seq = 0;                      // stamped by the link task on send
  if (xQueueSend(sEvQ, &ev, 0) != pdPASS) sEvDropped++;
}

void cfLinkService() {
  bool conn = (bool)Serial;
  // HELLO on boot and whenever a host (re)attaches.
  if (conn && (!sWasConn || !sHelloSent)) {
    sendHello();
    sHelloSent = true;
  }
  sWasConn = conn;

  CFEventMsg ev;
  while (sEvQ && xQueueReceive(sEvQ, &ev, 0) == pdPASS) {
    ev.seq = ++sEvSeq;
    if (conn) {
      writeFrame(CF_TYPE_EVENT, &ev, sizeof ev);
      sEvSent++;
      logf("CF", "event code=%u arg=%u seq=%u", ev.code, ev.arg, ev.seq);
    } else {
      sEvDropped++;
    }
  }

  // Liveness edge for the log (frames themselves are not logged — the FS
  // ring would fill with heartbeats).
  CFLinkSnap s;
  cfLinkRead(s);
  bool live = cfLinkLive(s, millis());
  if (sWasLive && !live) logf("CF", "link=offline ok=%lu bad=%lu",
                                (unsigned long)sFramesOk, (unsigned long)sFramesBad);
  sWasLive = live;
}

// ---------------------------------------------------------------------------
// Host -> watch
// ---------------------------------------------------------------------------
static void applyTime(const CFTimeMsg &t) {
  // Sanity: 2020-01-01 .. 2099-12-31 and a real-world offset.
  if (t.epoch < 1577836800UL || t.epoch > 4102444799UL ||
      t.utc_offset_min < -720 || t.utc_offset_min > 840) {
    logf("CF", "time rejected epoch=%lu off=%d",
         (unsigned long)t.epoch, (int)t.utc_offset_min);
    return;
  }
  // RTC = local wall time (same convention as AuroraOS' NTP sync).
  int64_t local = (int64_t)t.epoch + (int64_t)t.utc_offset_min * 60;
  CFCivil c = cfCivilFromEpoch(local);
  RtcTime r;
  r.year = c.year; r.month = c.month; r.day = c.day; r.weekday = c.wday;
  r.hour = c.hour; r.minute = c.minute; r.second = c.second;
  rtcRequestSet(r);
  sTimeSets++;
  settingsSetTz(t.utc_offset_min);   // face epoch maths; saved only on change
  logf("CF", "time_set %04u-%02u-%02u %02u:%02u:%02u off=%d",
       c.year, c.month, c.day, c.hour, c.minute, c.second,
       (int)t.utc_offset_min);
}

void cfLinkFeedLine(const char *line) {
  static CFFrame f;                // console task only; 132 B off the stack
  uint32_t now = millis();
  CFParseResult r = cfParseLine(line, f);
  switch (r) {
    case CF_PARSE_QUERY:
      sQueries++;
      sendHello();
      return;

    case CF_PARSE_VERSION: {
      sFramesVer++;
      CFLinkSnap &b = sBuf.beginWrite();
      if (!b.mismatch || (uint32_t)(now - b.mismatchMs) >= CF_LINK_LIVE_MS)
        logf("CF", "version_mismatch got=%u want=%u", f.version, CF_PROTO_VERSION);
      b.mismatch = true;
      b.mismatchMs = now;
      sBuf.publish();
      return;
    }

    case CF_PARSE_OK:
      break;

    case CF_PARSE_BAD:
    default:
      sFramesBad++;
      if (sFramesBad <= 5 || sFramesBad % 100 == 0)
        logf("CF", "bad_frame n=%lu", (unsigned long)sFramesBad);
      return;
  }

  // Valid frame.
  sFramesOk++;
  CFTimeMsg tm;
  bool isTime = false;
  CFLinkSnap &b = sBuf.beginWrite();
  if (!b.everValid || (uint32_t)(now - b.lastValidMs) >= CF_LINK_LIVE_MS)
    logf("CF", "link=up type=%c", f.type);
  b.everValid   = true;
  b.lastValidMs = now;
  b.mismatch    = false;
  switch (f.type) {
    case CF_TYPE_STATE: {
      CFStateMsg st;
      memcpy(&st, f.payload, sizeof st);
      b.st = st;
      b.haveState = true;
      b.stateRxMs = now;
      b.stateRxCount++;
      sStateFrames++;
      sLastStateSeq = st.seq;
      break;
    }
    case CF_TYPE_SESSIONS:
      memcpy(&b.ss, f.payload, sizeof b.ss);
      if (b.ss.count > 4) b.ss.count = 4;
      b.haveSessions = true;
      sSessFrames++;
      break;
    case CF_TYPE_TIME:
      memcpy(&tm, f.payload, sizeof tm);
      isTime = true;
      sTimeFrames++;
      break;
    default:
      break;
  }
  sBuf.publish();                   // the UI loop picks it up within ~10 ms

  if (isTime) applyTime(tm);
}

// One console reply line, one write.
static void serialLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void serialLine(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = (size_t)n < sizeof buf - 2 ? (size_t)n : sizeof buf - 2;
  buf[len++] = '\n';
  serialWriteRaw(buf, len);
}

// ---------------------------------------------------------------------------
// Console: `cf stats`, `cf set quiet <start> <end>`, `cf set ambient <pct>`,
// `cf set haptics <0|1>`, `cf buzz <blocked|done|cross|reset>`, `cf hello`.
// ---------------------------------------------------------------------------
static void printStats() {
  SerialLock lk;                    // one uninterrupted block
  CFLinkSnap s;
  uint32_t gen = cfLinkRead(s);
  uint32_t now = millis();
  const char *state = !s.everValid ? "never" : cfLinkLive(s, now) ? "live" : "offline";
  Serial.printf("CF LINK state=%s last_valid_ms_ago=%lu gen=%lu mismatch=%d\n",
                state,
                s.everValid ? (unsigned long)(now - s.lastValidMs) : 0UL,
                (unsigned long)gen, cfLinkVersionMismatch(s, now) ? 1 : 0);
  Serial.printf("  frames ok=%lu bad=%lu version=%lu state=%lu sessions=%lu "
                "time=%lu queries=%lu\n",
                (unsigned long)sFramesOk, (unsigned long)sFramesBad,
                (unsigned long)sFramesVer, (unsigned long)sStateFrames,
                (unsigned long)sSessFrames, (unsigned long)sTimeFrames,
                (unsigned long)sQueries);
  Serial.printf("  hello=%lu time_sets=%lu events_sent=%lu events_dropped=%lu "
                "last_state_seq=%u\n",
                (unsigned long)sHellos, (unsigned long)sTimeSets,
                (unsigned long)sEvSent, (unsigned long)sEvDropped, sLastStateSeq);
  if (s.haveState)
    Serial.printf("  global=%u primary=%u p_state=%u sessions=%u blocked=%u "
                  "5h=%u 7d=%u\n",
                  s.st.global_state, s.st.primary_index, s.st.p_state,
                  s.st.session_count, s.st.blocked_count, s.st.five_used,
                  s.st.seven_used);
  CFSettings cfg = settingsGet();
  Serial.printf("  cfg quiet=%u-%u ambient=%u%% haptics=%d tz=%d uptime_s=%lu\n",
                cfg.quietStart, cfg.quietEnd, cfg.ambientPct, cfg.haptics ? 1 : 0,
                (int)cfg.tzOffsetMin, (unsigned long)(now / 1000UL));
  claudeFaceStatsPrint(Serial);
}

static void usage() {
  SerialLock lk;
  Serial.println("usage: cf stats | cf set quiet <start 0-23> <end 0-23> | "
                 "cf set ambient <0-100> | cf set haptics <0|1> | "
                 "cf buzz <blocked|done|cross|reset> | cf hello");
}

bool cfLinkConsole(const char *line) {
  if (strncmp(line, "cf", 2) != 0 || (line[2] != ' ' && line[2] != '\0')) return false;
  const char *a = line + 2;
  while (*a == ' ') a++;

  if (strcmp(a, "stats") == 0) { printStats(); return true; }
  if (strcmp(a, "hello") == 0) { sendHello(); return true; }
  if (strncmp(a, "buzz ", 5) == 0) {
    if (claudeFaceRequestTestBuzz(a + 5)) serialLine("cf buzz queued (UI loop)");
    else usage();
    return true;
  }
  if (strncmp(a, "set ", 4) == 0) {
    a += 4;
    int v1 = -1, v2 = -1;
    if (strncmp(a, "quiet ", 6) == 0 && sscanf(a + 6, "%d %d", &v1, &v2) == 2 &&
        v1 >= 0 && v1 <= 23 && v2 >= 0 && v2 <= 23) {
      settingsSetQuiet((uint8_t)v1, (uint8_t)v2);
      serialLine("cf quiet = %d-%d%s", v1, v2, v1 == v2 ? " (disabled)" : "");
      return true;
    }
    if (strncmp(a, "ambient ", 8) == 0 && sscanf(a + 8, "%d", &v1) == 1 &&
        v1 >= 0 && v1 <= 100) {
      settingsSetAmbient((uint8_t)v1);
      serialLine("cf ambient = %d%%", v1);
      return true;
    }
    if (strncmp(a, "haptics ", 8) == 0 && sscanf(a + 8, "%d", &v1) == 1 &&
        (v1 == 0 || v1 == 1)) {
      settingsSetHaptics(v1 == 1);
      serialLine("cf haptics = %d", v1);
      return true;
    }
  }
  usage();
  return true;
}
