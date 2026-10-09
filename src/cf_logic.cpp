// Claude Face — pure protocol + behaviour logic. See cf_logic.h.
// No Arduino / FreeRTOS includes: this file is also compiled on the host by
// test/claude/run.sh.
#include <string.h>
#include "cf_logic.h"
#include "cf_util.h"

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int cfB64Decode(const char *in, size_t n, uint8_t *out, size_t outCap) {
  if (n == 0 || (n % 4) != 0) return -1;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; k++) {
      char c = in[i + k];
      if (c == '=') {
        // padding only in the last quad, only in the last two positions,
        // and nothing but padding after it
        if (i + 4 != n || k < 2) return -1;
        pad++;
        v[k] = 0;
      } else {
        if (pad) return -1;
        v[k] = b64val(c);
        if (v[k] < 0) return -1;
      }
    }
    uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                 ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    int bytes = 3 - pad;
    if (o + (size_t)bytes > outCap) return -1;
    out[o++] = (uint8_t)(w >> 16);
    if (bytes > 1) out[o++] = (uint8_t)(w >> 8);
    if (bytes > 2) out[o++] = (uint8_t)w;
  }
  return (int)o;
}

size_t cfB64Encode(const uint8_t *in, size_t n, char *out, size_t outCap) {
  size_t need = ((n + 2) / 3) * 4;
  if (outCap < need + 1) { if (outCap) out[0] = '\0'; return 0; }
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t w = (uint32_t)in[i] << 16;
    if (i + 1 < n) w |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) w |= (uint32_t)in[i + 2];
    out[o++] = kB64[(w >> 18) & 63];
    out[o++] = kB64[(w >> 12) & 63];
    out[o++] = (i + 1 < n) ? kB64[(w >> 6) & 63] : '=';
    out[o++] = (i + 2 < n) ? kB64[w & 63] : '=';
  }
  out[o] = '\0';
  return o;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static size_t expectedLen(char type) {
  switch (type) {
    case CF_TYPE_STATE:    return sizeof(CFStateMsg);
    case CF_TYPE_SESSIONS: return sizeof(CFSessionsMsg);
    case CF_TYPE_TIME:     return sizeof(CFTimeMsg);
    default:               return 0;
  }
}

CFParseResult cfParseLine(const char *line, CFFrame &out) {
  if (!line) return CF_PARSE_NOT_CF;
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' ||
               line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
  if (n < 3 || line[0] != '#' || line[1] != 'C' || line[2] != 'F')
    return CF_PARSE_NOT_CF;
  if (n == 4 && line[3] == 'Q') return CF_PARSE_QUERY;
  if (n < 6 || line[4] != ' ') return CF_PARSE_BAD;
  char type = line[3];
  size_t want = expectedLen(type);
  if (!want) return CF_PARSE_BAD;              // unknown / watch->host type

  uint8_t buf[CF_MAX_PAYLOAD + 2];
  int got = cfB64Decode(line + 5, n - 5, buf, sizeof buf);
  if (got < 3) return CF_PARSE_BAD;            // need >= 1 payload byte + CRC
  size_t plen = (size_t)got - 2;
  uint16_t crc = (uint16_t)(buf[plen] | ((uint16_t)buf[plen + 1] << 8));
  if (cfCrc16(buf, plen) != crc) return CF_PARSE_BAD;

  out.type = type;
  out.version = buf[0];
  out.len = (uint16_t)plen;
  memcpy(out.payload, buf, plen);
  // Version before size: a future protocol may legitimately change sizes.
  if (buf[0] != CF_PROTO_VERSION) return CF_PARSE_VERSION;
  if (plen != want) return CF_PARSE_BAD;
  return CF_PARSE_OK;
}

size_t cfEncodeFrame(char type, const void *payload, size_t len,
                     char *out, size_t outCap) {
  if (len > CF_MAX_PAYLOAD) return 0;
  uint8_t buf[CF_MAX_PAYLOAD + 2];
  memcpy(buf, payload, len);
  uint16_t crc = cfCrc16(buf, len);
  buf[len] = (uint8_t)(crc & 0xFF);
  buf[len + 1] = (uint8_t)(crc >> 8);
  if (outCap < 6) return 0;
  out[0] = '#'; out[1] = 'C'; out[2] = 'F'; out[3] = type; out[4] = ' ';
  size_t b = cfB64Encode(buf, len + 2, out + 5, outCap - 5);
  if (!b || 5 + b + 2 > outCap) { out[0] = '\0'; return 0; }
  out[5 + b] = '\n';
  out[6 + b] = '\0';
  return 6 + b;
}

// ---------------------------------------------------------------------------
// Haptic trigger state machine
// ---------------------------------------------------------------------------
static bool timeReached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static bool sameWindow(uint32_t a, uint32_t b) {
  uint32_t d = a > b ? a - b : b - a;
  return d <= CF_WINDOW_SLOP_S;
}

void cfHapticInit(CFHapticState &s) {
  memset(&s, 0, sizeof s);
}

void cfHapticAck(CFHapticState &s) {
  if (s.blkActive) s.blkAcked = true;
}

