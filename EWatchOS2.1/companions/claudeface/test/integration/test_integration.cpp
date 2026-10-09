// Host tests for the Claude Face integration policy (cf_policy.h,
// cf_orient.h) plus the frame-size assumption behind the link task's line
// buffer (serial_io.cpp).
//
//   ./run.sh      builds with clang++ -std=c++11 and runs; also builds the
//                 deliberately broken variants and requires them to FAIL.
//
// Break switches (each must turn this suite red):
//   -DCF_TEST_BREAK_SNAPSHOT  publish() stops flipping the front buffer
//   -DCF_TEST_BREAK_POLICY    swipe left/right cycle sessions the wrong way
//   -DCF_TEST_BREAK_ORIENT    touch mapping mirrored (vx = py)
//   -DCF_TEST_BREAK_DIR       swipe directions rotated the wrong way
//   -DCF_TEST_BREAK_BUTTON    the power-on button hold is not ignored
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <thread>

#include "cf_policy.h"
#include "cf_logic.h"
#include "cf_util.h"

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
// ---------------------------------------------------------------------------
// Landscape orientation (cf_orient.h): the watch lies with its physical top
// edge (panel row py = 0) pointing to the viewer's RIGHT.
// ---------------------------------------------------------------------------
static void testOrientation() {
  int16_t vx, vy, px, py;
  // Corners of the panel -> where the viewer sees them.
  cfPanelToView(0, 0, vx, vy);     CHECK(vx == 279 && vy == 0);     // top-left -> top-right
  cfPanelToView(239, 0, vx, vy);   CHECK(vx == 279 && vy == 239);   // top-right -> bottom-right
  cfPanelToView(0, 279, vx, vy);   CHECK(vx == 0 && vy == 0);       // bottom-left -> top-left
  cfPanelToView(239, 279, vx, vy); CHECK(vx == 0 && vy == 239);
  // The whole panel row py = 0 (physical top edge) is the viewer's right edge.
  int topRight = 0;
  for (int x = 0; x < CF_PANEL_W; x++) { cfPanelToView((int16_t)x, 0, vx, vy); if (vx == CF_VIEW_W - 1) topRight++; }
  CHECK(topRight == CF_PANEL_W);
  // Bijection + inverse over every pixel.
  static uint8_t hit[CF_VIEW_W * CF_VIEW_H];
  memset(hit, 0, sizeof hit);
  int bad = 0;
  for (int y = 0; y < CF_PANEL_H; y++) for (int x = 0; x < CF_PANEL_W; x++) {
    cfPanelToView((int16_t)x, (int16_t)y, vx, vy);
    if (vx < 0 || vy < 0 || vx >= CF_VIEW_W || vy >= CF_VIEW_H) { bad++; continue; }
    hit[vy * CF_VIEW_W + vx]++;
    cfViewToPanel(vx, vy, px, py);
    if (px != x || py != y) bad++;
  }
  for (int i = 0; i < CF_VIEW_W * CF_VIEW_H; i++) if (hit[i] != 1) bad++;
  CHECK(bad == 0);

  // Rect mapping = the set of mapped points (every region-sized rect).
  const CFRect rs[5] = { {0, 0, 136, 210}, {136, 0, 144, 68}, {136, 68, 144, 142},
                         {0, 210, 280, 30}, {26, 44, 85, 85} };
  for (int r = 0; r < 5; r++) {
    CFRect p = cfViewRectToPanel(rs[r]);
    int mis = 0;
    for (int y = 0; y < CF_PANEL_H; y++) for (int x = 0; x < CF_PANEL_W; x++) {
      cfPanelToView((int16_t)x, (int16_t)y, vx, vy);
      bool inV = vx >= rs[r].x && vx < rs[r].x + rs[r].w && vy >= rs[r].y && vy < rs[r].y + rs[r].h;
      bool inP = x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h;
      if (inV != inP) mis++;
    }
    CHECK(mis == 0);
  }

  // Swipe directions, derived from the point mapping rather than trusted:
  // move 60 px in a native direction, map both ends, see where it went.
  const struct { uint8_t d; int16_t dx, dy; } nat[4] = {
    { CF_DIR_UP, 0, -60 }, { CF_DIR_DOWN, 0, 60 }, { CF_DIR_LEFT, -60, 0 }, { CF_DIR_RIGHT, 60, 0 } };
  for (int i = 0; i < 4; i++) {
    int16_t ax, ay, bx, by;
    cfPanelToView(120, 140, ax, ay);
    cfPanelToView((int16_t)(120 + nat[i].dx), (int16_t)(140 + nat[i].dy), bx, by);
    int ddx = bx - ax, ddy = by - ay;
    uint8_t seen = (ddx > 0 && ddy == 0) ? CF_DIR_RIGHT : (ddx < 0 && ddy == 0) ? CF_DIR_LEFT
                 : (ddy > 0 && ddx == 0) ? CF_DIR_DOWN  : (ddy < 0 && ddx == 0) ? CF_DIR_UP : CF_DIR_NONE;
    CHECK(seen == cfPanelDirToView(nat[i].d));
  }
  // ... and the table itself, spelled out.
  CHECK(cfPanelDirToView(CF_DIR_UP) == CF_DIR_RIGHT);
  CHECK(cfPanelDirToView(CF_DIR_DOWN) == CF_DIR_LEFT);
  CHECK(cfPanelDirToView(CF_DIR_LEFT) == CF_DIR_UP);
  CHECK(cfPanelDirToView(CF_DIR_RIGHT) == CF_DIR_DOWN);
  CHECK(cfPanelDirToView(CF_DIR_NONE) == CF_DIR_NONE);
}

