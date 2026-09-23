// Claude Face — small pure helpers shared by the link, the face and the
// host tests. Header-only, no Arduino / FreeRTOS / hardware dependencies:
// only <stdint.h>, <stdio.h>, <string.h> and the generated cf_protocol.h.
// C++11-clean (arduino-esp32 2.0.17 compiles with -std=gnu++11).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cf_protocol.h"

// Unix epoch of 2000-01-01 00:00:00 — AuroraOS' rtcEpochSec() counts from
// there; the protocol speaks Unix seconds.
#define CF_UNIX_2000 946684800UL

// Link liveness thresholds (SPEC: "connected" = valid frame within 90 s;
// after 5 min without frames the watch reverts to normal power behaviour).
#define CF_LINK_LIVE_MS    90000UL
#define CF_LINK_HOLD_MS   300000UL

// Rate-limit data older than this renders hollow / dimmed.
#define CF_RL_STALE_S 900

// ---------------------------------------------------------------------------
// Local turn timer. The bridge sends p_turn_elapsed_s at send time; the watch
// keeps ticking it locally from millis() while the primary session is
// WORKING or BLOCKED and the link is live. Frozen otherwise (a dead link must
// not show a timer that keeps running forever).
// ---------------------------------------------------------------------------
static inline uint32_t cfTurnSeconds(uint8_t pState, uint32_t baseS,
                                     uint32_t rxAgeMs, bool linkLive) {
  if (linkLive && (pState == CF_STATE_WORKING || pState == CF_STATE_BLOCKED))
    return baseS + rxAgeMs / 1000UL;
  return baseS;
}

// "mm:ss", or "h:mm:ss" from one hour on.
static inline void cfFmtMMSS(char *out, size_t n, uint32_t s) {
  if (s >= 3600UL)
    snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600UL),
             (unsigned long)((s / 60UL) % 60UL), (unsigned long)(s % 60UL));
  else
    snprintf(out, n, "%02lu:%02lu", (unsigned long)(s / 60UL),
             (unsigned long)(s % 60UL));
}

// Compact duration for countdowns: "42m", "2h14", "3d4h". Seconds under a
// minute round up to "1m" (a countdown that shows 0m while still running
// reads as a bug).
static inline void cfFmtDur(char *out, size_t n, uint32_t s) {
  uint32_t m = (s + 59UL) / 60UL;
  if (m < 60UL)        snprintf(out, n, "%lum", (unsigned long)m);
  else if (m < 1440UL) snprintf(out, n, "%luh%02lu", (unsigned long)(m / 60UL),
                                (unsigned long)(m % 60UL));
  else                 snprintf(out, n, "%lud%luh", (unsigned long)(m / 1440UL),
                                (unsigned long)((m / 60UL) % 24UL));
}

// Civil date from a Unix epoch + offset (Howard Hinnant's days->civil).
// wday: 0 = Sunday (matches struct tm and the RV-3028 convention AuroraOS
// uses via NTP).
struct CFCivil {
  uint16_t year;
  uint8_t  month, day, hour, minute, second, wday;
};
static inline CFCivil cfCivilFromEpoch(int64_t t) {
  CFCivil c;
  int64_t days = t / 86400;
  int64_t rem  = t % 86400;
  if (rem < 0) { rem += 86400; days -= 1; }
  c.hour   = (uint8_t)(rem / 3600);
  c.minute = (uint8_t)((rem / 60) % 60);
  c.second = (uint8_t)(rem % 60);
  int64_t wd = (days + 4) % 7;               // 1970-01-01 was a Thursday
  if (wd < 0) wd += 7;
  c.wday = (uint8_t)wd;
  int64_t z   = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y   = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp  = (5 * doy + 2) / 153;
  int64_t d   = doy - (153 * mp + 2) / 5 + 1;
  int64_t m   = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y += 1;
  c.year  = (uint16_t)y;
  c.month = (uint8_t)m;
  c.day   = (uint8_t)d;
  return c;
}

// Quiet-hours test on whole hours. start == end disables quiet hours;
// start > end wraps midnight (default 23 -> 8).
static inline bool cfInQuietHours(uint8_t hour, uint8_t start, uint8_t end) {
  if (start == end) return false;
  if (start < end) return hour >= start && hour < end;
  return hour >= start || hour < end;
}

// Bounded copy of a protocol char[N] field (may lack a NUL if the sender
// misbehaves) into a NUL-terminated buffer.
static inline void cfCopyStr(char *dst, size_t dstN, const char *src, size_t srcN) {
  size_t n = 0;
  while (n < srcN && src[n]) n++;
  if (n >= dstN) n = dstN - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}