void cfHapticNoteSelect(CFHapticState &s, uint32_t nowMs) {
  s.selectGuardUntilMs = nowMs + 3000UL;
  if (!s.selectGuardUntilMs) s.selectGuardUntilMs = 1;
}

static CFBuzz pick(CFBuzz cur, CFBuzz cand) {
  // BLOCKED > CROSS > RESET > DONE
  static const uint8_t rank[5] = { 0, 4, 1, 3, 2 };
  return rank[cand] > rank[cur] ? cand : cur;
}

CFBuzz cfHapticStep(CFHapticState &s, const CFStateMsg *st, uint32_t nowMs,
                    bool linkLive, uint8_t localHour, const CFHapticCfg &cfg) {
  CFBuzz want = CF_BUZZ_NONE;

  if (st) {
    const bool first = !s.havePrev;
    const bool noRl = (st->flags & CF_FLAGS_NO_RATE_LIMITS) != 0;

    // ---- BLOCKED: a new blocked session (count went up) starts an alert.
    uint8_t prevBlk = first ? 0 : s.prevBlockedCount;
    if (st->blocked_count > prevBlk) {
      s.blkActive = true;
      s.blkAcked = false;
      s.blkFires = 1;
      s.blkLastMs = nowMs;
      want = pick(want, CF_BUZZ_BLOCKED);
    } else if (st->blocked_count == 0) {
      s.blkActive = false;
      s.blkAcked = false;
      s.blkFires = 0;
    }

    // ---- DONE after a turn > 60 s (transition into DONE only).
    bool guard = s.selectGuardUntilMs && !timeReached(nowMs, s.selectGuardUntilMs);
    if (!guard) s.selectGuardUntilMs = 0;
    if (!first && s.prevPState != CF_STATE_DONE && st->p_state == CF_STATE_DONE &&
        st->p_last_turn_s > CF_DONE_MIN_TURN_S && !guard)
      want = pick(want, CF_BUZZ_DONE);

    // ---- 5h crossings of 80 % / 95 %, once per window (keyed on resets_at).
    if (!noRl && st->five_used != CF_PCT_UNKNOWN && st->five_resets_at) {
      bool newWindow = !s.winKey || !sameWindow(s.winKey, st->five_resets_at);
      if (newWindow) {
        s.winKey = st->five_resets_at;
        // Already above a threshold when the window is first seen (boot,
        // reconnect): that crossing happened while we weren't watching.
        // A window change seen live starts clean.
        s.fired80 = first && st->five_used >= 80;
        s.fired95 = first && st->five_used >= 95;
      } else {
        s.winKey = st->five_resets_at;         // follow small jitter
      }
      if (st->five_used >= 95 && !s.fired95) {
        s.fired95 = true; s.fired80 = true;
        want = pick(want, CF_BUZZ_CROSS);
      } else if (st->five_used >= 80 && !s.fired80) {
        s.fired80 = true;
        want = pick(want, CF_BUZZ_CROSS);
      }
    }

    // ---- Leaving LIMITED because the window reset.
    bool limitedNow = st->global_state == CF_STATE_LIMITED;
    if (limitedNow) {
      if (!s.wasLimited || !s.limFiveResets) s.limFiveResets = st->five_resets_at;
      if (!s.wasLimited || !s.limSevenResets) s.limSevenResets = st->seven_resets_at;
    } else if (s.wasLimited && !first) {
      bool reset =
          (s.limFiveResets && st->bridge_now && st->bridge_now >= s.limFiveResets) ||
          (s.limSevenResets && st->bridge_now && st->bridge_now >= s.limSevenResets) ||
          (s.limFiveResets && st->five_resets_at &&
           !sameWindow(s.limFiveResets, st->five_resets_at)) ||
          (s.limSevenResets && st->seven_resets_at &&
           !sameWindow(s.limSevenResets, st->seven_resets_at));
      if (reset) want = pick(want, CF_BUZZ_RESET);
    }
    if (!limitedNow) { s.limFiveResets = 0; s.limSevenResets = 0; }
    s.wasLimited = limitedNow;

    s.havePrev = true;
    s.prevPState = st->p_state;
    s.prevBlockedCount = st->blocked_count;
    cfCopyStr(s.prevProject, sizeof s.prevProject, st->p_project, sizeof st->p_project);
  }

  // ---- BLOCKED repeats: every 60 s, max 5 fires total, until acked/resolved.
  if (want != CF_BUZZ_BLOCKED && s.blkActive && !s.blkAcked && linkLive &&
      s.blkFires < CF_BLOCKED_MAX_FIRES &&
      timeReached(nowMs, s.blkLastMs + CF_BLOCKED_REPEAT_MS)) {
    s.blkFires++;
    s.blkLastMs = nowMs;
    want = pick(want, CF_BUZZ_BLOCKED);
  }

  if (want == CF_BUZZ_NONE) return want;
  if (!cfg.enabled || cfInQuietHours(localHour, cfg.quietStart, cfg.quietEnd)) {
    s.suppressed++;
    return CF_BUZZ_NONE;
  }
  s.played[want]++;
  return want;
}
