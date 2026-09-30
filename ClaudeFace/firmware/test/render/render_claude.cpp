// Host render of every Claude Face visual state -> PNGs + a contact sheet.
// Built by render.sh against the real Arduino_GFX 1.4.x sources (including
// canvas/Arduino_Canvas.cpp) and the real firmware drawing module
// (src/claude_face_draw.cpp).
//
// The face is LANDSCAPE (cf_orient.h). Every frame is drawn twice:
//   * VIEWER image: a plain 280x240 HostGFX, rotation 0 — what the person at
//     the desk sees, upright. These are the PNGs and the contact sheet.
//   * PANEL image: an Arduino_Canvas(240, 280) with setRotation(3), exactly
//     as the firmware draws into its frame canvas; its framebuffer is
//     the native portrait panel image that flush() / region pushes send.
// Then rotating the panel image by 90 degrees (the explicit formula below,
// not the cf_orient.h helpers) must reproduce the viewer image pixel for
// pixel, for every frame.
#include "hostgfx.h"
#include "canvas/Arduino_Canvas.h"
#include "claude_face_draw.h"
#include "cf_policy.h"
#include <zlib.h>
#include <vector>
#include <string>

static const int VW = CF_VIEW_W, VH = CF_VIEW_H;     // 280 x 240 viewer
static const int PW = CF_PANEL_W, PH = CF_PANEL_H;   // 240 x 280 panel
static const uint16_t UNPAINTED = 0xF81F;

// Viewer pixel (vx, vy) is shown by panel pixel (px, py) = (vy, 279 - vx):
// the watch lies with its physical top edge (py = 0) pointing right.
static void panelToViewerImage(const uint16_t *panel, uint16_t *viewer) {
  for (int vy = 0; vy < VH; vy++)
    for (int vx = 0; vx < VW; vx++)
      viewer[vy * VW + vx] = panel[(PH - 1 - vx) * PW + vy];
}

// The firmware's canvas: native 240x280 framebuffer, drawn in rotation 3.
struct PanelCanvas {
  Arduino_Canvas cv;
  PanelCanvas() : cv(PW, PH, nullptr, 0, 0, 0) { cv.begin(GFX_SKIP_OUTPUT_BEGIN); }
  uint16_t *fb() { return cv.getFramebuffer(); }
  void rot() { cv.setRotation(CF_CANVAS_ROTATION); }
  void unrot() { cv.setRotation(0); }
};
static const uint32_t NOW = 1790000000UL;   // 2026-09-21 ~14:13 UTC
static const char *OUT = "out";

struct Shot { std::string name; std::vector<uint16_t> fb; };
static std::vector<Shot> gShots;
static std::vector<CFDrawInput> gFaceInputs;
static std::vector<std::string> gFaceNames;
static int gRotFails = 0, gRotChecked = 0;
static std::string gNativeSample;       // which frame gets a native PNG

static void setStr(char *dst, size_t n, const char *s) {
  memset(dst, 0, n);
  strncpy(dst, s, n - 1);
}

