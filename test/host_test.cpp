// Host harness for TankFace: the real tank_draw.cpp + dial_data.h, no
// hardware. Writes PNGs for test/check_hands.py, test/check_dial.py and for
// eyeballing. See test/render.sh.
//   host_test <outdir> [--break-hour | --no-hands]   (checker controls)
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "tank_draw.h"

static void be32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (i * 8))); }
static void chunk(FILE *f, const char *t, const std::vector<uint8_t> &d) {
  std::vector<uint8_t> b; be32(b, (uint32_t)d.size()); fwrite(b.data(), 1, 4, f);
  std::vector<uint8_t> c(t, t + 4); c.insert(c.end(), d.begin(), d.end()); fwrite(c.data(), 1, c.size(), f);
  b.clear(); be32(b, (uint32_t)crc32(0, c.data(), (uInt)c.size())); fwrite(b.data(), 1, 4, f);
}
static bool writePNG565(const std::string &path, const uint16_t *px, int w, int h) {
  FILE *f = fopen(path.c_str(), "wb"); if (!f) return false;
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
  std::vector<uint8_t> hd; be32(hd, w); be32(hd, h);
  for (uint8_t b : {8, 2, 0, 0, 0}) hd.push_back(b);
  chunk(f, "IHDR", hd);
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      uint16_t c = px[(size_t)y * w + x];
      raw.push_back((uint8_t)(((c >> 11) & 31) * 255 / 31));
      raw.push_back((uint8_t)(((c >> 5) & 63) * 255 / 63));
      raw.push_back((uint8_t)((c & 31) * 255 / 31));
    }
  }
  uLongf n = compressBound(raw.size()); std::vector<uint8_t> z(n);
  compress(z.data(), &n, raw.data(), raw.size()); z.resize(n);
  chunk(f, "IDAT", z); chunk(f, "IEND", {}); fclose(f);
  return true;
}

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : "out";
  bool noHands = argc > 2 && !strcmp(argv[2], "--no-hands");
  tank::testBreakHour = argc > 2 && !strcmp(argv[2], "--break-hour");
  std::vector<uint16_t> fb((size_t)tank::W * tank::H);
  int wrote = 0;

  // The bare dial (what an unset RTC shows, minus nothing: hub only).
  tank::draw(fb.data(), tank::Input{false, 0, 0, 0, 0, false});
  wrote += writePNG565(out + "/dial.png", fb.data(), tank::W, tank::H);

  // Times for the hand-angle checker: t_HHMMSS.png, no seconds hand. A
  // deterministic spread over 24 h; the checker skips times where the hands
  // overlap.
  for (int i = 0; i < 96; i++) {
    int t = (i * 2671 + 137) % 86400;                    // 2671 s steps, coprime-ish walk
    tank::Input in{!noHands, (uint8_t)(t / 3600), (uint8_t)(t / 60 % 60), (uint8_t)(t % 60), 0, false};
    tank::draw(fb.data(), in);
    char name[32]; snprintf(name, sizeof name, "/t_%02d%02d%02d.png", in.h, in.m, in.s);
    wrote += writePNG565(out + name, fb.data(), tank::W, tank::H);
  }

  // For eyeballing.
  struct { const char *name; tank::Input in; } shots[] = {
    {"/1_1010.png",         {true, 10, 10, 0, 0, false}},
    {"/2_0300.png",         {true, 3, 0, 0, 0, false}},
    {"/3_1350_seconds.png", {true, 13, 50, 24, 0.5f, true}},
    {"/4_0605_seconds.png", {true, 6, 5, 52, 0, true}},
  };
  for (auto &s : shots) { tank::draw(fb.data(), s.in); wrote += writePNG565(out + s.name, fb.data(), tank::W, tank::H); }

  printf("wrote %d PNGs to %s\n", wrote, out.c_str());
  return wrote == 1 + 96 + 4 ? 0 : 1;
}
