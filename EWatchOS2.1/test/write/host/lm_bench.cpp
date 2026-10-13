// Host bench for write_lm.cpp: each input line is "<L> logp[0][0..35] ... logp[L-1][..]";
// prints "<decoded> <corrected>" per line (compare with ml-write/lm.py).
#include "write_lm.h"
#include <cstdio>
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "r");
  int L;
  while (f && fscanf(f, "%d", &L) == 1) {
    float lp[wlm::kMaxWord][wlm::kClasses];
    for (int i = 0; i < L; i++) for (int c = 0; c < wlm::kClasses; c++) fscanf(f, "%f", &lp[i][c]);
    char w[wlm::kMaxWord + 1] = {}, out[wlm::kMaxWord + 1] = {};
    for (int i = 0; i < L; i++) {
      int c = wlm::decode(lp[i], w, i);
      w[i] = c < 26 ? 'A' + c : '0' + c - 26;
    }
    wlm::correct(w, L, lp, out);
    printf("%s %s\n", w, out);
  }
  return 0;
}
