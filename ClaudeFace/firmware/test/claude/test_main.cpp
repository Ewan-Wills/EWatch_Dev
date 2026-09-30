// Host unit tests for the Claude Face pure logic (src/cf_logic.cpp
// + cf_util.h). Built and run by run.sh with the host clang in C++14 mode.
//
//   test_main <vectors.txt> <checkout_out.txt>
//
// Exit code = number of failed checks (0 = pass). Prints the number of checks
// executed so a harness that silently ran nothing can be spotted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "cf_logic.h"
#include "cf_util.h"

static int gChecks = 0, gFails = 0;
#define CHECK(cond, ...) do { gChecks++; if (!(cond)) { gFails++; \
  printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------------------
static void testBase64() {
  struct { const char *plain, *b64; } kv[] = {
    { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
    { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
  };
  for (auto &k : kv) {
    char e[32]; size_t n = cfB64Encode((const uint8_t *)k.plain, strlen(k.plain), e, sizeof e);
    CHECK(n == strlen(k.b64) && strcmp(e, k.b64) == 0, "encode %s -> %s", k.plain, e);
    uint8_t d[32]; int m = cfB64Decode(k.b64, strlen(k.b64), d, sizeof d);
    CHECK(m == (int)strlen(k.plain) && memcmp(d, k.plain, m) == 0, "decode %s", k.b64);
  }
  const char *bad[] = { "Zg=a", "Z===", "Zm9", "Zm9v!AAA", "=AAA", "Zg==Zg==", "" };
  for (const char *b : bad) {
    uint8_t d[32];
    CHECK(cfB64Decode(b, strlen(b), d, sizeof d) < 0, "decode should reject '%s'", b);
  }
  // capacity limits
  uint8_t small[2];
  CHECK(cfB64Decode("Zm9v", 4, small, sizeof small) < 0, "decode overflow rejected");
  char tiny[4];
  CHECK(cfB64Encode((const uint8_t *)"foo", 3, tiny, sizeof tiny) == 0, "encode overflow rejected");
  // round trip every length 0..130 with pseudo-random bytes
  uint32_t seed = 12345;
  for (size_t len = 1; len <= 130; len++) {
    uint8_t in[130], out[140]; char enc[200];
    for (size_t i = 0; i < len; i++) { seed = seed * 1103515245u + 12345u; in[i] = (uint8_t)(seed >> 16); }
    size_t e = cfB64Encode(in, len, enc, sizeof enc);
    int d = cfB64Decode(enc, e, out, sizeof out);
    CHECK(d == (int)len && memcmp(in, out, len) == 0, "roundtrip len %zu", len);
  }
}

static void testCrc() {
  CHECK(cfCrc16((const uint8_t *)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");
  CHECK(cfCrc16((const uint8_t *)"", 0) == 0xFFFF, "CRC of empty = init");
}

// ---------------------------------------------------------------------------
static std::string unescape(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'r') { o += '\r'; i++; }
    else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
    else o += s[i];
  }
  return o;
}
static std::string fieldStr(const char *p, size_t n) {
  size_t k = 0; while (k < n && p[k]) k++;
  return std::string(p, k);
}
static bool checkField(const CFFrame &f, const std::string &k, const std::string &v) {
  long long iv = atoll(v.c_str());
  if (f.type == CF_TYPE_STATE) {
    CFStateMsg s; memcpy(&s, f.payload, sizeof s);
    if (k == "seq") return s.seq == iv;
    if (k == "five_resets_at") return s.five_resets_at == (uint32_t)iv;
    if (k == "five_pace") return s.five_pace == iv;
    if (k == "p_turn_elapsed_s") return s.p_turn_elapsed_s == (uint32_t)iv;
    if (k == "bridge_now") return s.bridge_now == (uint32_t)iv;
    if (k == "p_project") return fieldStr(s.p_project, sizeof s.p_project) == v;
    if (k == "p_current_tool") return fieldStr(s.p_current_tool, sizeof s.p_current_tool) == v;
  } else if (f.type == CF_TYPE_SESSIONS) {
    CFSessionsMsg s; memcpy(&s, f.payload, sizeof s);
    if (k == "seq") return s.seq == iv;
    if (k == "count") return s.count == iv;
    if (k == "row1_state") return s.rows[1].state == iv;
    if (k == "row0_project") return fieldStr(s.rows[0].project, sizeof s.rows[0].project) == v;
  } else if (f.type == CF_TYPE_TIME) {
    CFTimeMsg t; memcpy(&t, f.payload, sizeof t);
    if (k == "epoch") return t.epoch == (uint32_t)iv;
    if (k == "utc_offset_min") return t.utc_offset_min == iv;
  }
  printf("  unknown check field %s\n", k.c_str());
  return false;
}

static void testFrames(const char *vecPath) {
  FILE *fp = fopen(vecPath, "r");
  CHECK(fp != nullptr, "open %s", vecPath);
  if (!fp) return;
  char buf[1024];
  int n = 0;
  while (fgets(buf, sizeof buf, fp)) {
    std::string l(buf);
    while (!l.empty() && l.back() == '\n') l.pop_back();
    if (l.empty()) continue;
    size_t t1 = l.find('\t'), t2 = l.find('\t', t1 + 1), t3 = l.find('\t', t2 + 1);
    std::string exp = l.substr(0, t1), type = l.substr(t1 + 1, t2 - t1 - 1);
    std::string checks = l.substr(t2 + 1, t3 - t2 - 1), line = unescape(l.substr(t3 + 1));
    CFFrame f; memset(&f, 0, sizeof f);
    CFParseResult r = cfParseLine(line.c_str(), f);
    const char *names[] = { "NOTCF", "QUERY", "OK", "BAD", "VERSION" };
    CHECK(exp == names[r], "vector %d '%.40s': expected %s got %s", n, line.c_str(), exp.c_str(), names[r]);
    if (r == CF_PARSE_OK || r == CF_PARSE_VERSION)
      CHECK(type == std::string(1, f.type), "vector %d type %c", n, f.type);
    if (r == CF_PARSE_OK && !checks.empty()) {
      size_t p = 0;
      while (p < checks.size()) {
        size_t c = checks.find(',', p); if (c == std::string::npos) c = checks.size();
        std::string kv = checks.substr(p, c - p);
        size_t eq = kv.find('=');
        CHECK(checkField(f, kv.substr(0, eq), kv.substr(eq + 1)), "vector %d field %s", n, kv.c_str());
        p = c + 1;
      }
    }
    n++;
  }
  fclose(fp);
  CHECK(n >= 25, "expected >= 25 vectors, read %d", n);
}

// Encode watch->host frames; run.sh has Python decode + verify them.
static void writeCheckout(const char *path) {
  FILE *fp = fopen(path, "w");
  CHECK(fp != nullptr, "open %s", path);
  if (!fp) return;
  char line[128];
  CFEventMsg e; memset(&e, 0, sizeof e);
  e.version = CF_PROTO_VERSION; e.code = CF_EVENTCODE_SELECT_SESSION; e.arg = 2; e.seq = 513;
  size_t n = cfEncodeFrame(CF_TYPE_EVENT, &e, sizeof e, line, sizeof line);
  CHECK(n > 0 && line[n - 1] == '\n' && strncmp(line, "#CFE ", 5) == 0, "event frame shape");
  line[n - 1] = '\0';
  fprintf(fp, "arg=2,code=2,seq=513,version=%d\t%s\n", CF_PROTO_VERSION, line);
  e.code = CF_EVENTCODE_ACK; e.arg = 0; e.seq = 65535;
  n = cfEncodeFrame(CF_TYPE_EVENT, &e, sizeof e, line, sizeof line);
  line[n - 1] = '\0';
  fprintf(fp, "arg=0,code=1,seq=65535,version=%d\t%s\n", CF_PROTO_VERSION, line);
  CFHelloMsg h; memset(&h, 0, sizeof h);
  h.version = CF_PROTO_VERSION; strncpy(h.fw, "ClaudeFace fw1", sizeof h.fw - 1);
  n = cfEncodeFrame(CF_TYPE_HELLO, &h, sizeof h, line, sizeof line);
  CHECK(n > 0, "hello frame");
  line[n - 1] = '\0';
  fprintf(fp, "fw=ClaudeFace fw1,version=%d\t%s\n", CF_PROTO_VERSION, line);
  fclose(fp);
  // too-small buffer must fail cleanly
  char tiny[8];
  CHECK(cfEncodeFrame(CF_TYPE_EVENT, &e, sizeof e, tiny, sizeof tiny) == 0, "encode into tiny buffer fails");
}

// ---------------------------------------------------------------------------
// Haptic state machine
// ---------------------------------------------------------------------------
static const uint32_t T0 = 1790000000UL;
static CFStateMsg st0() {
  CFStateMsg s; memset(&s, 0, sizeof s);
  s.version = CF_PROTO_VERSION;
  s.global_state = CF_STATE_WORKING; s.p_state = CF_STATE_WORKING;
  s.five_used = 40; s.five_resets_at = T0 + 3600; s.seven_used = 20; s.seven_resets_at = T0 + 86400;
  s.bridge_now = T0;
  strcpy(s.p_project, "proj");
  return s;
}
static const CFHapticCfg kOn = { true, 23, 8 };
static CFBuzz step(CFHapticState &h, const CFStateMsg *s, uint32_t ms, uint8_t hour = 12,
                   const CFHapticCfg &cfg = kOn, bool live = true) {
  return cfHapticStep(h, s, ms, live, hour, cfg);
}

static void testHapticBlocked() {
  CFHapticState h; cfHapticInit(h);
  CFStateMsg s = st0();
  CHECK(step(h, &s, 1000) == CF_BUZZ_NONE, "first working frame silent");
  s.global_state = CF_STATE_BLOCKED; s.p_state = CF_STATE_BLOCKED; s.blocked_count = 1;
  CHECK(step(h, &s, 2000) == CF_BUZZ_BLOCKED, "blocked fires");
  CHECK(step(h, &s, 3000) == CF_BUZZ_NONE, "heartbeat of same blocked is silent");
  CHECK(step(h, nullptr, 2000 + 59999) == CF_BUZZ_NONE, "no repeat before 60 s");
  CHECK(step(h, nullptr, 2000 + 60000) == CF_BUZZ_BLOCKED, "repeat at 60 s (2)");
  CHECK(step(h, nullptr, 2000 + 60500) == CF_BUZZ_NONE, "no double repeat");
  CHECK(step(h, nullptr, 2000 + 120000) == CF_BUZZ_BLOCKED, "repeat 3");
  CHECK(step(h, nullptr, 2000 + 180000) == CF_BUZZ_BLOCKED, "repeat 4");
  CHECK(step(h, nullptr, 2000 + 240000) == CF_BUZZ_BLOCKED, "repeat 5");
  CHECK(step(h, nullptr, 2000 + 300000) == CF_BUZZ_NONE, "max 5 fires");
  CHECK(step(h, nullptr, 2000 + 900000) == CF_BUZZ_NONE, "still capped");
  // resolve then a new block
  s.blocked_count = 0; s.global_state = CF_STATE_WORKING; s.p_state = CF_STATE_WORKING;
  CHECK(step(h, &s, 1000000) == CF_BUZZ_NONE, "resolved silent");
  s.blocked_count = 1; s.global_state = CF_STATE_BLOCKED;
  CHECK(step(h, &s, 1001000) == CF_BUZZ_BLOCKED, "new block fires again");
  // ack stops repeats
  cfHapticAck(h);
  CHECK(step(h, nullptr, 1001000 + 60000) == CF_BUZZ_NONE, "acked: no repeat");
  // a second session blocking while the first is still blocked is a new alert
  s.blocked_count = 2;
  CHECK(step(h, &s, 1100000) == CF_BUZZ_BLOCKED, "second blocked session fires");
  CHECK(step(h, nullptr, 1100000 + 60000) == CF_BUZZ_BLOCKED, "and its repeat runs (ack cleared)");
  // repeats pause while the link is down
  CHECK(step(h, nullptr, 1100000 + 120000, 12, kOn, false) == CF_BUZZ_NONE, "no repeat with link down");
  // boot straight into a blocked state: alert
  CFHapticState h2; cfHapticInit(h2);
  CHECK(step(h2, &s, 5) == CF_BUZZ_BLOCKED, "first frame already blocked alerts");
}

static void testHapticDone() {
  CFHapticState h; cfHapticInit(h);
  CFStateMsg s = st0();
  step(h, &s, 1000);
  s.p_state = CF_STATE_DONE; s.global_state = CF_STATE_DONE; s.p_last_turn_s = 61;
  CHECK(step(h, &s, 2000) == CF_BUZZ_DONE, "done after 61 s turn buzzes");
  CHECK(step(h, &s, 3000) == CF_BUZZ_NONE, "done heartbeat silent");
  s.p_state = CF_STATE_WORKING; step(h, &s, 4000);
  s.p_state = CF_STATE_DONE; s.p_last_turn_s = 60;
  CHECK(step(h, &s, 5000) == CF_BUZZ_NONE, "done after exactly 60 s is silent (spec: > 60 s)");
  // first frame after boot already DONE: silent
  CFHapticState h2; cfHapticInit(h2);
  s.p_last_turn_s = 500;
  CHECK(step(h2, &s, 10) == CF_BUZZ_NONE, "boot into DONE silent");
  // watch-initiated session switch landing on a DONE session: silent
  CFHapticState h3; cfHapticInit(h3);
  CFStateMsg w = st0(); step(h3, &w, 100);
  cfHapticNoteSelect(h3, 200);
  CHECK(step(h3, &s, 400) == CF_BUZZ_NONE, "select guard suppresses fake DONE edge");
  w.p_state = CF_STATE_WORKING; step(h3, &w, 5000);
  CHECK(step(h3, &s, 6000) == CF_BUZZ_DONE, "guard expired: real DONE buzzes");
  // WAITING_INPUT is not DONE
  CFHapticState h4; cfHapticInit(h4); step(h4, &w, 1);
  CFStateMsg wi = s; wi.p_state = CF_STATE_WAITING_INPUT;
  CHECK(step(h4, &wi, 2) == CF_BUZZ_NONE, "waiting_input does not buzz");
}

static void testHapticCross() {
  CFHapticState h; cfHapticInit(h);
  CFStateMsg s = st0();
  s.five_used = 70; CHECK(step(h, &s, 1) == CF_BUZZ_NONE, "70 silent");
  s.five_used = 81; CHECK(step(h, &s, 2) == CF_BUZZ_CROSS, "cross 80");
  s.five_used = 85; CHECK(step(h, &s, 3) == CF_BUZZ_NONE, "85 no repeat");
  s.five_used = 79; step(h, &s, 4);
  s.five_used = 82; CHECK(step(h, &s, 5) == CF_BUZZ_NONE, "80 again same window: once per window");
  s.five_resets_at += 45;                       // bridge jitter, same window
  s.five_used = 96; CHECK(step(h, &s, 6) == CF_BUZZ_CROSS, "cross 95");
  s.five_used = 99; CHECK(step(h, &s, 7) == CF_BUZZ_NONE, "95 once");
  s.five_resets_at = T0 + 3600 + 5 * 3600; s.five_used = 3;   // new window
  CHECK(step(h, &s, 8) == CF_BUZZ_NONE, "new window low");
  s.five_used = 80; CHECK(step(h, &s, 9) == CF_BUZZ_CROSS, "cross 80 in new window");
  s.five_used = 100; CHECK(step(h, &s, 10) == CF_BUZZ_CROSS, "jump 80->100 crosses 95 once");
  // boot at 85 %: the crossing wasn't observed
  CFHapticState h2; cfHapticInit(h2);
  CFStateMsg b = st0(); b.five_used = 85;
  CHECK(step(h2, &b, 1) == CF_BUZZ_NONE, "boot at 85 silent");
  b.five_used = 90; CHECK(step(h2, &b, 2) == CF_BUZZ_NONE, "90 silent");
  b.five_used = 95; CHECK(step(h2, &b, 3) == CF_BUZZ_CROSS, "95 fires");
  // unknown / API-key sessions never buzz
  CFHapticState h3; cfHapticInit(h3);
  CFStateMsg u = st0(); u.five_used = CF_PCT_UNKNOWN; step(h3, &u, 1);
  u.five_used = 99; u.flags = CF_FLAGS_NO_RATE_LIMITS;
  CHECK(step(h3, &u, 2) == CF_BUZZ_NONE, "NO_RATE_LIMITS never buzzes");
}

static void testHapticLimited() {
  CFHapticState h; cfHapticInit(h);
  CFStateMsg s = st0();
  step(h, &s, 1);
  s.global_state = CF_STATE_LIMITED; s.five_used = 100; s.five_resets_at = T0 + 600;
  step(h, &s, 2);
  // leaves LIMITED before the reset time (e.g. bridge data correction): silent
  CFStateMsg n = s; n.global_state = CF_STATE_IDLE; n.bridge_now = T0 + 100;
  CHECK(step(h, &n, 3) == CF_BUZZ_NONE, "leave LIMITED without reset: silent");
  step(h, &s, 4);                                // limited again
  n.bridge_now = T0 + 601; n.five_used = 0;      // window reset
  CHECK(step(h, &n, 5) == CF_BUZZ_RESET, "window reset out of LIMITED: short pulse");
  CHECK(step(h, &n, 6) == CF_BUZZ_NONE, "once");
  // reset detected via a new resets_at even if bridge_now lags
  step(h, &s, 7);
  CFStateMsg m = s; m.global_state = CF_STATE_WORKING; m.bridge_now = T0;
  m.five_resets_at = T0 + 600 + 5 * 3600; m.five_used = 1;
  CHECK(step(h, &m, 8) == CF_BUZZ_RESET, "new resets_at out of LIMITED");
}

static void testHapticQuiet() {
  CFHapticState h; cfHapticInit(h);
  CFStateMsg s = st0(); step(h, &s, 1, 23);
  s.blocked_count = 1; s.global_state = CF_STATE_BLOCKED;
  CHECK(step(h, &s, 1000, 23) == CF_BUZZ_NONE, "quiet hours: blocked is visual only");
  CHECK(h.suppressed == 1, "suppressed counted (%u)", h.suppressed);
  CHECK(step(h, nullptr, 61000, 7) == CF_BUZZ_NONE, "07:00 still quiet");
  CHECK(step(h, nullptr, 121000, 8) == CF_BUZZ_BLOCKED, "08:00 quiet over: remaining repeats run");
  CFHapticCfg off = { false, 23, 8 };
  CFHapticState h2; cfHapticInit(h2);
  CFStateMsg b = st0(); b.blocked_count = 1;
  CHECK(step(h2, &b, 10, 12, off) == CF_BUZZ_NONE, "haptics disabled");
  CHECK(h2.played[CF_BUZZ_BLOCKED] == 0, "nothing played when disabled");
}

// ---------------------------------------------------------------------------
static void testUtil() {
  CHECK(cfTurnSeconds(CF_STATE_WORKING, 100, 5500, true) == 105, "working ticks");
  CHECK(cfTurnSeconds(CF_STATE_BLOCKED, 100, 61999, true) == 161, "blocked ticks");
  CHECK(cfTurnSeconds(CF_STATE_WORKING, 100, 5500, false) == 100, "link down freezes");
  CHECK(cfTurnSeconds(CF_STATE_DONE, 100, 5500, true) == 100, "done frozen");
  CHECK(cfTurnSeconds(CF_STATE_COMPACTING, 7, 3000, true) == 7, "compacting frozen");
  CHECK(cfTurnSeconds(CF_STATE_WORKING, 0xFFFFFF00UL, 0, true) == 0xFFFFFF00UL, "big base");
  char b[16];
  cfFmtMMSS(b, sizeof b, 0);    CHECK(!strcmp(b, "00:00"), "mmss 0 -> %s", b);
  cfFmtMMSS(b, sizeof b, 187);  CHECK(!strcmp(b, "03:07"), "mmss 187 -> %s", b);
  cfFmtMMSS(b, sizeof b, 3599); CHECK(!strcmp(b, "59:59"), "mmss 3599 -> %s", b);
  cfFmtMMSS(b, sizeof b, 3600); CHECK(!strcmp(b, "1:00:00"), "mmss 3600 -> %s", b);
  cfFmtMMSS(b, sizeof b, 12307);CHECK(!strcmp(b, "3:25:07"), "mmss 12307 -> %s", b);
  cfFmtDur(b, sizeof b, 0);     CHECK(!strcmp(b, "0m"), "dur 0 -> %s", b);
  cfFmtDur(b, sizeof b, 1);     CHECK(!strcmp(b, "1m"), "dur 1 -> %s", b);
  cfFmtDur(b, sizeof b, 180);   CHECK(!strcmp(b, "3m"), "dur 180 -> %s", b);
  cfFmtDur(b, sizeof b, 8040);  CHECK(!strcmp(b, "2h14"), "dur 8040 -> %s", b);
  cfFmtDur(b, sizeof b, 3600);  CHECK(!strcmp(b, "1h00"), "dur 3600 -> %s", b);
  cfFmtDur(b, sizeof b, 273600);CHECK(!strcmp(b, "3d4h"), "dur 273600 -> %s", b);
  CFCivil c = cfCivilFromEpoch(0);
  CHECK(c.year == 1970 && c.month == 1 && c.day == 1 && c.wday == 4, "epoch 0");
  c = cfCivilFromEpoch(1790000000LL);
  CHECK(c.year == 2026 && c.month == 9 && c.day == 21 && c.hour == 14 && c.minute == 13 &&
        c.second == 20 && c.wday == 1, "2026-09-21T14:13:20 Mon");
  c = cfCivilFromEpoch(1790000000LL - 300 * 60);
  CHECK(c.hour == 9 && c.minute == 13, "offset -300 -> 09:13");
  c = cfCivilFromEpoch(951782400LL);
  CHECK(c.year == 2000 && c.month == 2 && c.day == 29 && c.wday == 2, "leap day 2000");
  c = cfCivilFromEpoch(4102444799LL);
  CHECK(c.year == 2099 && c.month == 12 && c.day == 31 && c.hour == 23 && c.wday == 4, "2099-12-31");
  CHECK(cfInQuietHours(23, 23, 8) && cfInQuietHours(0, 23, 8) && cfInQuietHours(7, 23, 8), "quiet wrap in");
  CHECK(!cfInQuietHours(8, 23, 8) && !cfInQuietHours(22, 23, 8) && !cfInQuietHours(12, 23, 8), "quiet wrap out");
  CHECK(!cfInQuietHours(3, 8, 8), "start==end disables");
  CHECK(cfInQuietHours(3, 1, 5) && !cfInQuietHours(5, 1, 5) && !cfInQuietHours(0, 1, 5), "non-wrapping");
  char d[8]; const char src[4] = { 'a', 'b', 'c', 'd' };   // no NUL
  cfCopyStr(d, sizeof d, src, sizeof src); CHECK(!strcmp(d, "abcd"), "copy unterminated");
  cfCopyStr(d, 3, src, sizeof src);        CHECK(!strcmp(d, "ab"), "copy truncates");
}

int main(int argc, char **argv) {
  if (argc < 3) { printf("usage: test_main <vectors> <checkout_out>\n"); return 99; }
  testBase64();
  testCrc();
  testFrames(argv[1]);
  writeCheckout(argv[2]);
  testHapticBlocked();
  testHapticDone();
  testHapticCross();
  testHapticLimited();
  testHapticQuiet();
  testUtil();
  printf("host tests: %d checks, %d failed\n", gChecks, gFails);
  return gFails > 99 ? 99 : gFails;
}
