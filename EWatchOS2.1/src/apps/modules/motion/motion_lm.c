// motion_lm.c — see motion_lm.h. Forward pass is llama2.c run.c's forward()
// (MIT, Andrej Karpathy) with per-context KV caches and a RoPE table.
#include "motion_lm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef MOTION_HOST
static void *ps_calloc_safe(size_t n, size_t sz) { return calloc(n, sz); }
#else
#include "esp_heap_caps.h"
static void *ps_calloc_safe(size_t n, size_t sz) {
  void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);   // PSRAM first
  if (!p) p = calloc(n, sz);
  return p;
}
#endif

typedef struct { int dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len; } Config;

typedef struct {
  const float *tok_emb, *rms_att, *wq, *wk, *wv, *wo, *rms_ffn, *w1, *w2, *w3, *rms_final, *wcls;
} Weights;

static Config  C;
static Weights Wt;
static int     sReady = 0;
static long    sParams = 0;
static uint32_t sStateBytes = 0;
// shared scratch
static float *x, *xb, *xb2, *hb, *hb2, *q, *att, *logits;
static float *ropeCos, *ropeSin;              // [seq_len][head_size/2]
// per context
static float *kc[MLM_NCTX], *vc[MLM_NCTX];
static int    pos[MLM_NCTX];

static float *alloc_f(size_t n) {
  sStateBytes += (uint32_t)(n * sizeof(float));
  return (float *)ps_calloc_safe(n, sizeof(float));
}

int mlm_init(const char **err) {
  if (sReady) return 1;
  memcpy(&C, motion_model_words, sizeof(Config));
  if (C.dim <= 0 || C.n_layers <= 0 || C.n_heads <= 0 || C.n_kv_heads != C.n_heads) {
    if (err) *err = "bad model header";
    return 0;
  }
  int shared = C.vocab_size > 0;
  C.vocab_size = abs(C.vocab_size);
  const float *p = (const float *)(const void *)(motion_model_words + 7);
  const int L = C.n_layers, D = C.dim, Hd = C.hidden_dim, hs = C.dim / C.n_heads;
  const float *start = p;
  Wt.tok_emb = p;   p += (size_t)C.vocab_size * D;
  Wt.rms_att = p;   p += (size_t)L * D;
  Wt.wq = p;        p += (size_t)L * D * D;
  Wt.wk = p;        p += (size_t)L * D * D;
  Wt.wv = p;        p += (size_t)L * D * D;
  Wt.wo = p;        p += (size_t)L * D * D;
  Wt.rms_ffn = p;   p += (size_t)L * D;
  Wt.w1 = p;        p += (size_t)L * D * Hd;
  Wt.w2 = p;        p += (size_t)L * Hd * D;
  Wt.w3 = p;        p += (size_t)L * D * Hd;
  Wt.rms_final = p; p += D;
  sParams = (long)(p - start);
  p += (size_t)C.seq_len * hs / 2 * 2;       // legacy freq_cis_real/imag (unused)
  Wt.wcls = shared ? Wt.tok_emb : p;
  if (!shared) sParams += (long)C.vocab_size * D;
  if ((uint32_t)((p - start) + 7 + (shared ? 0 : C.vocab_size * D)) > motion_model_nwords) {
    if (err) *err = "model blob truncated";
    return 0;
  }

  sStateBytes = 0;
  x = alloc_f(D); xb = alloc_f(D); xb2 = alloc_f(D); q = alloc_f(D);
  hb = alloc_f(Hd); hb2 = alloc_f(Hd);
  att = alloc_f((size_t)C.n_heads * C.seq_len);
  logits = alloc_f(C.vocab_size);
  ropeCos = alloc_f((size_t)C.seq_len * hs / 2);
  ropeSin = alloc_f((size_t)C.seq_len * hs / 2);
  int ok = x && xb && xb2 && q && hb && hb2 && att && logits && ropeCos && ropeSin;
  for (int c = 0; c < MLM_NCTX; c++) {
    kc[c] = alloc_f((size_t)L * C.seq_len * D);
    vc[c] = alloc_f((size_t)L * C.seq_len * D);
    ok = ok && kc[c] && vc[c];
    pos[c] = 0;
  }
  if (!ok) { if (err) *err = "state alloc failed"; return 0; }
  for (int t = 0; t < C.seq_len; t++)
    for (int j = 0; j < hs / 2; j++) {
      float freq = 1.0f / powf(10000.0f, (2.0f * j) / (float)hs);
      float a = t * freq;
      ropeCos[t * (hs / 2) + j] = cosf(a);
      ropeSin[t * (hs / 2) + j] = sinf(a);
    }
  sReady = 1;
  return 1;
}

static void rmsnorm(float *o, const float *xx, const float *w, int n) {
  float ss = 0.0f;
  for (int j = 0; j < n; j++) ss += xx[j] * xx[j];
  ss /= n; ss += 1e-5f; ss = 1.0f / sqrtf(ss);
  for (int j = 0; j < n; j++) o[j] = w[j] * (ss * xx[j]);
}