static CFDrawInput base() {
  CFDrawInput in;
  memset(&in, 0, sizeof in);
  in.haveState = true;
  in.haveSessions = true;
  in.link = CF_LINK_LIVE;
  in.rxAgeMs = 1200;
  in.rtcOk = true; in.hour = 15; in.minute = 13;
  in.nowEpoch = NOW;
  in.utcOffsetMin = 60;
  in.nowMs = 123456;
  in.batOk = true; in.batPct = 87;
  CFStateMsg &s = in.st;
  s.version = CF_PROTO_VERSION; s.seq = 42;
  s.flags = CF_FLAGS_BRIDGE_OK;
  s.global_state = CF_STATE_WORKING;
  s.five_used = 46; s.five_resets_at = NOW + 2 * 3600 + 14 * 60; s.five_elapsed = 55; s.five_pace = -9;
  s.seven_used = 31; s.seven_resets_at = NOW + 3 * 86400 + 4 * 3600; s.seven_elapsed = 52; s.seven_pace = -21;
  s.rl_age_s = 40;
  s.session_count = 2; s.blocked_count = 0; s.subagent_count = 2;
  s.primary_index = 0;
  s.p_state = CF_STATE_WORKING; s.p_ctx = 41; s.p_model = CF_MODEL_OPUS; s.p_effort = CF_EFFORT_HIGH;
  s.p_flags = CF_PFLAGS_THINKING | CF_PFLAGS_CACHE_WARM;
  s.p_turn_elapsed_s = 187; s.p_last_turn_s = 95; s.p_tool_count = 14;
  setStr(s.p_current_tool, sizeof s.p_current_tool, "Bash|npm test");
  setStr(s.p_project, sizeof s.p_project, "EWatch_Firmware");
  setStr(s.p_model_name, sizeof s.p_model_name, "Opus 4.5");
  s.p_cache_expires_at = NOW + 280;
  s.p_lines_added = 214; s.p_lines_removed = 37;
  s.p_cost_cents = 342; s.cost_today_cents = 1288;
  s.p_pr_number = 57; s.p_pr_review = CF_PRREVIEW_APPROVED;
  s.bridge_now = NOW;
  CFSessionsMsg &ss = in.ss;
  ss.version = CF_PROTO_VERSION; ss.seq = 7; ss.count = 2;
  ss.rows[0].state = CF_STATE_WORKING; ss.rows[0].ctx = 41;
  setStr(ss.rows[0].project, sizeof ss.rows[0].project, "EWatch_Firm");
  ss.rows[1].state = CF_STATE_DONE; ss.rows[1].ctx = 12;
  setStr(ss.rows[1].project, sizeof ss.rows[1].project, "bridge");
  return in;
}

static void shot(const std::string &name, const CFDrawInput &in, bool detail = false) {
  // Viewer reference (no rotation anywhere).
  HostGFX *g = new HostGFX(VW, VH);
  g->fillScreen(UNPAINTED);                      // any unpainted pixel shows
  if (detail) cfDrawDetail(g, in);
  else { cfDrawFace(g, in); gFaceInputs.push_back(in); gFaceNames.push_back(name); }
  std::string p = std::string(OUT) + "/" + name + ".png";
  g->writePNG(p.c_str());

  // Real rotation path: Arduino_Canvas rotation 3 -> native framebuffer.
  PanelCanvas pc;
  pc.unrot(); pc.cv.fillScreen(UNPAINTED);
  pc.rot();
  if (detail) cfDrawDetail(&pc.cv, in); else cfDrawFace(&pc.cv, in);
  pc.unrot();
  std::vector<uint16_t> back((size_t)VW * VH);
  panelToViewerImage(pc.fb(), back.data());
  int d = 0;
  for (size_t k = 0; k < back.size(); k++) if (back[k] != g->fb[k]) d++;
  gRotChecked++;
  if (d) { printf("ROTATION FAIL %s: %d px differ between rotated panel and viewer\n", name.c_str(), d); gRotFails++; }
  if (name == gNativeSample) {
    std::string np = std::string(OUT) + "/native_panel_" + name + ".png";
    writePNG565(np.c_str(), pc.fb(), PW, PH);
  }

  Shot s; s.name = name; s.fb = g->fb;
  gShots.push_back(s);
  delete g;
}

// ---- contact sheet ----
static void writeSheet(const char *path, int cols) {
  const int tw = VW, lh = 14, th = VH + lh, gap = 8;
  int n = (int)gShots.size(), rows = (n + cols - 1) / cols;
  int W = cols * tw + (cols + 1) * gap, H = rows * th + (rows + 1) * gap;
  std::vector<uint16_t> img((size_t)W * H, 0x39E7);
  for (int i = 0; i < n; i++) {
    int ox = gap + (i % cols) * (tw + gap), oy = gap + (i / cols) * (th + gap);
    for (int y = 0; y < VH; y++)
      memcpy(&img[(size_t)(oy + y) * W + ox], &gShots[i].fb[(size_t)y * VW], VW * 2);
    HostGFX l(tw, lh); l.setTextColor(0xFFFF); l.setCursor(2, 3); l.print(gShots[i].name.c_str());
    for (int y = 0; y < lh; y++)
      memcpy(&img[(size_t)(oy + VH + y) * W + ox], &l.fb[(size_t)y * tw], tw * 2);
  }
  writePNG565(path, img.data(), W, H);
}

static int diffPx(const uint16_t *a, const uint16_t *b, size_t n) {
  int d = 0;
  for (size_t i = 0; i < n; i++) if (a[i] != b[i]) d++;
  return d;
}

