// motion_tok.c — see motion_tok.h. Must stay identical in behaviour to
// tools/motionlm/tokenizer.py (crosscheck.py compares them on random traces).
#include "motion_tok.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *kLabels[MT_NLABELS] = {
  "none", "flick_out", "flick_in", "twist_l", "twist_r", "shake",
  "double_shake", "raise", "lower", "knock", "circle",
  "swipe_uu", "swipe_lrl", "swipe_dd", "tap_tap_r",
};
static const char *kOrient[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
static const char *kTouch[7]  = { "TAP", "DTAP", "LONG", "SW_U", "SW_D", "SW_L", "SW_R" };

void mt_reset(MotionTokenizer *t) { memset(t, 0, sizeof(*t)); }

static int argmax_abs3(const int32_t v[3]) {
  int32_t best = -1; int bi = 0;
  for (int i = 0; i < 3; i++) {
    int32_t a = v[i] < 0 ? -v[i] : v[i];
    if (a > best) { best = a; bi = i; }
  }
  return bi;
}

int mt_push(MotionTokenizer *t, int16_t x, int16_t y, int16_t z) {
  t->buf[t->n][0] = x; t->buf[t->n][1] = y; t->buf[t->n][2] = z;
  if (++t->n < MT_WIN) return -1;
  t->n = 0;

  int32_t s[3], d[3], eax[3] = { 0, 0, 0 };
  int32_t e = 0;
  for (int k = 0; k < 3; k++) s[k] = (int32_t)t->buf[0][k] + t->buf[1][k] + t->buf[2][k];
  const int16_t *last = t->havePrev ? t->prevLast : t->buf[0];
  int16_t lastCopy[3] = { last[0], last[1], last[2] };
  for (int i = 0; i < MT_WIN; i++) {
    for (int k = 0; k < 3; k++) {
      int32_t dd = (int32_t)t->buf[i][k] - lastCopy[k];
      if (dd < 0) dd = -dd;
      e += dd; eax[k] += dd;
    }
    for (int k = 0; k < 3; k++) lastCopy[k] = t->buf[i][k];
  }
  for (int k = 0; k < 3; k++) d[k] = t->havePrev ? s[k] - t->prevSum[k] : 0;
  for (int k = 0; k < 3; k++) { t->prevSum[k] = s[k]; t->prevLast[k] = t->buf[MT_WIN - 1][k]; }
  t->havePrev = 1;

  int oax = argmax_abs3(s);
  int orient = oax * 2 + (s[oax] < 0 ? 1 : 0);
  int dax = argmax_abs3(d);
  int32_t dinf = d[dax] < 0 ? -d[dax] : d[dax];
  if (dinf < MT_STILL_D && e < MT_STILL_E) return MT_STILL0 + orient;
  if (e >= MT_JOLT_E && 6 * e > 5 * dinf) {
    int jax = argmax_abs3(eax);
    return MT_JOLT0 + jax * 2 + (e >= MT_JOLT_E2 ? 1 : 0);
  }
  int dir = dax * 2 + (d[dax] < 0 ? 1 : 0);
  int mag = dinf >= MT_BIG_D ? 1 : 0;
  return MT_MOVE0 + (dir * 2 + mag) * 6 + orient;
}

void mt_seg_reset(MotionSegmenter *s) { memset(s, 0, sizeof(*s)); }

MotionSegEvent mt_seg_push(MotionSegmenter *s, int tok) {
  if (!s->active) {
    if (mt_is_still(tok)) {
      if (s->npre < MT_PRE) s->pre[s->npre++] = tok;
      else { for (int i = 1; i < MT_PRE; i++) s->pre[i - 1] = s->pre[i]; s->pre[MT_PRE - 1] = tok; }
      return MS_NONE;
    }
    s->active = 1;
    s->len = 0;
    for (int i = 0; i < s->npre; i++) s->seq[s->len++] = s->pre[i];
    s->seq[s->len++] = tok;
    s->npre = 0;
    s->stillRun = 0;
    return MS_START;
  }
  s->seq[s->len++] = tok;
  s->stillRun = mt_is_still(tok) ? s->stillRun + 1 : 0;
  if (s->stillRun >= MT_END_STILL || s->len >= MT_MAX_LEN) {
    s->active = 0;
    s->stillRun = 0;
    return MS_END;            // seq/len stay valid until the next push
  }
  return MS_TOK;
}

const char *mt_tok_name(int t, char *b, int n) {
  if (t == MT_BOS) snprintf(b, n, "BOS");
  else if (t == MT_SEP) snprintf(b, n, "SEP");
  else if (t >= MT_STILL0 && t < MT_MOVE0) snprintf(b, n, "S%s", kOrient[t - MT_STILL0]);
  else if (t >= MT_MOVE0 && t < MT_JOLT0) {
    int k = t - MT_MOVE0, dm = k / 6;
    snprintf(b, n, "M%s%s", kOrient[dm / 2], (dm & 1) ? "!" : "");
  } else if (t >= MT_JOLT0 && t < MT_TOUCH0) {
    int k = t - MT_JOLT0;
    snprintf(b, n, "J%c%s", "XYZ"[k / 2], (k & 1) ? "!" : "");
  } else if (t >= MT_TOUCH0 && t < MT_LABEL0) snprintf(b, n, "%s", kTouch[t - MT_TOUCH0]);
  else if (t >= MT_LABEL0 && t < MT_VOCAB) snprintf(b, n, "<%s>", kLabels[t - MT_LABEL0]);
  else snprintf(b, n, "?%d", t);
  return b;
}

const char *ml_label_name(int l) {
  return (l >= 0 && l < MT_NLABELS) ? kLabels[l] : "?";
}

int ml_label_from_name(const char *name) {
  for (int i = 0; i < MT_NLABELS; i++) if (!strcmp(name, kLabels[i])) return i;
  return -1;
}
