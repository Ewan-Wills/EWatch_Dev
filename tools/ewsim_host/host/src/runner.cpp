// ewsim_host runner: boots the firmware on the simulated clock, plays an ewt
// step script through the watch's own console (`sim <seq> ...`, exactly what
// ewt sends over USB), films the ST7789 panel model at a constant frame rate
// with ewt-record-style overlays and encodes it with ffmpeg.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "host_sim.h"
#include "ewsim.h"
#include "view.h"
#include <string>
#include <vector>
#include <regex>
#include <set>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>

extern const char *const kEwsimScreens[];   // generated from the project's model.h (preprocessed)
extern const int kEwsimScreenCount;
extern const char *const kEwsimProject;

static const int W = 240, H = 280;

// ---------------------------------------------------------------- options
struct Opts {
  std::string script, out, snapDir = ".", fsRoot, fsSeed;
  int fps = 30, scale = 4, lead = 600, hold = 1000, boot = 1500;
  bool smooth = false, shortFmt = false, verbose = false, allowUnsupported = false, sleep = false;
  bool listScreens = false, check = false;
  uint64_t seed = 1;
  int Y = 2026, Mo = 10, D = 7, h = 10, mi = 9, s = 30;
  std::vector<std::string> prefs;
} O;

// ---------------------------------------------------------------- PNG
static bool pngWrite(const std::string &path, const uint16_t *fb) {
  std::vector<uint8_t> raw;
  raw.reserve((W * 3 + 1) * H);
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    for (int x = 0; x < W; x++) {
      uint16_t v = fb[y * W + x];
      int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      raw.push_back((uint8_t)((r << 3) | (r >> 2)));
      raw.push_back((uint8_t)((g << 2) | (g >> 4)));
      raw.push_back((uint8_t)((b << 3) | (b >> 2)));
    }
  }
  uLongf zl = compressBound(raw.size());
  std::vector<uint8_t> z(zl);
  compress2(z.data(), &zl, raw.data(), raw.size(), 6);
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  auto be32 = [&](uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); };
  auto chunk = [&](const char *t, const uint8_t *d, uint32_t n) {
    be32(n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)t, 4); c = crc32(c, d, n); be32((uint32_t)c);
  };
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  uint8_t ihdr[13] = { 0, 0, 0, (uint8_t)W, 0, 0, (uint8_t)(H >> 8), (uint8_t)H, 8, 2, 0, 0, 0 };
  ihdr[2] = (uint8_t)(W >> 8);
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), (uint32_t)zl);
  chunk("IEND", nullptr, 0);
  return fclose(f) == 0;
}

static bool uniform(const uint16_t *fb, uint16_t *col) {
  for (int i = 1; i < W * H; i++) if (fb[i] != fb[0]) return false;
  if (col) *col = fb[0];
  return true;
}

// ---------------------------------------------------------------- recorder (capture task)
struct InputTrack { bool down = false; int x = 0, y = 0; int64_t upAt = -1; };
static struct Rec {
  bool on = false, stop = false, stopped = false;
  FILE *ff = nullptr;
  uint64_t t0us = 0;
  uint64_t frames = 0, distinct = 0, nonUniform = 0;
  uLong lastCrc = 0;
  InputTrack finger, button;
  std::vector<int> cut;
} R;