// Dirty-region soundness checks (the firmware pushes only changed regions):
//  0. geometry: regions tile the canvas as documented; the swipe session
//     zone is exactly ACTIVITY + FOOTER; the rect mapping agrees with the
//     point mapping and the canvas.
//  1. containment: a region never paints outside its rect (viewer space);
//  2. incremental == full, through the REAL firmware path: panel starts with
//     frame i-1; canvas (rotation 3) redraws only the regions whose key
//     changed (+ the animation region); cfDirtyRects -> cfViewRectToPanel ->
//     cfPushPanelRect (small scratch, like the firmware) copies them into the
//     panel; the panel must equal a full native redraw of frame i;
//  3. stability: a static state's keys don't change when only time passes
//     within the same minute.
static int checkRegions() {
  static const uint8_t kR[5] = { CF_RGN_STATUS, CF_RGN_HERO, CF_RGN_CENTRE,
                                 CF_RGN_ACTIVITY, CF_RGN_FOOTER };
  int fails = 0;

  // 0. geometry
  {
    std::vector<int> cover((size_t)VW * VH, 0);
    const uint8_t tiles[4] = { CF_RGN_STATUS, CF_RGN_HERO, CF_RGN_ACTIVITY, CF_RGN_FOOTER };
    for (int t = 0; t < 4; t++) {
      CFRect r = cfRegionRect(tiles[t]);
      for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++)
        if (x >= 0 && y >= 0 && x < VW && y < VH) cover[(size_t)y * VW + x]++;
    }
    int bad = 0;
    for (size_t k = 0; k < cover.size(); k++) if (cover[k] != 1) bad++;
    if (bad) { printf("GEOMETRY FAIL: %d px not covered exactly once by the 4 tiles\n", bad); fails++; }
    CFRect h = cfRegionRect(CF_RGN_HERO), c = cfRegionRect(CF_RGN_CENTRE);
    if (c.x < h.x || c.y < h.y || c.x + c.w > h.x + h.w || c.y + c.h > h.y + h.h) {
      printf("GEOMETRY FAIL: CENTRE not inside HERO\n"); fails++;
    }
    // rect mapping vs point mapping vs the canvas itself, for every region
    for (int r = 0; r < 5; r++) {
      CFRect lr = cfRegionRect(kR[r]), pr = cfViewRectToPanel(lr);
      PanelCanvas pc; pc.unrot(); pc.cv.fillScreen(0); pc.rot();
      pc.cv.fillRect(lr.x, lr.y, lr.w, lr.h, 0xFFFF); pc.unrot();
      int mis = 0;
      for (int py = 0; py < PH; py++) for (int px = 0; px < PW; px++) {
        bool inP = px >= pr.x && px < pr.x + pr.w && py >= pr.y && py < pr.y + pr.h;
        if ((pc.fb()[py * PW + px] == 0xFFFF) != inP) mis++;
        int16_t vx, vy; cfPanelToView((int16_t)px, (int16_t)py, vx, vy);
        bool inV = vx >= lr.x && vx < lr.x + lr.w && vy >= lr.y && vy < lr.y + lr.h;
        if (inV != inP) mis++;
      }
      if (mis) { printf("MAPPING FAIL region 0x%02x: %d mismatches\n", kR[r], mis); fails++; }
    }
  }

  // 1. containment
  HostGFX *a = new HostGFX(VW, VH);
  for (size_t i = 0; i < gFaceInputs.size(); i++) {
    for (int r = 0; r < 5; r++) {
      a->fillScreen(UNPAINTED);
      cfDrawRegions(a, gFaceInputs[i], kR[r]);
      CFRect rc = cfRegionRect(kR[r]);
      int out = 0;
      for (int y = 0; y < VH; y++) for (int x = 0; x < VW; x++) {
        bool inside = x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
        if (!inside && a->fb[(size_t)y * VW + x] != UNPAINTED) out++;
      }
      if (out) { printf("CONTAIN FAIL %s region 0x%02x: %d px outside\n", gFaceNames[i].c_str(), kR[r], out); fails++; }
    }
  }
  delete a;

  // 2. incremental == full through canvas rotation + panel pushes
  const CFRect cr = cfRegionRect(CF_RGN_CENTRE);
  const size_t cap = (size_t)cr.w * cr.h;           // firmware scratch size
  std::vector<uint16_t> scratch(cap), panel((size_t)PW * PH);
  uint32_t sinkCalls = 0, pushedPx = 0;
  for (size_t i = 1; i < gFaceInputs.size(); i++) {
    const CFDrawInput &p = gFaceInputs[i - 1], &c = gFaceInputs[i];
    PanelCanvas cv; cv.rot();
    cfDrawFace(&cv.cv, p); cv.unrot();
    memcpy(panel.data(), cv.fb(), panel.size() * 2);        // frame i-1 on the panel
    uint8_t dirty = 0;
    for (int r = 0; r < 5; r++)
      if (cfRegionKey(p, kR[r]) != cfRegionKey(c, kR[r])) dirty |= kR[r];
    dirty |= cfAnimation(c).region;
    cv.rot(); cfDrawRegions(&cv.cv, c, dirty); cv.unrot();
    CFRect rs[4]; int n = cfDirtyRects(dirty, rs);
    for (int k = 0; k < n; k++) {
      sinkCalls += cfPushPanelRect(cv.fb(), cfViewRectToPanel(rs[k]), scratch.data(), cap,
        [&](int16_t x, int16_t y, uint16_t *src, int16_t w, int16_t h) {
          for (int yy = 0; yy < h; yy++)
            memcpy(&panel[(size_t)(y + yy) * PW + x], src + (size_t)yy * w, (size_t)w * 2);
          pushedPx += (uint32_t)w * h;
        });
    }
    PanelCanvas full; full.rot(); cfDrawFace(&full.cv, c); full.unrot();
    int d = diffPx(panel.data(), full.fb(), panel.size());
    if (d) { printf("INCREMENTAL FAIL %s -> %s: %d panel px differ (dirty=0x%02x)\n",
                    gFaceNames[i - 1].c_str(), gFaceNames[i].c_str(), d, dirty); fails++; }
  }
  printf("incremental: %zu transitions, %u push calls, %u px pushed\n",
         gFaceInputs.size() - 1, sinkCalls, pushedPx);

  // 3. stability
  for (size_t i = 0; i < gFaceInputs.size(); i++) {
    CFDrawInput c = gFaceInputs[i];
    if (cfAnimation(c).region) continue;
    CFDrawInput l = c; l.nowMs += 7000; l.rxAgeMs += 7000;      // same minute
    if (c.st.p_state == CF_STATE_WORKING || c.st.p_state == CF_STATE_BLOCKED) continue;
    for (int r = 0; r < 5; r++)
      if (cfRegionKey(c, kR[r]) != cfRegionKey(l, kR[r])) {
        printf("UNSTABLE KEY %s region 0x%02x\n", gFaceNames[i].c_str(), kR[r]); fails++;
      }
  }
  printf("region checks: %d failure(s)\n", fails);
  return fails;
}

