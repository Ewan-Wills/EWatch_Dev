// Host tests for the Claude Face integration policy (cf_policy.h) plus the
// frame-size assumption behind ewlog's console line buffer.
//
//   ./run.sh      builds with clang++ -std=c++11 and runs; also builds the
//                 deliberately broken variants and requires them to FAIL.
//
// Break switches (each must turn this suite red):
//   -DCF_TEST_BREAK_SNAPSHOT  publish() stops flipping the front buffer
//   -DCF_TEST_BREAK_POLICY    a policy constant is off by one (zone y)
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef CF_TEST_BREAK_POLICY
#include "../../../src/apps/claude/cf_protocol.h"
#define CF_SWIPE_ZONE_Y 176     // pre-empt the header's value (off by one)
#endif
#include "../../../src/apps/claude/cf_policy.h"
#include "../../../src/apps/claude/cf_logic.h"
#include "../../../src/apps/claude/cf_util.h"

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; \
  fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------
// Snapshot double buffer under real concurrency.
// ---------------------------------------------------------------------------
struct StdMux {
  std::mutex m;
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};
struct Payload { uint32_t v[64]; };   // 256 B, like CFLinkSnap

// In-place writer with no lock: what a naive "just memcpy into the shared
// struct" link would do. Used only to prove the tear detector can fire.
struct NaiveBuf {
  Payload p;
  NaiveBuf() { memset(&p, 0, sizeof p); }
};

static void testSnapshot() {
  StdMux mux;
  CFDoubleBuffer<Payload, StdMux> buf(mux);
  const uint32_t N = 200000;
  std::atomic<bool> done(false);
  uint32_t torn = 0, distinct = 0, backwards = 0;

  std::thread reader([&] {
    uint32_t lastV = 0, lastGen = 0;
    while (!done.load()) {
      Payload p;
      uint32_t g = buf.read(p);
      bool ok = true;
      for (int i = 1; i < 64; i++) if (p.v[i] != p.v[0]) { ok = false; break; }
      if (!ok) torn++;
      if (p.v[0] != lastV) { distinct++; if (p.v[0] < lastV) backwards++; lastV = p.v[0]; }
      if (g < lastGen) backwards++;
      lastGen = g;
    }
  });
  for (uint32_t n = 1; n <= N; n++) {
    Payload &b = buf.beginWrite();
    for (int i = 0; i < 64; i++) b.v[i] = n;
    buf.publish();
  }
  done = true;
  reader.join();
  Payload last;
  uint32_t g = buf.read(last);
  CHECK(torn == 0);
  CHECK(backwards == 0);
  CHECK(distinct > 10);            // reader actually saw the writer progress
  CHECK(g == N);
  CHECK(last.v[0] == N && last.v[63] == N);
  printf("  snapshot: torn=%u distinct=%u gen=%u\n", torn, distinct, g);

  // Detector self-test: the naive shared struct must show tearing, or the
  // torn==0 result above proves nothing.
  NaiveBuf nb;
  std::atomic<bool> d2(false);
  std::atomic<uint32_t> naiveTorn(0);
  std::thread r2([&] {
    while (!d2.load()) {
      Payload p;
      memcpy(&p, (const void *)&nb.p, sizeof p);
      for (int i = 1; i < 64; i++) if (p.v[i] != p.v[0]) { naiveTorn++; break; }
    }
  });
  for (uint32_t n = 1; n <= N * 5 && naiveTorn.load() == 0; n++) {
    volatile uint32_t *v = nb.p.v;
    for (int i = 0; i < 64; i++) v[i] = n;
  }
  d2 = true;
  r2.join();
  CHECK(naiveTorn.load() > 0);
  printf("  snapshot detector self-test: naive torn=%u (must be > 0)\n",
         naiveTorn.load());
}