// The full landscape gesture table, driven the way claude_face.cpp drives
// it: CST816S gesture id -> native panel direction -> viewer direction ->
// action. The viewer directions are also re-derived from the point mapping
// in testOrientation(), so a wrong chip table or a wrong rotation both fail.
static uint8_t routeChip(uint8_t chipGesture, bool detail) {
  return cfRouteSwipe(cfPanelDirToView(cfChipGestureToPanelDir(chipGesture)), detail);
}
static void testGestureTable() {
  // Chip ids as AuroraOS names them (core/event.h).
  CHECK(cfChipGestureToPanelDir(0x01) == CF_DIR_DOWN);
  CHECK(cfChipGestureToPanelDir(0x02) == CF_DIR_UP);
  CHECK(cfChipGestureToPanelDir(0x03) == CF_DIR_LEFT);
  CHECK(cfChipGestureToPanelDir(0x04) == CF_DIR_RIGHT);
  CHECK(cfChipGestureToPanelDir(0x05) == CF_DIR_NONE);   // tap
  CHECK(cfChipGestureToPanelDir(0x0B) == CF_DIR_NONE);   // double tap
  CHECK(cfChipGestureToPanelDir(0x0C) == CF_DIR_NONE);   // long press
  CHECK(cfChipGestureToPanelDir(0x00) == CF_DIR_NONE);

  // Viewer frame (watch top edge pointing right):
  CHECK(routeChip(CF_G_SWIPE_LEFT,  false) == CF_ACT_OPEN_DETAIL);    // viewer UP
  CHECK(routeChip(CF_G_SWIPE_RIGHT, false) == CF_ACT_NONE);           // viewer DOWN
  CHECK(routeChip(CF_G_SWIPE_DOWN,  false) == CF_ACT_SESSION_NEXT);   // viewer LEFT
  CHECK(routeChip(CF_G_SWIPE_UP,    false) == CF_ACT_SESSION_PREV);   // viewer RIGHT
  // Detail page open: every swipe closes it; non-swipes do nothing.
  const uint8_t sw[4] = { CF_G_SWIPE_DOWN, CF_G_SWIPE_UP, CF_G_SWIPE_LEFT, CF_G_SWIPE_RIGHT };
  for (int i = 0; i < 4; i++) CHECK(routeChip(sw[i], true) == CF_ACT_CLOSE_DETAIL);
  CHECK(routeChip(CF_G_SINGLE_TAP, false) == CF_ACT_NONE);
  CHECK(routeChip(CF_G_LONG_PRESS, true) == CF_ACT_NONE);
  // Viewer-direction table spelled out.
  CHECK(cfRouteSwipe(CF_DIR_LEFT, false) == CF_ACT_SESSION_NEXT);
  CHECK(cfRouteSwipe(CF_DIR_RIGHT, false) == CF_ACT_SESSION_PREV);
  CHECK(cfRouteSwipe(CF_DIR_UP, false) == CF_ACT_OPEN_DETAIL);
  CHECK(cfRouteSwipe(CF_DIR_DOWN, false) == CF_ACT_NONE);
  CHECK(cfRouteSwipe(CF_DIR_NONE, true) == CF_ACT_NONE);
}

// Side button: short press / power-off hold / bounce / held-at-boot.
static void testButton() {
  CFButton b;
  // Short press.
  cfButtonInit(b, false);
  CHECK(cfButtonStep(b, false, 0) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 1000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 1500) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 1500) == CF_BTN_SHORT);
  CHECK(cfButtonStep(b, false, 1600) == CF_BTN_NONE);          // once
  // Bounce (< 30 ms) is nothing.
  CHECK(cfButtonStep(b, true, 2000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 2010) == CF_BTN_NONE);
  // 1..3 s: nothing on release.
  CHECK(cfButtonStep(b, true, 3000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 5999) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 5999) == CF_BTN_NONE);
  // Hold 3 s: power-off exactly once, while still held; release is silent.
  CHECK(cfButtonStep(b, true, 7000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 9999) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 10000) == CF_BTN_POWER_OFF);
  CHECK(cfButtonStep(b, true, 12000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 12500) == CF_BTN_NONE);
  // millis() wrap in the middle of a hold.
  CHECK(cfButtonStep(b, true, 0xFFFFFF00UL) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 3000) == CF_BTN_POWER_OFF);
  CHECK(cfButtonStep(b, false, 3100) == CF_BTN_NONE);
  // Held at boot (the user powering on): ignored until released — no
  // power-off after 3 s, no short press on release; the next press counts.
  cfButtonInit(b, true);
  CHECK(cfButtonStep(b, true, 100) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 4000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 9000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 9100) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, true, 10000) == CF_BTN_NONE);
  CHECK(cfButtonStep(b, false, 10200) == CF_BTN_SHORT);
  cfButtonInit(b, true);
  CHECK(cfButtonStep(b, false, 200) == CF_BTN_NONE);           // quick boot tap
}

