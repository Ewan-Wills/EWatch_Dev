// Host bench for the Write CNN (write_cnn.cpp, the firmware source).
//   make -C test/write/host cnn_bench
//   cnn_bench SAMPLES.txt [--img OUT.f32] [-p]   (-p: print all class probabilities)
// SAMPLES: one glyph per line: "<label> <n> x y stroke x y stroke ..." (screen px).
// Prints "<label> <top1> <p1> <top2> <top3>" per line, then accuracy and time.
// --img writes every rasterised 28x28 image (float32) for comparison with
// ml-write/common.py.
#include "write_cnn.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: cnn_bench SAMPLES.txt [--img OUT.f32]\n"); return 2; }
  FILE *img = nullptr;
  bool probs = false;
  for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-p")) probs = true;
  for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--img")) img = fopen(argv[i + 1], "wb");
  if (!wcnn::init()) { printf("init failed\n"); return 1; }
  std::ifstream f(argv[1]);
  std::string line;
  int n = 0, ok1 = 0, ok3 = 0;
  double us = 0;
  std::vector<wrec::RawPt> pts;
  float buf[wcnn::kG * wcnn::kG];
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string lab; int np;
    if (!(ss >> lab >> np)) continue;
    pts.resize(np);
    for (int i = 0; i < np; i++) { int x, y, s; ss >> x >> y >> s; pts[i] = { (int16_t)x, (int16_t)y, (uint8_t)s }; }
    auto t0 = std::chrono::steady_clock::now();
    wrec::Cand c[3];
    int k = wcnn::classify(pts.data(), np, c, 3);
    us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (img) { wcnn::rasterise(pts.data(), np, buf); fwrite(buf, sizeof buf, 1, img); }
    char L = lab[0];
    n++;
    if (k && c[0].ch == L) ok1++;
    for (int j = 0; j < k; j++) if (c[j].ch == L) { ok3++; break; }
    if (probs) {          // all class probabilities, in wcnn class order
      float pr[wcnn::kClasses];
      wcnn::rasterise(pts.data(), np, buf);
      wcnn::infer(buf, pr);
      printf("%c", L);
      for (int j = 0; j < wcnn::kClasses; j++) printf(" %.5g", pr[j]);
      printf("\n");
    } else
    printf("%c %c %.4f %c %c\n", L, k ? c[0].ch : '?', k ? c[0].score : 0.f, k > 1 ? c[1].ch : '?', k > 2 ? c[2].ch : '?');
  }
  if (img) fclose(img);
  fprintf(stderr, "n=%d top1=%.4f top3=%.4f us/glyph=%.0f\n", n, n ? (double)ok1 / n : 0, n ? (double)ok3 / n : 0, n ? us / n : 0);
  return n ? 0 : 1;
}