static void blend(uint16_t &v, int cr, int cg, int cb, double a) {
  int r = ((v >> 11) & 31) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
  r = (int)(r + (cr - r) * a); g = (int)(g + (cg - g) * a); b = (int)(b + (cb - b) * a);
  v = (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

// Same overlays as ewt's Recorder.compose: rounded panel corners (r 36),
// a 15 px finger dot (fades over 220 ms after release), orange side-button
// bar on the right edge (held + 200 ms).
static void compose(uint16_t *f, int64_t tms) {
  const InputTrack &F = R.finger, &B = R.button;
  int64_t since = F.upAt >= 0 ? tms - F.upAt : -1;
  if (F.down || (since >= 0 && since < 220)) {
    double a = F.down ? 0.55 : 0.55 * (1 - since / 220.0);
    int cx = F.x, cy = F.y, r = 15;
    for (int yy = std::max(0, cy - r - 2); yy < std::min(H, cy + r + 3); yy++)
      for (int xx = std::max(0, cx - r - 2); xx < std::min(W, cx + r + 3); xx++) {
        double d = sqrt((double)(xx - cx) * (xx - cx) + (double)(yy - cy) * (yy - cy));
        if (d <= r) blend(f[yy * W + xx], 255, 255, 255, a);
        else if (d <= r + 2) blend(f[yy * W + xx], 40, 40, 40, a);
      }
  }
  int64_t bs = B.upAt >= 0 ? tms - B.upAt : -1;
  if (B.down || (bs >= 0 && bs < 200))
    for (int yy = 110; yy < 170; yy++)
      for (int xx = W - 7; xx < W; xx++) blend(f[yy * W + xx], 255, 140, 40, 0.85);
  for (int i : R.cut) f[i] = 0;
}

static void sampleInputs() {
  bool live = false; uint16_t x = 0, y = 0;
  ewsimTouch(live, x, y);
  int64_t now = (int64_t)(sim_now_us() / 1000);
  if (live) { R.finger.down = true; R.finger.x = x; R.finger.y = y; }
  else if (R.finger.down) { R.finger.down = false; R.finger.upAt = now; }
  bool b = ewsimButton();
  if (b) R.button.down = true;
  else if (R.button.down) { R.button.down = false; R.button.upAt = now; }
}

static void captureTask(void *) {
  static uint16_t fb[W * H];
  uint64_t k = 0;
  for (;;) {
    if (!R.on) { vTaskDelay(5); continue; }
    if (R.stop) { R.stopped = true; R.on = false; vTaskDelay(portMAX_DELAY); }
    uint64_t due = R.t0us + k * 1000000ULL / (uint64_t)O.fps;
    uint64_t now = sim_now_us();
    sampleInputs();
    if (now + 500 < due) {
      uint64_t ms = (due - now) / 1000;
      vTaskDelay(ms > 5 ? 5 : (ms ? ms : 1));
      continue;
    }
    ewsimPanelFrame(fb, true);
    uLong c = crc32(0, (const Bytef *)fb, sizeof fb);
    if (!k || c != R.lastCrc) R.distinct++;
    R.lastCrc = c;
    if (!uniform(fb, nullptr)) R.nonUniform++;
    compose(fb, (int64_t)(due / 1000));
    if (R.ff) fwrite(fb, 2, W * H, R.ff);
    R.frames++;
    k++;
  }
}

// ---------------------------------------------------------------- console link (host side)
static int sSeq = 0;
static size_t sMark = 0;          // TX offset at the last input step (for expect log/noerr)

static std::string txFrom(size_t off) {
  size_t n = host_serial_tx_size();
  return off < n ? std::string(host_serial_tx_data() + off, n - off) : std::string();
}

// Send `sim <seq> <cmd>` and wait for `S:<seq> OK|ERR ...` (sim time).
static bool sim(const std::string &cmd, std::string &reply, uint32_t timeoutMs = 15000) {
  int seq = ++sSeq;
  size_t off = host_serial_tx_size();
  std::string line = "sim " + std::to_string(seq) + " " + cmd + "\n";
  host_serial_inject(line.c_str());
  std::string tag = "S:" + std::to_string(seq) + " ";
  uint64_t dl = sim_now_us() + (uint64_t)timeoutMs * 1000;
  while (sim_now_us() < dl) {
    vTaskDelay(1);
    std::string tx = txFrom(off);
    size_t p = tx.find(tag);
    while (p != std::string::npos && p > 0 && tx[p - 1] != '\n') p = tx.find(tag, p + 1);
    if (p != std::string::npos) {
      size_t e = tx.find('\n', p);
      if (e == std::string::npos) continue;
      reply = tx.substr(p + tag.size(), e - p - tag.size());
      return true;
    }
  }
  reply = "no reply within " + std::to_string(timeoutMs) + " ms (console paused or no ewsim in this firmware?)";
  return false;
}

static std::vector<std::string> linesSince(size_t off) {
  std::vector<std::string> out;
  std::string tx = txFrom(off), cur;
  for (char c : tx) {
    if (c == '\n') { out.push_back(cur); cur.clear(); }
    else if (c != '\r') cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static const std::regex kErrRe(R"(\] E/|^E \(\d+\)|\bERR\b)");

static int screenId(const std::string &s) {
  if (!s.empty() && isdigit((unsigned char)s[0])) return atoi(s.c_str());
  for (int i = 0; i < kEwsimScreenCount; i++)
    if (strcasecmp(kEwsimScreens[i], s.c_str()) == 0) return i;
  return -1;
}
static std::string screenList() {
  std::string l;
  for (int i = 0; i < kEwsimScreenCount; i++) { l += kEwsimScreens[i]; l += ' '; }
  return l;
}

// ---------------------------------------------------------------- script
static std::vector<std::string> splitSteps(const std::string &text) {
  std::vector<std::string> steps;
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
    i = e == std::string::npos ? text.size() + 1 : e + 1;
    std::string t = line;
    t.erase(0, t.find_first_not_of(" \t"));
    if (t.rfind("expect", 0) != 0) { size_t h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h); }
    size_t a = 0;
    while (a <= line.size()) {
      size_t b = line.find(';', a);
      std::string s = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
      s.erase(0, s.find_first_not_of(" \t\r"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
      if (!s.empty() && s[0] != '#') steps.push_back(s);
      a = b == std::string::npos ? line.size() + 1 : b + 1;
    }
  }
  return steps;
}

static std::vector<std::string> words(const std::string &s) {
  std::vector<std::string> w;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < s.size() && !isspace((unsigned char)s[j])) j++;
    if (j > i) w.push_back(s.substr(i, j - i));
    i = j;
  }
  return w;
}

static bool isInt(const std::string &s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-') ? 1 : 0;
  if (i >= s.size()) return false;
  for (; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

// Validate the whole script before the watch boots: a typo or an unknown
// screen must fail loudly, not render 10 s of the wrong thing.
static std::string validate(const std::string &step) {
  auto a = words(step);
  const std::string &c = a[0];
  auto nInts = [&](size_t from, size_t need) {
    if (a.size() < from + need) return false;
    for (size_t i = from; i < from + need; i++) if (!isInt(a[i])) return false;
    return true;
  };
  if (c == "tap" || c == "dtap") return nInts(1, 2) ? "" : "usage: " + c + " X Y";
  if (c == "long") return nInts(1, 2) ? "" : "usage: long X Y [MS]";
  if (c == "drag") return nInts(1, 4) ? "" : "usage: drag X1 Y1 X2 Y2 [MS]";
  if (c == "swipe") return a.size() == 2 && a[1].size() == 1 && strchr("udlr", a[1][0]) ? "" : "usage: swipe u|d|l|r";
  if (c == "gest") return nInts(1, 1) ? "" : "usage: gest CODE [X Y]";
  if (c == "stroke") return a.size() >= 4 ? "" : "usage: stroke DT X Y [X Y ...] [| X Y ...]";
  if (c == "touch") return (a.size() == 2 && a[1] == "up") || (a.size() >= 4 && nInts(2, 2)) ? "" : "usage: touch down X Y | touch up";
  if (c == "btn") {
    static const std::set<std::string> ok = { "short", "long", "vlong", "down", "up" };
    if (a.size() == 2 && ok.count(a[1])) return "";
    if (a.size() == 3 && a[1] == "hold" && isInt(a[2])) return "";
    return "usage: btn short|long|vlong|down|up|hold MS";
  }
  if (c == "tilt") return (a.size() == 2 && a[1] == "off") || a.size() == 4 ? "" : "usage: tilt GX GY GZ | tilt off";
  if (c == "shake" || c == "wake" || c == "state" || c == "xcheck" || c == "show") return "";
  if (c == "wait") return nInts(1, 1) ? "" : "usage: wait MS";
  if (c == "goto") {
    if (a.size() != 2) return "usage: goto SCREEN";
    int id = screenId(a[1]);
    if (id < 0 || id >= kEwsimScreenCount) return "unknown screen '" + a[1] + "'; known: " + screenList();
    return "";
  }
  if (c == "snap") return a.size() <= 2 ? "" : "usage: snap NAME";
  if (c == "keepcon") return a.size() == 2 ? "" : "usage: keepcon on|off";
  if (c == "raw") return a.size() >= 2 ? "" : "usage: raw LINE";
  if (c == "expect") {
    if (a.size() < 2) return "usage: expect screen|log|nolog|noerr|fps ...";
    if (a[1] == "screen") return a.size() == 3 && screenId(a[2]) >= 0 ? "" : "expect screen: unknown screen; known: " + screenList();
    if (a[1] == "log" || a[1] == "nolog") {
      try { std::regex r(a.size() > 2 ? a[2] : ""); } catch (...) { return "bad regex"; }
      return a.size() >= 3 ? "" : "usage: expect log RE [MS]";
    }
    if (a[1] == "noerr") return "";
    if (a[1] == "fps") return a.size() == 3 ? "" : "usage: expect fps MIN";
    if (a[1] == "snap") return "expect snap (golden images) is not supported by ewsim_host; use ewt on the watch";
    return "unknown expect '" + a[1] + "'";
  }
  if (c == "golden") return "golden is not supported by ewsim_host; use ewt on the watch";
  return "unknown step '" + c + "'";
}

static std::vector<std::string> sNotes;
static int sFails = 0;

static bool runStep(const std::string &step) {
  auto a = words(step);
  const std::string &c = a[0];
  std::string reply;
  auto fail = [&](const std::string &why) {
    fprintf(stderr, "FAIL step '%s': %s (t=%.2fs)\n", step.c_str(), why.c_str(), sim_now_us() / 1e6);
    auto ls = linesSince(sMark);
    size_t from = ls.size() > 12 ? ls.size() - 12 : 0;
    if (from < ls.size()) fprintf(stderr, "log tail:\n");
    for (size_t i = from; i < ls.size(); i++) fprintf(stderr, "  %s\n", ls[i].substr(0, 200).c_str());
    sFails++;
    return false;
  };
  static const std::set<std::string> input = { "tap", "dtap", "long", "drag", "swipe", "gest", "touch", "btn", "shake", "wake", "stroke" };
  if (input.count(c)) {
    sMark = host_serial_tx_size();
    if (c == "btn" && a[1] == "vlong") sNotes.push_back("btn vlong powers the watch off");
    if (!sim(step, reply)) return fail(reply);
    if (reply.rfind("OK", 0) != 0) return fail(reply);
  } else if (c == "tilt") {
    sMark = host_serial_tx_size();
    std::string cmd = "tilt off";
    if (a[1] != "off") {
      char b[96];
      snprintf(b, sizeof b, "tilt %d %d %d", (int)lround(atof(a[1].c_str()) * 4096), (int)lround(atof(a[2].c_str()) * 4096),
               (int)lround(atof(a[3].c_str()) * 4096));
      cmd = b;
    }
    if (!sim(cmd, reply) || reply.rfind("OK", 0) != 0) return fail(reply);
  } else if (c == "goto") {
    sMark = host_serial_tx_size();
    int id = screenId(a[1]);
    // registry.cpp's viewFor() falls back to the watch face for screens that
    // have no view in this build: ewsim's goto then "succeeds" on the watch
    // too, showing the wrong app. Catch it here instead of filming it.
    if (id != 0 && viewFor((Screen)id) == viewFor((Screen)0))
      return fail("screen " + std::string(kEwsimScreens[id]) + " has no view in this build (viewFor() falls back to the watch face)");
    if (!sim("goto " + std::to_string(id), reply) || reply.rfind("OK", 0) != 0) return fail(reply);
    if (!sim("state", reply)) return fail(reply);
    size_t p = reply.find("scr=");
    int got = p == std::string::npos ? -1 : atoi(reply.c_str() + p + 4);
    if (got != id) return fail("goto " + a[1] + " did not take: watch is on " + (got >= 0 && got < kEwsimScreenCount ? kEwsimScreens[got] : std::to_string(got)));
  } else if (c == "wait") {
    vTaskDelay((TickType_t)atoi(a[1].c_str()));
  } else if (c == "state") {
    if (!sim("state", reply)) return fail(reply);
    sNotes.push_back("state: " + reply);
  } else if (c == "snap") {
    static uint16_t fb[W * H];
    ewsimPanelFrame(fb, false);
    char nm[64];
    snprintf(nm, sizeof nm, "snap-%.3f", sim_now_us() / 1e6);
    std::string path = O.snapDir + "/" + (a.size() > 1 ? a[1] : std::string(nm)) + ".png";
    if (!pngWrite(path, fb)) return fail("cannot write " + path);
    uint16_t col;
    if (uniform(fb, &col)) {
      char b[160];
      snprintf(b, sizeof b, "WARNING snap %s is one flat colour (0x%04X): nothing drawn?", path.c_str(), col);
      sNotes.push_back(b);
    }
    if (host_backlight() == 0 || !gEwsimPanel.dispOn) sNotes.push_back("WARNING snap " + path + " taken with the screen off (backlight 0 / DISPOFF)");
    sNotes.push_back("snap: " + path);
  } else if (c == "xcheck") {
    // Cross-check: ewsim's own bus-tap shadow (what `ewt snap` returns on the
    // watch) against this panel model, via the real console protocol.
    size_t off = host_serial_tx_size();
    if (!sim("snap", reply)) return fail(reply);
    std::string tx = txFrom(off);
    size_t p = tx.find(" SNAP ");
    if (p == std::string::npos) return fail("no SNAP header: " + reply);
    p = tx.find('\n', p) + 1;
    std::vector<uint8_t> img;
    bool bad = false;
    while (img.size() < (size_t)W * H * 2 && p + 4 <= tx.size()) {
      if ((uint8_t)tx[p] != 0xA5 || (uint8_t)tx[p + 1] != 0x5A) { bad = true; break; }
      size_t n = (uint8_t)tx[p + 2] | ((uint8_t)tx[p + 3] << 8);
      uint16_t sum = 0;
      for (size_t i = 0; i < n; i++) sum += (uint8_t)tx[p + 4 + i];
      uint16_t want = (uint8_t)tx[p + 4 + n] | ((uint8_t)tx[p + 5 + n] << 8);
      if (sum != want) bad = true;
      img.insert(img.end(), tx.begin() + p + 4, tx.begin() + p + 4 + n);
      p += 6 + n;
    }
    if (bad || img.size() != (size_t)W * H * 2) return fail("snap stream corrupt");
    static uint16_t fb[W * H];
    ewsimPanelFrame(fb, false);
    int diff = 0;
    for (int i = 0; i < W * H; i++) if ((uint16_t)(img[2 * i] | (img[2 * i + 1] << 8)) != fb[i]) diff++;
    char b[128];
    snprintf(b, sizeof b, "xcheck: ewsim bus-tap shadow vs panel model: %d of %d px differ (%.2f%%)", diff, W * H, 100.0 * diff / (W * H));
    sNotes.push_back(b);
  } else if (c == "keepcon") {
    host_serial_inject(a[1] == "on" || a[1] == "1" ? "keepcon 1\n" : "keepcon 0\n");
    vTaskDelay(300);
  } else if (c == "raw") {
    sMark = host_serial_tx_size();
    host_serial_inject((step.substr(4) + "\n").c_str());
    vTaskDelay(500);
  } else if (c == "show") {
    std::regex rx(a.size() > 1 ? step.substr(5) : ".*");
    for (auto &l : linesSince(sMark)) if (std::regex_search(l, rx)) sNotes.push_back("log: " + l.substr(0, 200));
  } else if (c == "expect") {
    if (a[1] == "screen") {
      if (!sim("state", reply)) return fail(reply);
      size_t p = reply.find("scr=");
      int got = p == std::string::npos ? -1 : atoi(reply.c_str() + p + 4);
      int want = screenId(a[2]);
      if (got != want)
        return fail("expected screen " + a[2] + ", got " + (got >= 0 && got < kEwsimScreenCount ? kEwsimScreens[got] : std::to_string(got)));
    } else if (a[1] == "log" || a[1] == "nolog") {
      std::regex rx(a[2]);
      if (a[1] == "log") {
        int ms = a.size() > 3 ? atoi(a[3].c_str()) : 1500;
        uint64_t dl = sim_now_us() + (uint64_t)ms * 1000;
        for (;;) {
          bool hit = false;
          for (auto &l : linesSince(sMark)) if (std::regex_search(l, rx)) hit = true;
          if (hit) break;
          if (sim_now_us() > dl) return fail("no log line matching /" + a[2] + "/ within " + std::to_string(ms) + " ms");
          vTaskDelay(50);
        }
      } else {
        vTaskDelay(200);
        for (auto &l : linesSince(sMark)) if (std::regex_search(l, rx)) return fail("unexpected log line: " + l);
      }
    } else if (a[1] == "noerr") {
      vTaskDelay(200);
      for (auto &l : linesSince(sMark)) if (std::regex_search(l, kErrRe)) return fail("error line: " + l);
    } else if (a[1] == "fps") {
      uint64_t p0 = gEwsimPanel.pixels;
      vTaskDelay(1000);
      double fps = (gEwsimPanel.pixels - p0) / (double)(W * H);
      char b[64]; snprintf(b, sizeof b, "fps %.1f", fps);
      if (fps < atof(a[2].c_str())) return fail(std::string(b) + " < " + a[2] + " (full-frame equivalents)");
      sNotes.push_back(b);
    }
  }
  return true;
}

static std::vector<std::string> sSteps;
static int sStepFailedAt = 0;

static void runnerTask(void *) {
  vTaskDelay((TickType_t)O.boot);
  std::string reply;
  if (!sim("state", reply, 5000)) {
    fprintf(stderr, "ewsim: the watch console never answered `sim state` after %d ms of boot: %s\n", O.boot, reply.c_str());
    sFails++; sStepFailedAt = -1;
    sim_stop();
  }
  host_serial_inject("keepcon 1\n");     // console stays up in apps that own Serial (as `ewt keepcon on`)
  vTaskDelay(100);
  if (!O.out.empty()) {
    R.t0us = sim_now_us();
    R.on = true;
  }
  vTaskDelay((TickType_t)O.lead);
  for (size_t i = 0; i < sSteps.size(); i++) {
    if (O.verbose) fprintf(stderr, "> %s  (t=%.2fs)\n", sSteps[i].c_str(), sim_now_us() / 1e6);
    if (!runStep(sSteps[i])) { sStepFailedAt = (int)i + 1; break; }
  }
  if (!sStepFailedAt) vTaskDelay((TickType_t)O.hold);
  if (R.on) {
    R.stop = true;
    while (!R.stopped) vTaskDelay(1);
  }
  sim_stop();
}

// ---------------------------------------------------------------- firmware entry
static void loopTask(void *) {
  setup();
  for (;;) loop();
}

static void onSignal(int sig) {
  fprintf(stderr, "\newsim: CRASH (signal %d) in task '%s' at t=%.3fs\n", sig, sim_current_task_name(), sim_now_us() / 1e6);
  const char *tx = host_serial_tx_data();
  size_t n = host_serial_tx_size();
  size_t from = n > 1500 ? n - 1500 : 0;
  fprintf(stderr, "console tail:\n%.*s\n", (int)(n - from), tx + from);
  _exit(71);
}

static void usage() {
  fprintf(stderr,
    "usage: ewsim [options] \"STEPS\"            (normally run via ./ewsimrec)\n"
    "  --script-file F     steps from a .ewt file (one per line, # comments)\n"
    "  --out OUT.mp4       record an MP4 (+ OUT.sheet.png contact sheet)\n"
    "  --snapdir DIR       where `snap NAME` PNGs go (default .)\n"
    "  --fps N --scale N --smooth --short --lead MS --hold MS   (as ewt record)\n"
    "  --boot MS           simulated boot time before the script (default 1500)\n"
    "  --time HH:MM[:SS] --date YYYY-MM-DD   RTC start (default 2026-10-07 10:09:30)\n"
    "  --pref NS.KEY=VAL   seed an NVS preference (e.g. ewatch.wfStyle=2)\n"
    "  --sleep             keep the watch's dim/screensaver/screen-off timeouts\n"
    "  --fs DIR            copy DIR into the simulated LittleFS before boot\n"
    "  --seed N            PRNG seed (esp_random/random)\n"
    "  --allow-unsupported don't fail when a not-simulated app (BLE) is opened\n"
    "  --screens           list screen names and exit\n"
    "  -v                  echo the watch console to stderr\n");
}

int main(int argc, char **argv) {
  std::string scriptFile;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
    if (a == "--script-file") scriptFile = next();
    else if (a == "--out") O.out = next();
    else if (a == "--snapdir") O.snapDir = next();
    else if (a == "--fps") O.fps = atoi(next().c_str());
    else if (a == "--scale") O.scale = atoi(next().c_str());
    else if (a == "--lead") O.lead = atoi(next().c_str());
    else if (a == "--hold") O.hold = atoi(next().c_str());
    else if (a == "--boot") O.boot = atoi(next().c_str());
    else if (a == "--smooth") O.smooth = true;
    else if (a == "--short") O.shortFmt = true;
    else if (a == "--sleep") O.sleep = true;
    else if (a == "--allow-unsupported") O.allowUnsupported = true;
    else if (a == "--fs") O.fsSeed = next();
    else if (a == "--fsroot") O.fsRoot = next();
    else if (a == "--seed") O.seed = strtoull(next().c_str(), nullptr, 0);
    else if (a == "--pref") O.prefs.push_back(next());
    else if (a == "--time") { std::string t = next(); O.s = 0; if (sscanf(t.c_str(), "%d:%d:%d", &O.h, &O.mi, &O.s) < 2) { usage(); return 2; } }
    else if (a == "--date") { if (sscanf(next().c_str(), "%d-%d-%d", &O.Y, &O.Mo, &O.D) != 3) { usage(); return 2; } }
    else if (a == "--screens") O.listScreens = true;
    else if (a == "-v" || a == "--verbose") O.verbose = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { fprintf(stderr, "ewsim: unknown option %s\n", a.c_str()); usage(); return 2; }
    else O.script += (O.script.empty() ? "" : ";") + a;
  }
  if (O.listScreens) {
    for (int i = 0; i < kEwsimScreenCount; i++) printf("%d=%s%s", i, kEwsimScreens[i], i + 1 < kEwsimScreenCount ? " " : "\n");
    return 0;
  }
  if (!scriptFile.empty()) {
    FILE *f = fopen(scriptFile.c_str(), "r");
    if (!f) { fprintf(stderr, "ewsim: cannot read %s\n", scriptFile.c_str()); return 2; }
    std::string t; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
    fclose(f);
    O.script = t + "\n" + O.script;
  }
  if (O.fps < 1 || O.fps > 120 || O.scale < 1 || O.scale > 8) { fprintf(stderr, "ewsim: bad --fps/--scale\n"); return 2; }
  sSteps = splitSteps(O.script);
  for (auto &s : sSteps) {
    std::string e = validate(s);
    if (!e.empty()) { fprintf(stderr, "ewsim: bad step '%s': %s\n", s.c_str(), e.c_str()); return 2; }
  }

  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGSEGV, onSignal);
  signal(SIGBUS, onSignal);
  signal(SIGABRT, onSignal);
  signal(SIGFPE, onSignal);
  signal(SIGILL, onSignal);
  signal(SIGPIPE, SIG_IGN);

  // simulated hardware state
  host_seed_random(O.seed);
  host_set_rtc(O.Y, O.Mo, O.D, O.h, O.mi, O.s);
  host_serial_set_echo(O.verbose);
  if (system(("mkdir -p '" + O.snapDir + "'").c_str()) != 0) { fprintf(stderr, "ewsim: cannot create %s\n", O.snapDir.c_str()); return 2; }
  if (O.fsRoot.empty()) {
    char tmpl[] = "/tmp/ewsim-fs-XXXXXX";
    O.fsRoot = mkdtemp(tmpl) ? tmpl : "/tmp/ewsim-fs";
  }
  mkdir(O.fsRoot.c_str(), 0755);
  if (!O.fsSeed.empty()) {
    std::string cmd = "cp -R '" + O.fsSeed + "'/. '" + O.fsRoot + "'/";
    if (system(cmd.c_str()) != 0) { fprintf(stderr, "ewsim: --fs copy failed\n"); return 2; }
  }
  ewsimFsSetRoot(O.fsRoot.c_str());
  if (!O.sleep) {                        // video default: never dim, no screensaver, no screen-off
    ewsimPrefSeed("ewatch", "pDim", "0");
    ewsimPrefSeed("ewatch", "sleepSec", "0");
    ewsimPrefSeed("ewatch", "saverSec", "0");
  }
  for (auto &p : O.prefs) {
    size_t dot = p.find('.'), eq = p.find('=');
    if (dot == std::string::npos || eq == std::string::npos || eq < dot) { fprintf(stderr, "ewsim: --pref wants NS.KEY=VALUE\n"); return 2; }
    ewsimPrefSeed(p.substr(0, dot).c_str(), p.substr(dot + 1, eq - dot - 1).c_str(), p.substr(eq + 1).c_str());
  }

  // ffmpeg (same filter chain and encoder settings as `ewt record`)
  if (!O.out.empty()) {
    int r = 36;
    for (int y = 0; y < r; y++)
      for (int x = 0; x < r; x++)
        if ((r - x - 0.5) * (r - x - 0.5) + (r - y - 0.5) * (r - y - 0.5) > r * r) {
          R.cut.push_back(y * W + x); R.cut.push_back(y * W + (W - 1 - x));
          R.cut.push_back((H - 1 - y) * W + x); R.cut.push_back((H - 1 - y) * W + (W - 1 - x));
        }
    std::string flt = "scale=iw*" + std::to_string(O.scale) + ":ih*" + std::to_string(O.scale) + ":flags=" + (O.smooth ? "lanczos" : "neighbor");
    if (O.shortFmt && W * O.scale <= 1080) flt += ",pad=1080:1920:(ow-iw)/2:(oh-ih)/2:black";
    std::string cmd = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb565le -s 240x280 -r " + std::to_string(O.fps) +
                      " -i - -vf '" + flt + "' -c:v libx264 -crf 16 -pix_fmt yuv420p -movflags +faststart '" + O.out + "'";
    R.ff = popen(cmd.c_str(), "w");
    if (!R.ff) { fprintf(stderr, "ewsim: cannot start ffmpeg\n"); return 2; }
  }

  auto wall0 = std::chrono::steady_clock::now();
  sim_create_task(loopTask, "loopTask", nullptr, 1, 1);
  sim_create_task(runnerTask, "ewsim-host", nullptr, 20, 0);
  sim_create_task(captureTask, "capture", nullptr, 24, 0);
  int rc = sim_run();
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
  double simS = sim_now_us() / 1e6;

  int exitCode = 0;
  if (rc == 2) exitCode = 5;
  if (sFails) exitCode = 1;
  std::string sheet;
  if (R.ff) {
    int er = pclose(R.ff);
    if (er != 0) { fprintf(stderr, "ewsim: ffmpeg failed (%d)\n", er); exitCode = exitCode ? exitCode : 6; }
    double secs = R.frames / (double)O.fps;
    sheet = O.out.substr(0, O.out.rfind('.')) + ".sheet.png";
    if (R.frames) {
      char c[1024];
      // keep the aspect: a --short (1080x1920) clip gets 216x384 tiles, not squashed 240x280 ones
      snprintf(c, sizeof c, "ffmpeg -loglevel error -y -i '%s' -vf 'fps=%.4f,scale=%s,tile=6x1' -frames:v 1 '%s'",
               O.out.c_str(), 6 / std::max(secs, 0.1), (O.shortFmt && W * O.scale <= 1080) ? "216:384" : "240:280", sheet.c_str());
      if (system(c) != 0) fprintf(stderr, "ewsim: contact sheet failed\n");
    }
    printf("recorded %s %.1fs @%dfps: %llu frames, %llu distinct, %llu with content; sim %.1fs in %.1fs wall (%.1fx); sheet %s\n",
           O.out.c_str(), secs, O.fps, (unsigned long long)R.frames, (unsigned long long)R.distinct,
           (unsigned long long)R.nonUniform, simS, wall, simS / std::max(wall, 1e-3), sheet.c_str());
    if (R.frames == 0 || R.nonUniform == 0) {
      fprintf(stderr, "FAIL empty footage: %severy frame is one flat colour (nothing drawn / screen off)\n",
              R.frames ? "" : "no frames;");
      if (!exitCode) exitCode = 4;
    } else if (R.nonUniform * 2 < R.frames) {
      fprintf(stderr, "WARNING %.1fs of %.1fs is one flat colour (screen off/dark or a blank app?): check the sheet\n",
              (R.frames - R.nonUniform) / (double)O.fps, secs);
    } else if (R.distinct <= 1 && R.frames > 1) {
      fprintf(stderr, "WARNING footage never changes (one distinct frame over %.1fs)\n", secs);
    }
  } else {
    printf("ran %zu steps; sim %.1fs in %.1fs wall\n", sSteps.size(), simS, wall);
  }
  for (auto &n : sNotes) printf("  %s\n", n.c_str());
  if (*host_notes()) printf("  note: %s\n", host_notes());
  if (host_unsupported_count()) {
    fprintf(stderr, "%s not simulated on the host: %s%s\n", O.allowUnsupported ? "WARNING" : "FAIL",
            host_unsupported_list(), O.allowUnsupported ? "" : " (footage shows a NOT SIMULATED card; use ewt record on the watch)");
    if (!O.allowUnsupported && !exitCode) exitCode = 3;
  }
  if (sStepFailedAt > 0) fprintf(stderr, "FAIL at step %d of %zu\n", sStepFailedAt, sSteps.size());
  printf("%s\n", exitCode ? "RESULT: FAIL" : "RESULT: PASS");
  fflush(stdout); fflush(stderr);
  std::string rm = "rm -rf '" + O.fsRoot + "'";
  if (O.fsRoot.rfind("/tmp/ewsim-fs-", 0) == 0) (void)!system(rm.c_str());
  _exit(exitCode);
}