static void softmax(float *v, int n) {
  float m = v[0];
  for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
  float s = 0.0f;
  for (int i = 0; i < n; i++) { v[i] = expf(v[i] - m); s += v[i]; }
  for (int i = 0; i < n; i++) v[i] /= s;
}

static void matmul(float *o, const float *xx, const float *w, int n, int d) {
  for (int i = 0; i < d; i++) {
    const float *row = w + (size_t)i * n;
    float v = 0.0f;
    for (int j = 0; j < n; j++) v += row[j] * xx[j];
    o[i] = v;
  }
}

void mlm_reset(int ctx) { if (ctx >= 0 && ctx < MLM_NCTX) pos[ctx] = 0; }
int  mlm_pos(int ctx)   { return (ctx >= 0 && ctx < MLM_NCTX) ? pos[ctx] : 0; }

const float *mlm_feed(int ctx, int token) {
  if (!sReady || ctx < 0 || ctx >= MLM_NCTX) return NULL;
  if (token < 0 || token >= C.vocab_size) return NULL;
  int p = pos[ctx];
  if (p >= C.seq_len) return NULL;
  const int D = C.dim, Hd = C.hidden_dim, H = C.n_heads, hs = D / H;
  const float scale = 1.0f / sqrtf((float)hs);
  memcpy(x, Wt.tok_emb + (size_t)token * D, D * sizeof(float));

  for (int l = 0; l < C.n_layers; l++) {
    rmsnorm(xb, x, Wt.rms_att + (size_t)l * D, D);
    size_t loff = (size_t)l * C.seq_len * D;
    float *k = kc[ctx] + loff + (size_t)p * D;
    float *v = vc[ctx] + loff + (size_t)p * D;
    matmul(q, xb, Wt.wq + (size_t)l * D * D, D, D);
    matmul(k, xb, Wt.wk + (size_t)l * D * D, D, D);
    matmul(v, xb, Wt.wv + (size_t)l * D * D, D, D);
    const float *rc = ropeCos + p * (hs / 2), *rs = ropeSin + p * (hs / 2);
    for (int i = 0; i < D; i += 2) {
      int j = (i % hs) / 2;
      float fcr = rc[j], fci = rs[j];
      float q0 = q[i], q1 = q[i + 1];
      q[i] = q0 * fcr - q1 * fci; q[i + 1] = q0 * fci + q1 * fcr;
      float k0 = k[i], k1 = k[i + 1];
      k[i] = k0 * fcr - k1 * fci; k[i + 1] = k0 * fci + k1 * fcr;
    }
    for (int h = 0; h < H; h++) {
      const float *qh = q + h * hs;
      float *a = att + h * C.seq_len;
      for (int t = 0; t <= p; t++) {
        const float *kt = kc[ctx] + loff + (size_t)t * D + h * hs;
        float s = 0.0f;
        for (int i = 0; i < hs; i++) s += qh[i] * kt[i];
        a[t] = s * scale;
      }
      softmax(a, p + 1);
      float *o = xb + h * hs;
      memset(o, 0, hs * sizeof(float));
      for (int t = 0; t <= p; t++) {
        const float *vt = vc[ctx] + loff + (size_t)t * D + h * hs;
        float w = a[t];
        for (int i = 0; i < hs; i++) o[i] += w * vt[i];
      }
    }
    matmul(xb2, xb, Wt.wo + (size_t)l * D * D, D, D);
    for (int i = 0; i < D; i++) x[i] += xb2[i];
    rmsnorm(xb, x, Wt.rms_ffn + (size_t)l * D, D);
    matmul(hb, xb, Wt.w1 + (size_t)l * D * Hd, D, Hd);
    matmul(hb2, xb, Wt.w3 + (size_t)l * D * Hd, D, Hd);
    for (int i = 0; i < Hd; i++) {
      float val = hb[i];
      val *= 1.0f / (1.0f + expf(-val));
      hb[i] = val * hb2[i];
    }
    matmul(xb, hb, Wt.w2 + (size_t)l * D * Hd, Hd, D);
    for (int i = 0; i < D; i++) x[i] += xb[i];
  }
  rmsnorm(x, x, Wt.rms_final, D);
  matmul(logits, x, Wt.wcls, D, C.vocab_size);
  pos[ctx] = p + 1;
  return logits;
}

int  mlm_vocab(void)   { return C.vocab_size; }
int  mlm_dim(void)     { return C.dim; }
int  mlm_layers(void)  { return C.n_layers; }
int  mlm_hidden(void)  { return C.hidden_dim; }
int  mlm_heads(void)   { return C.n_heads; }
int  mlm_seq_len(void) { return C.seq_len; }
long mlm_params(void)  { return sParams; }
uint32_t mlm_state_bytes(void) { return sStateBytes; }
