// Host render of every Claude Face visual state -> PNGs + a contact sheet.
// Built by render.sh against the real Arduino_GFX 1.4.x sources and the real
// firmware drawing module (src/apps/claude/claude_face_draw.cpp).
#include "hostgfx.h"
#include "claude_face_draw.h"
#include <zlib.h>
#include <vector>
#include <string>

static const uint32_t NOW = 1790000000UL;   // 2026-09-21 ~14:13 UTC
static const char *OUT = "out";

struct Shot { std::string name; std::vector<uint16_t> fb; };
static std::vector<Shot> gShots;
static std::vector<CFDrawInput> gFaceInputs;
static std::vector<std::string> gFaceNames;

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
  HostGFX *g = new HostGFX();
  g->fillScreen(0xF81F);                       // magenta: any unpainted pixel shows
  if (detail) cfDrawDetail(g, in); else { cfDrawFace(g, in); gFaceInputs.push_back(in); gFaceNames.push_back(name); }
  std::string p = std::string(OUT) + "/" + name + ".png";
  g->writePNG(p.c_str());
  Shot s; s.name = name; s.fb.assign(g->fb, g->fb + 240 * 280);
  gShots.push_back(s);
  delete g;
}

// ---- contact sheet ----
class LabelGFX : public Arduino_GFX {
public:
  uint16_t fb[240 * 14];
  LabelGFX() : Arduino_GFX(240, 14) { memset(fb, 0, sizeof fb); }
  bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { fb[y * 240 + x] = c; }
};
static void be32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (i * 8))); }
static void chunk(FILE *f, const char *t, const std::vector<uint8_t> &d) {
  std::vector<uint8_t> b; be32(b, (uint32_t)d.size()); fwrite(b.data(), 1, 4, f);
  std::vector<uint8_t> c(t, t + 4); c.insert(c.end(), d.begin(), d.end());
  fwrite(c.data(), 1, c.size(), f);
  uint32_t crc = (uint32_t)crc32(0, c.data(), (uInt)c.size());
  b.clear(); be32(b, crc); fwrite(b.data(), 1, 4, f);
}
static void writeSheet(const char *path, int cols) {
  const int tw = 240, th = 280 + 14, gap = 8;
  int n = (int)gShots.size(), rows = (n + cols - 1) / cols;
  int W = cols * tw + (cols + 1) * gap, H = rows * th + (rows + 1) * gap;
  std::vector<uint16_t> img((size_t)W * H, 0x39E7);
  for (int i = 0; i < n; i++) {
    int ox = gap + (i % cols) * (tw + gap), oy = gap + (i / cols) * (th + gap);
    for (int y = 0; y < 280; y++)
      memcpy(&img[(size_t)(oy + y) * W + ox], &gShots[i].fb[y * 240], 240 * 2);
    LabelGFX l; l.setTextColor(0xFFFF); l.setCursor(2, 3); l.print(gShots[i].name.c_str());
    for (int y = 0; y < 14; y++)
      memcpy(&img[(size_t)(oy + 280 + y) * W + ox], &l.fb[y * 240], 240 * 2);
  }
  FILE *f = fopen(path, "wb");
  const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; fwrite(sig, 1, 8, f);
  std::vector<uint8_t> h; be32(h, W); be32(h, H); h.push_back(8); h.push_back(2);
  h.push_back(0); h.push_back(0); h.push_back(0); chunk(f, "IHDR", h);
  std::vector<uint8_t> raw;
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    for (int x = 0; x < W; x++) {
      uint16_t c = img[(size_t)y * W + x];
      raw.push_back((uint8_t)(((c >> 11) & 31) * 255 / 31));
      raw.push_back((uint8_t)(((c >> 5) & 63) * 255 / 63));
      raw.push_back((uint8_t)((c & 31) * 255 / 31));
    }
  }
  uLongf zn = compressBound(raw.size()); std::vector<uint8_t> z(zn);
  compress(z.data(), &zn, raw.data(), raw.size()); z.resize(zn);
  chunk(f, "IDAT", z); chunk(f, "IEND", std::vector<uint8_t>()); fclose(f);
}

static int diffPx(const HostGFX &a, const HostGFX &b) {
  int d = 0;
  for (int i = 0; i < 240 * 280; i++) if (a.fb[i] != b.fb[i]) d++;
  return d;
}

// Dirty-region soundness checks (the firmware pushes only changed regions):
//  1. containment: a region never paints outside its rect;
//  2. incremental == full: starting from frame i-1, redrawing only the
//     regions whose key changed (+ the animation region) must reproduce
//     frame i exactly;
//  3. stability: a static state's keys don't change when only time passes
//     within the same minute.
static int checkRegions() {
  static const uint8_t kR[5] = { CF_RGN_STATUS, CF_RGN_HERO, CF_RGN_CENTRE,
                                 CF_RGN_ACTIVITY, CF_RGN_FOOTER };
  int fails = 0;
  HostGFX *a = new HostGFX(), *b = new HostGFX();
  for (size_t i = 0; i < gFaceInputs.size(); i++) {
    for (int r = 0; r < 5; r++) {
      a->fillScreen(0xF81F);
      cfDrawRegions(a, gFaceInputs[i], kR[r]);
      CFRect rc = cfRegionRect(kR[r]);
      int out = 0;
      for (int y = 0; y < 280; y++) for (int x = 0; x < 240; x++) {
        bool inside = x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
        if (!inside && a->fb[y * 240 + x] != 0xF81F) out++;
      }
      if (out) { printf("CONTAIN FAIL %s region 0x%02x: %d px outside\n", gFaceNames[i].c_str(), kR[r], out); fails++; }
    }
  }
  for (size_t i = 1; i < gFaceInputs.size(); i++) {
    const CFDrawInput &p = gFaceInputs[i - 1], &c = gFaceInputs[i];
    cfDrawFace(a, p);
    uint8_t dirty = 0;
    for (int r = 0; r < 5; r++)
      if (cfRegionKey(p, kR[r]) != cfRegionKey(c, kR[r])) dirty |= kR[r];
    dirty |= cfAnimation(c).region;
    cfDrawRegions(a, c, dirty);
    cfDrawFace(b, c);
    int d = diffPx(*a, *b);
    if (d) { printf("INCREMENTAL FAIL %s -> %s: %d px differ (dirty=0x%02x)\n",
                    gFaceNames[i - 1].c_str(), gFaceNames[i].c_str(), d, dirty); fails++; }
  }
  // stability
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
  delete a; delete b;
  printf("region checks: %d failure(s)\n", fails);
  return fails;
}

int main(int argc, char **argv) {
  if (argc > 1) OUT = argv[1];
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
  writeSheet(sheet.c_str(), 6);

  // Self-check: no magenta (unpainted) pixels may survive in any full frame.
  int bad = 0;
  for (size_t i = 0; i < gShots.size(); i++) {
    int m = 0;
    for (int p = 0; p < 240 * 280; p++) if (gShots[i].fb[p] == 0xF81F) m++;
    if (m) { printf("UNPAINTED %s: %d px\n", gShots[i].name.c_str(), m); bad++; }
  }
  printf("rendered %zu frames -> %s/ (%d with unpainted pixels)\n", gShots.size(), OUT, bad);
  bad += checkRegions();
  return bad ? 1 : 0;
}