// ---------------------------------------------------------------------------
// Gesture routing
// ---------------------------------------------------------------------------
static void testGestures() {
  // ButtonShort classification: converted swipe vs the side button.
  CHECK(cfButtonShortIsSwipe(true, false, 1000, 500));          // swipe
  CHECK(!cfButtonShortIsSwipe(true, true, 1000, 500));          // button pressed
  CHECK(!cfButtonShortIsSwipe(false, false, 1000, 500));        // no touch seen
  CHECK(!cfButtonShortIsSwipe(true, false, 5000, 500));         // stale touch
  CHECK(cfButtonShortIsSwipe(true, false, 5, 0xFFFFFF00UL));    // millis wrap

  // Zone boundary.
  CHECK(!cfInSessionZone(174));
  CHECK(cfInSessionZone(175));
  CHECK(cfInSessionZone(279));

  // Start-y: recent touch-down wins over the gesture's own coordinate.
  CHECK(cfGestureStartY(true, 1000, 900, 200, 40) == 200);
  CHECK(cfGestureStartY(true, 9000, 900, 200, 40) == 40);
  CHECK(cfGestureStartY(false, 1000, 900, 200, 40) == 40);

  // Tap vs swipe vs slow press.
  CHECK(cfIsTap(0, 0, 100));
  CHECK(cfIsTap(14, 0, 100));
  CHECK(!cfIsTap(15, 0, 100));
  CHECK(!cfIsTap(10, 11, 100));
  CHECK(!cfIsTap(0, 0, 600));

  // Session cycling.
  CHECK(cfNextSessionIndex(0, 3, +1) == 1);
  CHECK(cfNextSessionIndex(2, 3, +1) == 0);
  CHECK(cfNextSessionIndex(0, 3, -1) == 2);
  CHECK(cfNextSessionIndex(255, 3, +1) == 1);   // unknown primary -> from 0
  CHECK(cfNextSessionIndex(0, 1, +1) == 255);   // skip when count < 2
  CHECK(cfNextSessionIndex(0, 0, -1) == 255);
  CHECK(cfNextSessionIndex(3, 9, +1) == 0);     // count clamps to 4 rows

  // Ackable.
  CFStateMsg st;
  memset(&st, 0, sizeof st);
  CHECK(!cfAckable(st));
  st.p_state = CF_STATE_BLOCKED;               CHECK(cfAckable(st));
  st.p_state = CF_STATE_WORKING; st.blocked_count = 1; CHECK(cfAckable(st));
  st.blocked_count = 0; st.p_state = CF_STATE_DONE;    CHECK(!cfAckable(st));
  st.p_flags = CF_PFLAGS_UNACKED_DONE;                 CHECK(cfAckable(st));
}