// cfPushPanelRect: exact pixels land, nothing else is touched, bands obey
// the scratch cap, clipping works, and the no-scratch fallback is a
// full-width band.
static void testPushPlanner() {
  static uint16_t fb[CF_PANEL_W * CF_PANEL_H], panel[CF_PANEL_W * CF_PANEL_H];
  for (int i = 0; i < CF_PANEL_W * CF_PANEL_H; i++) fb[i] = (uint16_t)(i * 2654435761u >> 16);
  uint16_t scratch[1000];
  const CFRect rects[4] = { {  0, 144, 210, 136 },    // HERO's panel rect
                            { 44, 169,  85,  85 },    // CENTRE's panel rect
                            {210,   0,  30, 280 },    // FOOTER's panel rect
                            {230, 270,  40,  40 } };  // runs off the panel (clip)
  for (int r = 0; r < 4; r++) {
    for (int useScratch = 0; useScratch < 2; useScratch++) {
      memset(panel, 0, sizeof panel);
      int maxBlock = 0, calls = 0;
      cfPushPanelRect(fb, rects[r], useScratch ? scratch : nullptr, sizeof scratch / 2,
        [&](int16_t x, int16_t y, uint16_t *src, int16_t w, int16_t h) {
          calls++;
          if (w * h > maxBlock) maxBlock = w * h;
          for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++)
            panel[(y + yy) * CF_PANEL_W + x + xx] = src[yy * w + xx];
        });
      CFRect c = rects[r];
      int wrong = 0;
      for (int y = 0; y < CF_PANEL_H; y++) for (int x = 0; x < CF_PANEL_W; x++) {
        bool in = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
        bool band = y >= c.y && y < c.y + c.h;
        uint16_t v = panel[y * CF_PANEL_W + x];
        if (in && v != fb[y * CF_PANEL_W + x]) wrong++;
        if (!in && useScratch && v != 0) wrong++;                 // exact rect only
        if (!band && v != 0) wrong++;                             // never outside the band
      }
      CHECK(wrong == 0);
      if (useScratch) CHECK(maxBlock <= 1000);
      else            CHECK(calls == 1);
    }
  }
}

static void testPower() {
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

  CFStateMsg a, b;
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
// TIME convention (cf_link.cpp applyTime): RTC = UTC epoch + offset, and
// the face's inverse (claude_face.cpp buildInput: RTC fields -> epoch).
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
  // Known points for the inverse.
  CHECK(cfEpochFromCivil(2000, 1, 1, 0, 0, 0) == (int64_t)CF_UNIX_2000);
  CHECK(cfEpochFromCivil(2026, 9, 23, 12, 0, 0) == 1790164800LL);
  CHECK(cfEpochFromCivil(2024, 2, 29, 23, 59, 59) == 1709251199LL);   // leap day
  // Round trip every 6 h 17 min 23 s from 2000 to 2099 (hits all months,
  // leap days, year ends, every hour/minute band).
  int bad = 0, n = 0;
  for (int64_t t = (int64_t)CF_UNIX_2000; t < 4102444800LL; t += 22643) {
    CFCivil k = cfCivilFromEpoch(t);
    if (cfEpochFromCivil(k.year, k.month, k.day, k.hour, k.minute, k.second) != t) bad++;
    n++;
  }
  CHECK(bad == 0);
  CHECK(n > 100000);
}

// ---------------------------------------------------------------------------
// Link task line buffer (serial_io.cpp, 320 B) fits the biggest host->watch
// frame.
// ---------------------------------------------------------------------------
static void testLineFits() {
  CFStateMsg st;
  memset(&st, 0xAB, sizeof st);
  st.version = CF_PROTO_VERSION;
  char line[512];
  size_t n = cfEncodeFrame(CF_TYPE_STATE, &st, sizeof st, line, sizeof line);
  CHECK(n > 0);
  CHECK(n + 1 <= 320);             // + NUL; the reader keeps 319 chars max
  // The reader strips the '\n' before handing the line over.
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
  testOrientation();
  testGestureTable();
  testButton();
  testPushPlanner();
  testPower();
  testTimeConvention();
  testLineFits();
  printf("%s: %d/%d checks passed\n", gFails ? "FAILED" : "OK",
         gChecks - gFails, gChecks);
  return gFails ? 1 : 0;
}
