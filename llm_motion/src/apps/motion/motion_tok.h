// motion_tok — the "motion language" tokenizer + segmenter (pure C, no
// Arduino), mirrored bit-for-bit by tools/motionlm/tokenizer.py and checked
// by tools/motionlm/crosscheck.py. Integer maths only.
//
// IMU: raw MMA8451 samples (4096 counts = 1 g) at 50 Hz -> one token per
// 3 samples (60 ms): STILL_<orientation>, MOVE_<dir>_<mag>_<orientation>,
// JOLT_<axis>_<level>. Touch gestures are tokens too. Labels are tokens at
// the end of the vocabulary; the LM predicts one after SEP.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
  MT_BOS = 0, MT_SEP = 1,
  MT_STILL0 = 2, MT_MOVE0 = 8, MT_JOLT0 = 80, MT_TOUCH0 = 86,
  MT_TAP = 86, MT_DTAP, MT_LONG, MT_SW_U, MT_SW_D, MT_SW_L, MT_SW_R,
  MT_LABEL0 = 93,
  MT_NLABELS = 15,
  MT_VOCAB = MT_LABEL0 + MT_NLABELS,   // 108
};

// Label ids (order == tools/motionlm/vocab.py LABELS)
enum {
  ML_NONE = 0, ML_FLICK_OUT, ML_FLICK_IN, ML_TWIST_L, ML_TWIST_R, ML_SHAKE,
  ML_DOUBLE_SHAKE, ML_RAISE, ML_LOWER, ML_KNOCK, ML_CIRCLE,
  ML_SWIPE_UU, ML_SWIPE_LRL, ML_SWIPE_DD, ML_TAP_TAP_R,
};

#define MT_WIN        3      // samples per token (60 ms at 50 Hz)
#define MT_STILL_D    1229   // window-sum change < 0.10 g
#define MT_BIG_D      5530   // >= 0.45 g -> big move
#define MT_STILL_E    1434   // jerk (L1 of 3 diffs x 3 axes) < 0.35 g
#define MT_JOLT_E     4915   // >= 1.2 g
#define MT_JOLT_E2    10240  // >= 2.5 g -> strong jolt
#define MT_PRE        2      // still tokens of context before motion
#define MT_END_STILL  5      // still tokens that close a segment (300 ms)
#define MT_MAX_LEN    40     // segment cap (tokens incl. pre-roll)
#define MT_SEQ_LEN    48     // LM context

typedef struct {
  int16_t buf[MT_WIN][3];
  int     n;
  int32_t prevSum[3];
  int16_t prevLast[3];
  int     havePrev;
} MotionTokenizer;

void mt_reset(MotionTokenizer *t);
// Push one sample. Returns a token id (>= 0) when a window completes, else -1.
int  mt_push(MotionTokenizer *t, int16_t x, int16_t y, int16_t z);

static inline int mt_is_still(int tok) { return tok >= MT_STILL0 && tok < MT_MOVE0; }

typedef enum { MS_NONE = 0, MS_START, MS_TOK, MS_END } MotionSegEvent;

typedef struct {
  int     active;
  int     pre[MT_PRE];
  int     npre;
  int     seq[MT_MAX_LEN];
  int     len;
  int     stillRun;
} MotionSegmenter;

void mt_seg_reset(MotionSegmenter *s);
// Feed a token. MS_START: seq[0..len) holds pre-roll + this token (feed them
// all to the LM). MS_TOK: this token was appended. MS_END: this token was
// appended and the segment is complete (seq/len valid until the next call).
MotionSegEvent mt_seg_push(MotionSegmenter *s, int tok);

// Short display names ("S+Z", "M+Y!", "JX!", "SW_U", "<shake>").
const char *mt_tok_name(int tok, char *buf, int buflen);
const char *ml_label_name(int label);      // "flick_out", ...
int         ml_label_from_name(const char *name);  // -1 if unknown

#ifdef __cplusplus
}
#endif