// ---------------------------------------------------------------------------
// Power policy
// ---------------------------------------------------------------------------
static void testPower() {
  // Hold: face + ever-valid + last frame within 5 min.
  CHECK(cfShouldHold(true, true, 100000, 0, CF_LINK_HOLD_MS));
  CHECK(cfShouldHold(true, true, 299999, 0, CF_LINK_HOLD_MS));
  CHECK(!cfShouldHold(true, true, 300000, 0, CF_LINK_HOLD_MS));
  CHECK(!cfShouldHold(false, true, 1000, 0, CF_LINK_HOLD_MS));
  CHECK(!cfShouldHold(true, false, 1000, 0, CF_LINK_HOLD_MS));
  CHECK(cfShouldHold(true, true, 1000, 0xFFFFF000UL, CF_LINK_HOLD_MS));   // wrap

  // Backlight.
  CHECK(cfBacklightDuty(50000, 0, true, true, 15) == 255);        // BLOCKED
  CHECK(cfBacklightDuty(5000, 0, true, false, 15) == 255);        // < 10 s
  CHECK(cfBacklightDuty(9999, 0, true, false, 15) == 255);
  CHECK(cfBacklightDuty(10000, 0, true, false, 15) == 38);        // ambient 15 %
  CHECK(cfBacklightDuty(10000, 0, false, false, 15) == 38);       // no trigger yet
  CHECK(cfBacklightDuty(0, 0, false, false, 15) == 38);
  CHECK(cfBacklightDuty(99999, 0, true, false, 0) == 0);
  CHECK(cfBacklightDuty(99999, 0, true, false, 1) == 4);          // floor
  CHECK(cfBacklightDuty(99999, 0, true, false, 100) == 255);
  CHECK(cfBacklightDuty(99999, 0, true, false, 250) == 255);      // clamp
  CHECK(cfBacklightDuty(100, 0xFFFFFF00UL, true, false, 15) == 255);   // wrap

  // Wake-worthy edges.
  CFStateMsg a, b;
  memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
  a.p_state = CF_STATE_WORKING; b.p_state = CF_STATE_BLOCKED;
  CHECK(cfWakeWorthy(true, a, b));
  CHECK(!cfWakeWorthy(true, b, b));                      // still blocked
  CHECK(cfWakeWorthy(false, a, b));                      // first frame blocked
  b.p_state = CF_STATE_WORKING; b.blocked_count = 1;     // another session
  CHECK(cfWakeWorthy(true, a, b));
  b.blocked_count = 0; b.p_state = CF_STATE_DONE; b.p_last_turn_s = 61;
  CHECK(cfWakeWorthy(true, a, b));
  b.p_last_turn_s = 30;
  CHECK(!cfWakeWorthy(true, a, b));                      // short turn
  b.p_last_turn_s = 90; a.p_state = CF_STATE_DONE;
  CHECK(!cfWakeWorthy(true, a, b));                      // already DONE

  // State identity changes exactly on visible changes.
  memset(&a, 0, sizeof a); b = a;
  CHECK(cfStateIdentity(a) == cfStateIdentity(b));
  b.seq = 99; b.five_used = 40;                           // heartbeat-ish
  CHECK(cfStateIdentity(a) == cfStateIdentity(b));
  b.global_state = CF_STATE_WORKING;
  CHECK(cfStateIdentity(a) != cfStateIdentity(b));
  b = a; b.primary_index = 2;
  CHECK(cfStateIdentity(a) != cfStateIdentity(b));
}

// ---------------------------------------------------------------------------
// TIME convention (cf_link.cpp applyTime): RTC = UTC epoch + offset.
// ---------------------------------------------------------------------------
static void testTimeConvention() {
  // 2026-09-23 12:00:00 UTC = 1790164800; BST (+60) -> 13:00 local, Wed.
  int64_t local = (int64_t)1790164800LL + 60 * 60;
  CFCivil c = cfCivilFromEpoch(local);
  CHECK(c.year == 2026 && c.month == 9 && c.day == 23);
  CHECK(c.hour == 13 && c.minute == 0 && c.second == 0);
  CHECK(c.wday == 3);
  // Crossing midnight backwards with a negative offset.
  c = cfCivilFromEpoch((int64_t)1790121600LL - 300 * 60);   // 00:00 UTC, -05:00
  CHECK(c.day == 22 && c.hour == 19 && c.wday == 2);
}

// ---------------------------------------------------------------------------
// Console line buffer (ewlog.cpp, 320 B) fits the biggest host->watch frame.
// ---------------------------------------------------------------------------
static void testLineFits() {
  CFStateMsg st;
  memset(&st, 0xAB, sizeof st);
  st.version = CF_PROTO_VERSION;
  char line[512];
  size_t n = cfEncodeFrame(CF_TYPE_STATE, &st, sizeof st, line, sizeof line);
  CHECK(n > 0);
  CHECK(n + 1 <= 320);             // + NUL; ewlog keeps 319 chars max
  // The console strips the '\n' before handing the line over.
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
  CFFrame f;
  CHECK(cfParseLine(line, f) == CF_PARSE_OK);
  CHECK(f.type == CF_TYPE_STATE && f.len == sizeof st);
  // A 96-byte buffer (the old size) would truncate it: parse must fail.
  char trunc[96];
  memcpy(trunc, line, 95); trunc[95] = '\0';
  CHECK(cfParseLine(trunc, f) != CF_PARSE_OK);
  printf("  STATE line = %u chars\n", (unsigned)n);
}

int main() {
  testSnapshot();
  testGestures();
  testPower();
  testTimeConvention();
  testLineFits();
  printf("%s: %d/%d checks passed\n", gFails ? "FAILED" : "OK",
         gChecks - gFails, gChecks);
  return gFails ? 1 : 0;
}