// The rotation checker must be able to fail: draw one frame through rotation
// 1 (watch top pointing LEFT) and through a one-pixel-shifted viewer; both
// must be caught by the same comparison shot() uses.
static int selfTestRotationChecker(const CFDrawInput &in) {
  HostGFX g(VW, VH); cfDrawFace(&g, in);
  int caught = 0;
  {
    PanelCanvas pc; pc.cv.setRotation(1); cfDrawFace(&pc.cv, in); pc.unrot();
    std::vector<uint16_t> back((size_t)VW * VH); panelToViewerImage(pc.fb(), back.data());
    if (diffPx(back.data(), g.fb.data(), back.size()) > 0) caught++;
  }
  {
    PanelCanvas pc; pc.rot(); cfDrawFace(&pc.cv, in); pc.cv.drawPixel(140, 5, 0x1234); pc.unrot();
    std::vector<uint16_t> back((size_t)VW * VH); panelToViewerImage(pc.fb(), back.data());
    if (diffPx(back.data(), g.fb.data(), back.size()) > 0) caught++;
  }
  printf("rotation checker self-test: %d/2 deliberate faults caught\n", caught);
  return caught == 2 ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc > 1) OUT = argv[1];
  gNativeSample = "03_working_phase0";
  CFDrawInput in;

  // Idle / none
  in = base(); in.st.global_state = CF_STATE_NONE; in.st.p_state = CF_STATE_NONE;
  in.st.primary_index = 0xFF; in.st.p_project[0] = 0; in.st.session_count = 0; in.ss.count = 0;
  in.st.subagent_count = 0; in.st.p_turn_elapsed_s = 0;
  shot("01_none", in);
  in = base(); in.st.global_state = CF_STATE_IDLE; in.st.p_state = CF_STATE_IDLE;
  in.st.p_turn_elapsed_s = 0; in.st.subagent_count = 0; in.st.p_current_tool[0] = 0;
  in.ss.rows[0].state = CF_STATE_IDLE;
  shot("02_idle", in);

  // WORKING, three animation phases
  for (int i = 0; i < 3; i++) {
    in = base(); in.nowMs = 100000 + i * 660; in.rxAgeMs = 1200 + i * 660;
    char n[32]; snprintf(n, sizeof n, "03_working_phase%d", i);
    shot(n, in);
  }
  in = base(); in.st.global_state = CF_STATE_COMPACTING; in.st.p_state = CF_STATE_COMPACTING;
  in.ss.rows[0].state = CF_STATE_COMPACTING; setStr(in.st.p_current_tool, 24, "compacting");
  shot("04_compacting", in);

  in = base(); in.st.global_state = CF_STATE_DONE; in.st.p_state = CF_STATE_DONE;
  in.st.p_turn_elapsed_s = 0; in.st.p_cache_expires_at = NOW + 1800; in.ss.rows[0].state = CF_STATE_DONE;
  in.st.p_flags |= CF_PFLAGS_UNACKED_DONE; in.st.subagent_count = 0;
  shot("05_done", in);
  in = base(); in.st.global_state = CF_STATE_WAITING_INPUT; in.st.p_state = CF_STATE_WAITING_INPUT;
  in.st.p_turn_elapsed_s = 0; in.st.p_cache_expires_at = NOW + 1800; in.ss.rows[0].state = CF_STATE_WAITING_INPUT;
  shot("06_waiting", in);

  // BLOCKED pulse on / off
  in = base(); in.st.global_state = CF_STATE_BLOCKED; in.st.p_state = CF_STATE_BLOCKED;
  in.st.blocked_count = 1; in.ss.rows[0].state = CF_STATE_BLOCKED;
  setStr(in.st.p_current_tool, 24, "Bash|rm -rf build");
  in.nowMs = 250; shot("07_blocked_pulse_on", in);
  in.nowMs = 750; shot("08_blocked_pulse_off", in);

  // LIMITED
  in = base(); in.st.global_state = CF_STATE_LIMITED; in.st.p_state = CF_STATE_LIMITED;
  in.st.five_used = 100; in.st.five_elapsed = 71; in.st.five_pace = 29;
  in.st.five_resets_at = NOW + 83 * 60; in.ss.rows[0].state = CF_STATE_LIMITED;
  in.st.p_turn_elapsed_s = 0;
  shot("09_limited", in);

  // OFFLINE with last-known values (4 min), and never connected
  in = base(); in.link = CF_LINK_OFFLINE; in.rxAgeMs = 4 * 60 * 1000 + 10000;
  shot("10_offline_4m", in);
  in = base(); in.link = CF_LINK_NEVER; in.haveState = false; in.haveSessions = false;
  memset(&in.st, 0, sizeof in.st); memset(&in.ss, 0, sizeof in.ss);
  shot("11_never_connected", in);

  // Stale rate limits (hollow / dimmed rings)
  in = base(); in.st.rl_age_s = 1500; in.st.flags |= CF_FLAGS_RL_STALE;
  shot("12_rl_stale", in);
  // API-key session: no rate limits -> dashes
  in = base(); in.st.flags |= CF_FLAGS_NO_RATE_LIMITS; in.st.five_used = CF_PCT_UNKNOWN;
  in.st.seven_used = CF_PCT_UNKNOWN; in.st.five_elapsed = CF_PCT_UNKNOWN;
  in.st.seven_elapsed = CF_PCT_UNKNOWN; in.st.five_pace = CF_PACE_UNKNOWN; in.st.seven_pace = CF_PACE_UNKNOWN;
  in.st.five_resets_at = 0; in.st.seven_resets_at = 0; in.st.p_ctx = CF_PCT_UNKNOWN;
  in.st.p_cost_cents = CF_U16_UNKNOWN;
  shot("13_no_rate_limits", in);

  // Pace: ahead (burning fast) with projected hit, and far behind
  in = base(); in.st.five_used = 78; in.st.five_elapsed = 40; in.st.five_pace = 38;
  in.st.seven_used = 64; in.st.seven_elapsed = 50; in.st.seven_pace = 14;
  in.st.projected_hit = NOW + 95 * 60;
  shot("14_pace_ahead_projected", in);
  in = base(); in.st.five_used = 12; in.st.five_elapsed = 70; in.st.five_pace = -58;
  in.st.seven_used = 20; in.st.seven_elapsed = 85; in.st.seven_pace = -65;
  shot("15_pace_behind", in);

  // Version mismatch
  in = base(); in.versionMismatch = true;
  shot("16_version_mismatch", in);

  // 4 sessions + "+2", 7 subagents (5 dots)
  in = base(); in.st.session_count = 6; in.st.subagent_count = 7; in.ss.count = 4;
  in.ss.rows[1].state = CF_STATE_BLOCKED; setStr(in.ss.rows[1].project, 12, "claude-cfg");
  in.ss.rows[2].state = CF_STATE_DONE;    setStr(in.ss.rows[2].project, 12, "website");
  in.ss.rows[3].state = CF_STATE_IDLE;    setStr(in.ss.rows[3].project, 12, "scrubmarine");
  in.st.blocked_count = 1; in.st.global_state = CF_STATE_BLOCKED;
  in.nowMs = 250;
  shot("17_sessions_plus2_subagents", in);

  // Context bar colours
  const uint8_t ctxs[3] = { 65, 75, 92 };
  for (int i = 0; i < 3; i++) {
    in = base(); in.st.p_ctx = ctxs[i];
    char n[32]; snprintf(n, sizeof n, "18_ctx_%u", ctxs[i]);
    shot(n, in);
  }

  // Cache cold in 3m (my turn)
  in = base(); in.st.global_state = CF_STATE_DONE; in.st.p_state = CF_STATE_DONE;
  in.st.p_turn_elapsed_s = 0; in.st.p_cache_expires_at = NOW + 170; in.ss.rows[0].state = CF_STATE_DONE;
  in.ss.count = 3; in.st.session_count = 3; in.ss.rows[2].state = CF_STATE_WORKING;
  setStr(in.ss.rows[2].project, 12, "website");
  shot("19_cache_cold", in);

  // Long turn timer + no battery reading + no RTC
  in = base(); in.st.p_turn_elapsed_s = 3 * 3600 + 25 * 60 + 7; in.batOk = false; in.rtcOk = false;
  setStr(in.st.p_current_tool, 24, "Edit|claude_face_draw.cpp");
  in.st.p_flags |= CF_PFLAGS_HUNG;
  shot("20_long_turn_nobat_nortc", in);

  // Detail page
  in = base(); in.st.projected_hit = NOW + 95 * 60; in.st.p_flags |= CF_PFLAGS_FAST_MODE;
  shot("21_detail", in, true);
  in = base(); in.st.flags |= CF_FLAGS_NO_RATE_LIMITS; in.st.p_pr_number = 0;
  in.st.p_cost_cents = CF_U16_UNKNOWN; in.st.p_model_name[0] = 0; in.st.p_effort = 0;
  shot("22_detail_unknowns", in, true);

  std::string sheet = std::string(OUT) + "/contact_sheet.png";
  writeSheet(sheet.c_str(), 5);

  // Self-check: no magenta (unpainted) pixels may survive in any full frame.
  int bad = 0;
  for (size_t i = 0; i < gShots.size(); i++) {
    int m = 0;
    for (size_t p = 0; p < gShots[i].fb.size(); p++) if (gShots[i].fb[p] == UNPAINTED) m++;
    if (m) { printf("UNPAINTED %s: %d px\n", gShots[i].name.c_str(), m); bad++; }
  }
  printf("rendered %zu frames -> %s/ (%d with unpainted pixels)\n", gShots.size(), OUT, bad);
  printf("rotation: %d/%d frames: rotated native panel == viewer image (%d fail)\n",
         gRotChecked - gRotFails, gRotChecked, gRotFails);
  bad += gRotFails;
  bad += selfTestRotationChecker(base());
  bad += checkRegions();
  return bad ? 1 : 0;
}
