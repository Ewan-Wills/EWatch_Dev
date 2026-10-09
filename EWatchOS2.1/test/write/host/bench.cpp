// Host bench for the Write recogniser: the firmware's write_recog.cpp compiled
// for the Mac, fed synthetic draws (tools/write_glyphs.py bench FILE).
//   make -C test/host && test/host/bench /tmp/draws.txt
// Prints accuracy (top-1 / top-3), per-char accuracy, the confusions and the
// time per recognition (Mac; the watch is slower, see NOTES.md).
#include "write_recog.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iterator>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: bench DRAWS.txt [-v] [--tpl BLOB COUNT]\n"); return 2; }
  bool verbose = false;
  std::vector<uint8_t> blob;
  int blobCount = -1;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) verbose = true;
    else if (!strcmp(argv[i], "--tpl") && i + 2 < argc) {
      std::ifstream bf(argv[i + 1], std::ios::binary);
      blob.assign(std::istreambuf_iterator<char>(bf), {});
      blobCount = atoi(argv[i + 2]);
      i += 2;
    }
  }
  if (!strcmp(argv[1], "--selftest")) {
    // user templates: add -> serialise -> clear -> deserialise -> same answer
    if (!wrec::init()) return 1;
    wrec::RawPt p[] = {{0,0,0},{10,40,0},{20,80,0},{60,80,0},{0,0,1},{40,0,1}};
    wrec::Pt c[wrec::kN];
    if (!wrec::normalize(p, 6, c)) { printf("selftest: normalize failed\n"); return 1; }
    wrec::addUser('Q', c);
    uint8_t buf[8192];
    size_t n = wrec::userSerialize(buf, sizeof buf);
    wrec::clearUser();
    wrec::Cand k[3];
    wrec::classify(c, k, 3);
    char before = k[0].ch;
    bool ok = wrec::userDeserialize(buf, n);
    wrec::classify(c, k, 3);
    printf("selftest: bytes=%zu restored=%d users=%d without=%c with=%c(user=%d score=%.2f)\n",
           n, ok, wrec::userCount(), before, k[0].ch, k[0].user, k[0].score);
    return ok && k[0].ch == 'Q' && k[0].user && before != 'Q' ? 0 : 1;
  }
  bool ok = blobCount >= 0 ? wrec::initFrom(blob.data(), blobCount) : wrec::init();
  if (!ok) { fprintf(stderr, "init failed\n"); return 1; }
  std::ifstream f(argv[1]);
  std::string line;
  struct Sample { char ch; std::vector<wrec::RawPt> pts; };
  std::vector<Sample> samples;
  int stroke = 0;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') { samples.push_back({line[1], {}}); stroke = 0; continue; }
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) {
      int x, y;
      if (sscanf(tok.c_str(), "%d,%d", &x, &y) == 2)
        samples.back().pts.push_back({(int16_t)x, (int16_t)y, (uint8_t)stroke});
    }
    stroke++;
  }
  int ok1 = 0, ok3 = 0, n = 0, edits = 0;
  std::map<char, std::pair<int, int>> per;
  std::map<std::string, int> conf;
  double totalUs = 0, maxUs = 0;
  for (auto &s : samples) {
    n++;
    auto t0 = std::chrono::steady_clock::now();
    wrec::Edit e = wrec::editGesture(s.pts.data(), (int)s.pts.size());
    wrec::Cand c[3];
    int nc = 0;
    if (e == wrec::Edit::None) {
      wrec::Pt cloud[wrec::kN];
      if (wrec::normalize(s.pts.data(), (int)s.pts.size(), cloud)) nc = wrec::classify(cloud, c, 3);
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    totalUs += us; if (us > maxUs) maxUs = us;
    if (e != wrec::Edit::None) edits++;
    char got = nc ? c[0].ch : '?';
    bool hit3 = false;
    for (int i = 0; i < nc; i++) if (c[i].ch == s.ch) hit3 = true;
    per[s.ch].second++;
    if (got == s.ch) { ok1++; per[s.ch].first++; }
    else {
      conf[std::string(1, s.ch) + "->" + (e != wrec::Edit::None ? std::string(wrec::editName(e)) : std::string(1, got))]++;
      if (verbose) fprintf(stderr, "miss %c got %c (%.2f) %c %c\n", s.ch, got, nc ? c[0].score : 0, nc > 1 ? c[1].ch : ' ', nc > 2 ? c[2].ch : ' ');
    }
    if (hit3) ok3++;
  }
  printf("samples=%d top1=%.1f%% top3=%.1f%% edit-misfires=%d builtin=%d mean=%.0fus max=%.0fus\n",
         n, 100.0 * ok1 / n, 100.0 * ok3 / n, edits, wrec::builtinCount(), totalUs / n, maxUs);
  printf("per-char:");
  for (auto &kv : per) printf(" %c=%d/%d", kv.first, kv.second.first, kv.second.second);
  printf("\nconfusions:");
  std::vector<std::pair<int, std::string>> cv;
  for (auto &kv : conf) cv.push_back({kv.second, kv.first});
  std::sort(cv.rbegin(), cv.rend());
  for (auto &p : cv) printf(" %s:%d", p.second.c_str(), p.first);
  printf("\n");
  return 0;
}
