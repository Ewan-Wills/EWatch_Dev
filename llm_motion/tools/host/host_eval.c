// host_eval — runs the FIRMWARE's C tokenizer + segmenter + LM runtime on the
// Mac (same source files as the watch, compiled with -DMOTION_HOST) so the
// Python pipeline can be cross-checked against the code that ships.
//
// stdin, one job per line:
//   t <n> x0 y0 z0 x1 y1 z1 ...      raw trace (counts) -> streamed exactly as
//                                    the engine does (incremental KV decode)
//   q <n> tok0 tok1 ...              one token sequence -> [BOS] toks [SEP]
//   b <n>                            benchmark: n forwards, prints us/forward
// stdout:
//   S <label> <p> <tok,tok,...>      one line per closed segment
//   E <tok,tok,...>                  after a trace: every token produced
//   B <us_per_forward>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "motion_tok.h"
#include "motion_lm.h"

static void classify(const float *lg, int *lab, float *p) {
  float m = -1e30f; int bi = 0;
  for (int i = 0; i < MT_NLABELS; i++) if (lg[MT_LABEL0 + i] > m) { m = lg[MT_LABEL0 + i]; bi = i; }
  float s = 0;
  for (int i = 0; i < MT_NLABELS; i++) s += expf(lg[MT_LABEL0 + i] - m);
  *lab = bi; *p = 1.0f / s;
}

static double now_us(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void) {
  const char *err = NULL;
  if (!mlm_init(&err)) { fprintf(stderr, "init: %s\n", err); return 1; }
  fprintf(stderr, "model dim=%d layers=%d hidden=%d heads=%d vocab=%d seq=%d params=%ld state=%u B\n",
          mlm_dim(), mlm_layers(), mlm_hidden(), mlm_heads(), mlm_vocab(), mlm_seq_len(),
          mlm_params(), mlm_state_bytes());
  static char line[1 << 20];
  while (fgets(line, sizeof line, stdin)) {
    char *sp = line;
    char kind = *sp++;
    int n = (int)strtol(sp, &sp, 10);
    if (kind == 't') {
      MotionTokenizer tk; MotionSegmenter sg;
      mt_reset(&tk); mt_seg_reset(&sg);
      static int all[100000]; int na = 0;
      for (int i = 0; i < n; i++) {
        int x = (int)strtol(sp, &sp, 10), y = (int)strtol(sp, &sp, 10), z = (int)strtol(sp, &sp, 10);
        int t = mt_push(&tk, (int16_t)x, (int16_t)y, (int16_t)z);
        if (t < 0) continue;
        all[na++] = t;
        MotionSegEvent ev = mt_seg_push(&sg, t);
        if (ev == MS_START) {
          mlm_reset(0);
          mlm_feed(0, MT_BOS);
          for (int k = 0; k < sg.len; k++) mlm_feed(0, sg.seq[k]);
        } else if (ev == MS_TOK || ev == MS_END) {
          mlm_feed(0, t);
        }
        if (ev == MS_END) {
          const float *lg = mlm_feed(0, MT_SEP);
          int lab; float p; classify(lg, &lab, &p);
          printf("S %s %.6f ", ml_label_name(lab), p);
          for (int k = 0; k < sg.len; k++) printf(k ? ",%d" : "%d", sg.seq[k]);
          printf("\n");
        }
      }
      printf("E ");
      for (int k = 0; k < na; k++) printf(k ? ",%d" : "%d", all[k]);
      printf("\n");
    } else if (kind == 'q') {
      mlm_reset(1);
      mlm_feed(1, MT_BOS);
      for (int i = 0; i < n; i++) mlm_feed(1, (int)strtol(sp, &sp, 10));
      const float *lg = mlm_feed(1, MT_SEP);
      int lab; float p; classify(lg, &lab, &p);
      printf("S %s %.6f\n", ml_label_name(lab), p);
    } else if (kind == 'b') {
      double t0 = now_us();
      int done = 0;
      while (done < n) {
        mlm_reset(0);
        for (int k = 0; k < 24 && done < n; k++, done++) mlm_feed(0, MT_MOVE0 + (k * 7) % 72);
      }
      printf("B %.3f\n", (now_us() - t0) / n);
    }
    fflush(stdout);
  }
  return 0;
}
