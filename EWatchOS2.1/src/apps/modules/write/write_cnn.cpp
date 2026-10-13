// Write CNN: rasteriser + inference. See write_cnn.h.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")       // the firmware builds -Os; these loops are the hot path
#endif
#include "write_cnn.h"
#include "write_cnn_weights.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <chrono>
#if defined(ARDUINO)
  #include <esp_heap_caps.h>
#endif

namespace wcnn {

uint32_t gStageUs[6];
static uint32_t nowUs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {

using namespace wcnn_w;
constexpr float kFit = 20.0f;      // longest glyph side in pixels (common.py FIT)
constexpr float kRadius = 1.1f;    // brush radius (common.py RADIUS)

// Weights stay int8 in flash (104 KB) with a float scale per output channel.
// Activations + scratch (~40 KB) in internal RAM.
bool sReady = false;
float *a1, *a2, *a3, *h1, *dist;

void *alloc(size_t n, bool fast) {
#if defined(ARDUINO)
  void *p = heap_caps_malloc(n, fast ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                     : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (p) return p;
#else
  (void)fast;
#endif
  return malloc(n);
}

// 3x3 conv (pad 1) + bias + ReLU + 2x2 max-pool (floor). in: ci x H x W,
// out: co x H/2 x W/2. The input is copied once into a zero-padded buffer so
// the inner loop is branch-free: one weight in a register, a run of FMAs
// along a row (about 3x faster on the S3 than the bounds-checked version).
float *pad, *tmp;     // ci*(H+2)*(W+2) max 16*16*16, H*W max 28*28 (internal RAM)

void convPool(const float *in, int ci, int H, int W, const int8_t *w, const float *ws,
              const float *b, int co, float *out) {
  const int H2 = H / 2, W2 = W / 2, PW = W + 2, PS = (H + 2) * PW;
  memset(pad, 0, sizeof(float) * ci * PS);
  for (int c = 0; c < ci; c++)
    for (int y = 0; y < H; y++)
      memcpy(pad + c * PS + (y + 1) * PW + 1, in + (c * H + y) * W, sizeof(float) * W);
  for (int o = 0; o < co; o++) {
    for (int i = 0; i < H * W; i++) tmp[i] = b[o];
    const int8_t *wo = w + o * ci * 9;
    const float sc = ws[o];
    for (int c = 0; c < ci; c++) {
      const float *pc = pad + c * PS;
      for (int k = 0; k < 9; k++) {
        const float wk = wo[c * 9 + k] * sc;
        const float *src = pc + (k / 3) * PW + (k % 3);
        for (int y = 0; y < H; y++) {
          const float *s = src + y * PW;
          float *t = tmp + y * W;
          for (int x = 0; x < W; x++) t[x] += wk * s[x];
        }
      }
    }
    for (int py = 0; py < H2; py++)
      for (int px = 0; px < W2; px++) {
        const float *t = tmp + py * 2 * W + px * 2;
        float m = t[0];
        if (t[1] > m) m = t[1];
        if (t[W] > m) m = t[W];
        if (t[W + 1] > m) m = t[W + 1];
        out[(o * H2 + py) * W2 + px] = m > 0 ? m : 0;
      }
  }
}

}  // namespace

bool init() {
  if (sReady) return true;
  a1 = (float *)alloc(sizeof(float) * C1 * 14 * 14, true);
  a2 = (float *)alloc(sizeof(float) * C2 * 7 * 7, true);
  a3 = (float *)alloc(sizeof(float) * C3 * 3 * 3, true);
  h1 = (float *)alloc(sizeof(float) * FC, true);
  dist = (float *)alloc(sizeof(float) * kG * kG, true);
  pad = (float *)alloc(sizeof(float) * C1 * 16 * 16 > sizeof(float) * C2 * 9 * 9 ? sizeof(float) * C1 * 16 * 16 : sizeof(float) * C2 * 9 * 9, true);
  tmp = (float *)alloc(sizeof(float) * kG * kG, true);
  sReady = a1 && a2 && a3 && h1 && dist && pad && tmp;
  return sReady;
}

char classChar(int i) { return (i >= 0 && i < kClasses) ? kClassChars[i] : '?'; }

bool rasterise(const wrec::RawPt *pts, int n, float *img) {
  if (n < 1 || !dist) return false;
  int16_t mnx = 32767, mny = 32767, mxx = -32768, mxy = -32768;
  for (int i = 0; i < n; i++) {
    if (pts[i].x < mnx) mnx = pts[i].x; if (pts[i].x > mxx) mxx = pts[i].x;
    if (pts[i].y < mny) mny = pts[i].y; if (pts[i].y > mxy) mxy = pts[i].y;
  }
  float size = (float)((mxx - mnx) > (mxy - mny) ? (mxx - mnx) : (mxy - mny));
  if (size < 1e-6f) size = 1e-6f;
  const float s = kFit / size;
  const float cx = (mnx + mxx) * 0.5f, cy = (mny + mxy) * 0.5f;
  const float reach = kRadius + 0.5f;
  for (int i = 0; i < kG * kG; i++) dist[i] = 1e9f;
  // Each segment updates the pixels within `reach` of its box: the same
  // per-pixel minimum distance as common.py, only cheaper.
  auto seg = [&](float x0, float y0, float x1, float y1) {
    float bx0 = fminf(x0, x1) - reach, bx1 = fmaxf(x0, x1) + reach;
    float by0 = fminf(y0, y1) - reach, by1 = fmaxf(y0, y1) + reach;
    int ix0 = (int)floorf(bx0 - 0.5f), ix1 = (int)ceilf(bx1 - 0.5f);
    int iy0 = (int)floorf(by0 - 0.5f), iy1 = (int)ceilf(by1 - 0.5f);
    if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0;
    if (ix1 > kG - 1) ix1 = kG - 1; if (iy1 > kG - 1) iy1 = kG - 1;
    float abx = x1 - x0, aby = y1 - y0;
    float L2 = abx * abx + aby * aby;
    if (L2 < 1e-9f) L2 = 1e-9f;
    for (int iy = iy0; iy <= iy1; iy++) {
      float py = iy + 0.5f;
      for (int ix = ix0; ix <= ix1; ix++) {
        float px = ix + 0.5f;
        float t = ((px - x0) * abx + (py - y0) * aby) / L2;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float qx = x0 + t * abx - px, qy = y0 + t * aby - py;
        float d = sqrtf(qx * qx + qy * qy);
        float &m = dist[iy * kG + ix];
        if (d < m) m = d;
      }
    }
  };
  auto X = [&](int i) { return (pts[i].x - cx) * s + kG * 0.5f; };
  auto Y = [&](int i) { return (pts[i].y - cy) * s + kG * 0.5f; };
  for (int i = 0; i < n; i++) {
    bool first = i == 0 || pts[i].stroke != pts[i - 1].stroke;
    bool last = i == n - 1 || pts[i + 1].stroke != pts[i].stroke;
    if (first && last) seg(X(i), Y(i), X(i), Y(i));            // single-point stroke
    if (!first) seg(X(i - 1), Y(i - 1), X(i), Y(i));
  }
  for (int i = 0; i < kG * kG; i++) {
    float v = kRadius + 0.5f - dist[i];
    img[i] = v < 0 ? 0 : (v > 1 ? 1 : v);
  }
  return true;
}

bool infer(const float *img, float *prob) {
  if (!sReady) return false;
  uint32_t t0 = nowUs();
  convPool(img, 1, 28, 28, c1_w, c1_s, c1_b, C1, a1);
  uint32_t t1 = nowUs();
  convPool(a1, C1, 14, 14, c2_w, c2_s, c2_b, C2, a2);
  uint32_t t2 = nowUs();
  convPool(a2, C2, 7, 7, c3_w, c3_s, c3_b, C3, a3);
  uint32_t t3 = nowUs();
  gStageUs[1] = t1 - t0; gStageUs[2] = t2 - t1; gStageUs[3] = t3 - t2;
  const int nin = C3 * 9;
  for (int o = 0; o < FC; o++) {
    const int8_t *w = f1_w + o * nin;
    float acc = 0;
    for (int i = 0; i < nin; i++) acc += w[i] * a3[i];
    acc = acc * f1_s[o] + f1_b[o];
    h1[o] = acc > 0 ? acc : 0;
  }
  gStageUs[4] = nowUs() - t3;
  float mx = -1e30f;
  for (int o = 0; o < NC; o++) {
    const int8_t *w = f2_w + o * FC;
    float acc = 0;
    for (int i = 0; i < FC; i++) acc += w[i] * h1[i];
    acc = acc * f2_s[o] + f2_b[o];
    prob[o] = acc;
    if (acc > mx) mx = acc;
  }
  float sum = 0;
  for (int o = 0; o < NC; o++) { prob[o] = expf(prob[o] - mx); sum += prob[o]; }
  for (int o = 0; o < NC; o++) prob[o] /= sum;
  return true;
}

int classify(const wrec::RawPt *pts, int n, wrec::Cand *out, int maxOut) {
  static float img[kG * kG];
  float prob[kClasses];
  uint32_t t0 = nowUs();
  if (!init() || !rasterise(pts, n, img)) return 0;
  gStageUs[0] = nowUs() - t0;
  if (!infer(img, prob)) return 0;
  bool used[kClasses] = {};
  int k = 0;
  for (; k < maxOut && k < kClasses; k++) {
    int m = -1;
    for (int j = 0; j < kClasses; j++) if (!used[j] && (m < 0 || prob[j] > prob[m])) m = j;
    used[m] = true;
    out[k] = { classChar(m), 1.0f - prob[m], prob[m], false };
  }
  return k;
}

}  // namespace wcnn
