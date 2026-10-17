// Motion engine — see motion_engine.h and llm_motion/NOTES.md.
#include "motion_engine.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string.h>
#include <math.h>

#include "model.h"
#include "view.h"
#include "ewlog.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "storage.h"
#include "power_mgr.h"
#include "motion_tok.h"
#include "motion_lm.h"
#include "motion_traces.h"
#if defined(EWATCH_ENABLE_SPOTIFY) && EWATCH_ENABLE_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  #include "spotify.h"
  #define MOTION_HAVE_SPOTIFY 1
#endif

// ---------------------------------------------------------------------------
// Label -> action table (shown on the app's mapping page)
// ---------------------------------------------------------------------------
static const char *kActionText[MT_NLABELS] = {
  "-",                // none
  "Next track",       // flick_out
  "Prev track",       // flick_in
  "Torch on/off",     // twist_l
  "Torch on/off",     // twist_r
  "Back",             // shake
  "Home (face)",      // double_shake
  "Wake + time",      // raise
  "Screen off",       // lower
  "Play/pause",       // knock
  "Quick settings",   // circle
  "App launcher",     // swipe_uu
  "Silent on/off",    // swipe_lrl
  "Home (face)",      // swipe_dd
  "Stopwatch",        // tap_tap_r
};
const char *motionActionText(int l) { return (l >= 0 && l < MT_NLABELS) ? kActionText[l] : "?"; }

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
struct QItem { uint8_t kind; int16_t a, b, c; };   // kind 0 = sample(a,b,c), 1 = touch token (a)

static QueueHandle_t     sQ = nullptr;
static TaskHandle_t      sTask = nullptr;
static volatile bool     sFeedOn = false;      // I/O task pushes samples
static volatile bool     sResetReq = false;
static volatile bool     sViewOpen = false;
static portMUX_TYPE      sMux = portMUX_INITIALIZER_UNLOCKED;
static MotionSnap        sSnap;                // guarded by sMux
static volatile int      sPendingAction = -1;  // label for the render task
static volatile bool     sVerbose = false;
static volatile int      sBenchReq = 0;
static volatile bool     sBenchDone = false;
static float             sBenchUs = 0, sBenchMin = 0, sBenchMax = 0;
static const MotionTraceRef *volatile sInj = nullptr;
static volatile int      sInjPos = 0;
static Screen            sTorchReturn = Screen::Watch;

// engine-task-only
static MotionTokenizer   sTok;
static MotionSegmenter   sSeg;
static int               sTouchToks[8];
static int               sNTouch = 0;
static uint32_t          sLastTouchMs = 0;
static int16_t           sRaw[256][3];
static uint32_t          sRawN = 0;
static float             sSegFwdMs = 0;
static uint32_t          sRateT0 = 0, sRateN = 0;

static inline void snapBump() { sSnap.version++; }

bool motionEngineReady() { return sSnap.ready; }

void motionGetSnap(MotionSnap *out) {
  portENTER_CRITICAL(&sMux);
  *out = sSnap;
  portEXIT_CRITICAL(&sMux);
}

void motionSetDry(bool d)      { portENTER_CRITICAL(&sMux); sSnap.dry = d; snapBump(); portEXIT_CRITICAL(&sMux);
                                 EWLOGI("MOTION", "dry=%d", d ? 1 : 0); }
void motionSetCapture(int l)   { portENTER_CRITICAL(&sMux); sSnap.capLabel = l; snapBump(); portEXIT_CRITICAL(&sMux);
                                 EWLOGI("MOTION", "capture=%s", l >= 0 ? ml_label_name(l) : "off"); }

// ---------------------------------------------------------------------------
// LM helpers (engine task only)
// ---------------------------------------------------------------------------
static const float *feedTimed(int ctx, int tok) {
  int64_t t0 = esp_timer_get_time();
  const float *lg = mlm_feed(ctx, tok);
  float ms = (float)(esp_timer_get_time() - t0) / 1000.0f;
  sSegFwdMs += ms;
  sRateN++;
  portENTER_CRITICAL(&sMux);
  sSnap.lastFwdMs = ms;
  sSnap.forwards++;
  sSnap.avgFwdMs += (ms - sSnap.avgFwdMs) / (sSnap.forwards < 64 ? sSnap.forwards : 64);
  portEXIT_CRITICAL(&sMux);
  return lg;
}

static int predictNext(const float *lg) {
  if (!lg) return -1;
  int bi = MT_STILL0; float m = lg[MT_STILL0];
  for (int i = MT_STILL0 + 1; i < MT_TOUCH0; i++) if (lg[i] > m) { m = lg[i]; bi = i; }
  return bi;
}

static void labelSoftmax(const float *lg, int *best, float *p) {
  float m = -1e30f; int bi = 0;
  for (int i = 0; i < MT_NLABELS; i++) if (lg[MT_LABEL0 + i] > m) { m = lg[MT_LABEL0 + i]; bi = i; }
  float s = 0;
  for (int i = 0; i < MT_NLABELS; i++) s += expf(lg[MT_LABEL0 + i] - m);
  *best = bi; *p = 1.0f / s;
}

static void pushDisplayTok(int t, int pred) {
  portENTER_CRITICAL(&sMux);
  if (sSnap.ntoks < 8) sSnap.toks[sSnap.ntoks++] = (uint8_t)t;
  else { memmove(sSnap.toks, sSnap.toks + 1, 7); sSnap.toks[7] = (uint8_t)t; }
  sSnap.predNext = (int16_t)pred;
  sSnap.segActive = sSeg.active;
  sSnap.segLen = (uint8_t)sSeg.len;
  snapBump();
  portEXIT_CRITICAL(&sMux);
  if (sVerbose) {
    char b[16];
    Serial.printf("MTOK %s\n", mt_tok_name(t, b, sizeof b));
  }
}

// One recognition result: log, capture, haptic, queue the action.
static void report(const float *lg, bool touch, const int *seq, int len) {
  int64_t t0 = esp_timer_get_time();
  int best; float p;
  labelSoftmax(lg, &best, &p);
  float clsMs = (float)(esp_timer_get_time() - t0) / 1000.0f + sSnap.lastFwdMs;   // SEP forward + softmax
  bool dry; int cap; float thr;
  portENTER_CRITICAL(&sMux);
  dry = sSnap.dry; cap = sSnap.capLabel; thr = sSnap.thr;
  portEXIT_CRITICAL(&sMux);
  int fin = (p >= thr) ? best : ML_NONE;
  EWLOGI("MOTION", "gesture=%s p=%.2f ms=%.2f seg_ms=%.2f src=%s n=%d best=%s",
         ml_label_name(fin), p, clsMs, sSegFwdMs, touch ? "touch" : "imu", len, ml_label_name(best));

  // Data capture for real-wrist fine-tuning (Serial only: not the flash log ring).
  if (cap >= 0) {
    static uint32_t capN = 0;
    capN++;
    char line[320];
    int o = snprintf(line, sizeof line, "MCAP n=%lu label=%s pred=%s p=%.3f src=%s toks=",
                     (unsigned long)capN, ml_label_name(cap), ml_label_name(best), p, touch ? "touch" : "imu");
    for (int i = 0; i < len && o < (int)sizeof line - 6; i++) o += snprintf(line + o, sizeof line - o, i ? ",%d" : "%d", seq[i]);
    Serial.printf("%s\n", line);
    if (!touch) {
      // (len + 1) windows: one priming window so the host re-tokenizes exactly.
      int ns = (len + 1) * MT_WIN;
      if (ns > 255) ns = 255;
      if ((uint32_t)ns > sRawN) ns = (int)sRawN;
      int parts = (ns + 15) / 16;
      for (int pi = 0; pi < parts; pi++) {
        o = snprintf(line, sizeof line, "MCAPRAW n=%lu part=%d/%d ", (unsigned long)capN, pi + 1, parts);
        for (int k = pi * 16; k < ns && k < (pi + 1) * 16; k++) {
          const int16_t *s = sRaw[(sRawN - ns + k) & 255];
          o += snprintf(line + o, sizeof line - o, "%d,%d,%d;", s[0], s[1], s[2]);
        }
        Serial.printf("%s\n", line);
      }
    }
    Serial.flush();
  }

  portENTER_CRITICAL(&sMux);
  sSnap.lastLabel = fin;
  sSnap.lastBest = best;
  sSnap.lastP = p;
  sSnap.lastTouch = touch;
  sSnap.lastAtMs = millis();
  sSnap.lastClassifyMs = clsMs;
  sSnap.segments++;
  if (fin != ML_NONE) sSnap.recognised++;
  sSnap.segActive = false;
  snprintf(sSnap.lastAction, sizeof sSnap.lastAction, "%s%s",
           fin != ML_NONE ? kActionText[fin] : "-", (fin != ML_NONE && dry) ? " (dry)" : "");
  snapBump();
  portEXIT_CRITICAL(&sMux);

  if (fin != ML_NONE) {
    haptic(HapticRole::Tap);
    sPendingAction = fin;             // render task logs (dry) or executes it
  }
}

static void processSample(int16_t x, int16_t y, int16_t z) {
  sRaw[sRawN & 255][0] = x; sRaw[sRawN & 255][1] = y; sRaw[sRawN & 255][2] = z;
  sRawN++;
  int t = mt_push(&sTok, x, y, z);
  if (t < 0) return;
  MotionSegEvent ev = mt_seg_push(&sSeg, t);
  const float *lg = nullptr;
  if (ev == MS_START) {
    sSegFwdMs = 0;
    mlm_reset(0);
    feedTimed(0, MT_BOS);
    for (int k = 0; k < sSeg.len; k++) lg = feedTimed(0, sSeg.seq[k]);
  } else if (ev == MS_TOK || ev == MS_END) {
    lg = feedTimed(0, t);
  }
  pushDisplayTok(t, lg ? predictNext(lg) : -1);
  if (ev == MS_END) {
    const float *out = feedTimed(0, MT_SEP);
    if (out) report(out, false, sSeg.seq, sSeg.len);
  }
}

static void finishTouch() {
  const float *lg = feedTimed(1, MT_SEP);
  if (lg) report(lg, true, sTouchToks, sNTouch);
  sNTouch = 0;
}

static void processTouch(int tok) {
  if (sNTouch == 0) { sSegFwdMs = 0; mlm_reset(1); feedTimed(1, MT_BOS); }
  sTouchToks[sNTouch++] = tok;
  feedTimed(1, tok);
  sLastTouchMs = millis();
  pushDisplayTok(tok, -1);
  if (sNTouch >= 6) finishTouch();
}

static void runBench(int n) {
  float mn = 1e9f, mx = 0, sum = 0;
  int done = 0;
  while (done < n) {
    mlm_reset(1);
    for (int k = 0; k < 24 && done < n; k++, done++) {
      int64_t t0 = esp_timer_get_time();
      mlm_feed(1, MT_MOVE0 + (k * 7) % 72);
      float us = (float)(esp_timer_get_time() - t0);
      sum += us; if (us < mn) mn = us; if (us > mx) mx = us;
    }
    vTaskDelay(1);                 // let the I/O task and WDT breathe
  }
  sNTouch = 0;
  sBenchUs = sum / n; sBenchMin = mn; sBenchMax = mx;
}

static void engineTask(void *) {
  const char *err = nullptr;
  bool ok = mlm_init(&err);
  portENTER_CRITICAL(&sMux);
  sSnap.ready = ok;
  snapBump();
  portEXIT_CRITICAL(&sMux);
  if (ok) {
    EWLOGI("MOTION", "model_ready params=%ld dim=%d layers=%d hidden=%d heads=%d vocab=%d seq=%d state_bytes=%lu",
           mlm_params(), mlm_dim(), mlm_layers(), mlm_hidden(), mlm_heads(), mlm_vocab(), mlm_seq_len(),
           (unsigned long)mlm_state_bytes());
  } else {
    EWLOGE("MOTION", "model_init_failed err=%s", err ? err : "?");
  }
  sRateT0 = millis();
  for (;;) {
    QItem it;
    bool got = xQueueReceive(sQ, &it, pdMS_TO_TICKS(50)) == pdPASS;
    if (!ok) continue;
    if (sResetReq) {
      sResetReq = false;
      mt_reset(&sTok); mt_seg_reset(&sSeg); sNTouch = 0;
      portENTER_CRITICAL(&sMux); sSnap.segActive = false; sSnap.ntoks = 0; snapBump(); portEXIT_CRITICAL(&sMux);
    }
    if (got && it.kind == 0) {
      int16_t x = it.a, y = it.b, z = it.c;
      const MotionTraceRef *inj = sInj;
      bool injEnded = false;
      if (inj) {                         // replay a canned trace in place of the live sensor
        int p = sInjPos;
        // Fresh tokenizer at both ends of the trace, so the step between the
        // live sensor and the canned trace doesn't become a phantom "move".
        if (p == 0) { mt_reset(&sTok); mt_seg_reset(&sSeg); }
        const int16_t *s = kMotionTraceSamples[inj->offset + p];
        x = s[0]; y = s[1]; z = s[2];
        if (++p >= inj->count) { sInj = nullptr; p = 0; injEnded = true; }
        sInjPos = p;
        portENTER_CRITICAL(&sMux); sSnap.injectLeft = sInj ? (uint16_t)(inj->count - p) : 0; portEXIT_CRITICAL(&sMux);
      }
      processSample(x, y, z);
      if (injEnded) { mt_reset(&sTok); mt_seg_reset(&sSeg); EWLOGI("MOTION", "inject_done"); }
    } else if (got && it.kind == 1) {
      processTouch(it.a);
    }
    if (sNTouch > 0 && millis() - sLastTouchMs > 700) finishTouch();
    if (sBenchReq > 0) { runBench(sBenchReq); sBenchReq = 0; sBenchDone = true; }
    uint32_t now = millis();
    if (now - sRateT0 >= 1000) {
      portENTER_CRITICAL(&sMux);
      sSnap.tokPerSec = sRateN * 1000.0f / (now - sRateT0);
      snapBump();
      portEXIT_CRITICAL(&sMux);
      sRateN = 0; sRateT0 = now;
    }
  }
}

static void ensureTask() {
  if (sTask) return;
  sQ = xQueueCreate(64, sizeof(QItem));
  sSnap.dry = true;
  sSnap.capLabel = -1;
  sSnap.lastLabel = -1;
  sSnap.lastBest = -1;
  sSnap.predNext = -1;
  sSnap.thr = 0.6f;
  strcpy(sSnap.lastAction, "-");
  mt_reset(&sTok); mt_seg_reset(&sSeg);
  // Core 0 beside the I/O task (prio 5) and below it: inference never delays
  // sensor polling, and the render task on core 1 is never blocked by it.
  xTaskCreatePinnedToCore(engineTask, "motion", 6144, nullptr, 2, &sTask, 0);
}

static void setRunning(bool on) {
  if (on && !sFeedOn) sResetReq = true;
  sFeedOn = on;
  portENTER_CRITICAL(&sMux); sSnap.running = on; snapBump(); portEXIT_CRITICAL(&sMux);
}

void motionEngineStart() {
  ensureTask();
  sViewOpen = true;
  if (!sFeedOn) EWLOGI("MOTION", "engine=on");
  setRunning(true);
}

void motionEngineStop() {
  sViewOpen = false;
  if (sSnap.global) return;              // background mode keeps it running
  if (sFeedOn) EWLOGI("MOTION", "engine=off");
  setRunning(false);
  sInj = nullptr;
}

void motionSetGlobal(bool on) {
  ensureTask();
  portENTER_CRITICAL(&sMux); sSnap.global = on; snapBump(); portEXIT_CRITICAL(&sMux);
  EWLOGI("MOTION", "global=%d", on ? 1 : 0);
  if (on) setRunning(true);
  else if (!sViewOpen) setRunning(false);
}

void motionFeedSample(bool ok, int16_t ax, int16_t ay, int16_t az) {
  if (!sFeedOn || !ok || !sQ) return;
  QItem it = { 0, ax, ay, az };
  xQueueSend(sQ, &it, 0);               // never block the I/O task
}

void motionFeedTouch(Gesture g) {
  if (!sFeedOn || !sQ) return;
  int t;
  switch (g) {
    case Gesture::SingleTap:  t = MT_TAP;  break;
    case Gesture::DoubleTap:  t = MT_DTAP; break;
    case Gesture::LongPress:  t = MT_LONG; break;
    case Gesture::SwipeUp:    t = MT_SW_U; break;
    case Gesture::SwipeDown:  t = MT_SW_D; break;
    case Gesture::SwipeLeft:  t = MT_SW_L; break;
    case Gesture::SwipeRight: t = MT_SW_R; break;
    default: return;
  }
  QItem it = { 1, (int16_t)t, 0, 0 };
  xQueueSend(sQ, &it, 0);
}

// ---------------------------------------------------------------------------
// Actions (render task)
// ---------------------------------------------------------------------------
void motionServiceRender() {
  int a = sPendingAction;
  if (a < 0) return;
  sPendingAction = -1;
  bool dry;
  portENTER_CRITICAL(&sMux); dry = sSnap.dry; portEXIT_CRITICAL(&sMux);
  if (dry) { EWLOGI("MOTION", "action=\"%s\" label=%s dry=1", kActionText[a], ml_label_name(a)); return; }
  Screen cur;
  { ModelLock lk; cur = model.screen; }
  bool done = true;
  switch (a) {
#ifdef MOTION_HAVE_SPOTIFY
    case ML_FLICK_OUT: spotifyPostCmd(SpotifyCmd::Next); break;
    case ML_FLICK_IN:  spotifyPostCmd(SpotifyCmd::Prev); break;
    case ML_KNOCK:     spotifyPostCmd(SpotifyCmd::PlayPause); break;
#else
    case ML_FLICK_OUT: case ML_FLICK_IN: case ML_KNOCK: done = false; break;
#endif
    case ML_TWIST_L: case ML_TWIST_R:
      if (cur == Screen::Torch) switchTo(sTorchReturn);
      else { sTorchReturn = cur; switchTo(Screen::Torch); }
      break;
    case ML_SHAKE:
      if (cur != Screen::Watch) postEvent(makeEvent(EventType::ButtonShort));   // the view's own "back"
      else done = false;                 // ButtonShort on the face = sleep: not a "back"
      break;
    case ML_DOUBLE_SHAKE: case ML_SWIPE_DD: switchTo(Screen::Watch); break;
    case ML_RAISE:      powerOnActivity(); switchTo(Screen::Watch); break;
    case ML_LOWER:      powerRequestSleep(); break;
    case ML_CIRCLE:     switchTo(Screen::QuickSettings); break;
    case ML_SWIPE_UU:   switchTo(Screen::AppList); break;
    case ML_SWIPE_LRL: {                 // same as Quick Settings' silent tile
      uint8_t hs;
      { ModelLock lk; model.hapticStrength = model.hapticStrength ? 0 : 100; hs = model.hapticStrength; model.revision++; }
      hapticSetStrengthPct(hs);
      Storage::save();
      if (hs) haptic(HapticRole::ToggleOn);
      break;
    }
    case ML_TAP_TAP_R:  switchTo(Screen::Stopwatch); break;
    default: done = false; break;
  }
  EWLOGI("MOTION", "action=\"%s\" label=%s dry=0 done=%d", kActionText[a], ml_label_name(a), done ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Console: motion [inject|dry|global|cap|thr|bench|verbose|classes]
// ---------------------------------------------------------------------------
static void printStatus() {
  MotionSnap s; motionGetSnap(&s);
  Serial.printf("motion: ready=%d running=%d dry=%d global=%d cap=%s thr=%.2f fwd=%lu segs=%lu rec=%lu "
                "avg_fwd_ms=%.3f last_cls_ms=%.3f params=%ld state_bytes=%lu\n",
                s.ready, s.running, s.dry, s.global, s.capLabel >= 0 ? ml_label_name(s.capLabel) : "off",
                s.thr, (unsigned long)s.forwards, (unsigned long)s.segments, (unsigned long)s.recognised,
                s.avgFwdMs, s.lastClassifyMs, s.ready ? mlm_params() : 0L,
                (unsigned long)(s.ready ? mlm_state_bytes() : 0));
}

bool motionConsole(const char *line) {
  if (strncmp(line, "motion", 6) != 0 || (line[6] != '\0' && line[6] != ' ')) return false;
  char cmd[16] = "", arg[24] = "";
  int v = -1;
  int n = sscanf(line + 6, "%15s %23s %d", cmd, arg, &v);
  ensureTask();
  if (n <= 0 || !strcmp(cmd, "status")) { printStatus(); return true; }
  if (!strcmp(cmd, "classes")) {
    for (int i = 0; i < MT_NLABELS; i++) Serial.printf("motion: %2d %-13s -> %s\n", i, ml_label_name(i), kActionText[i]);
    return true;
  }
  if (!strcmp(cmd, "dry"))     { motionSetDry(atoi(arg) != 0); printStatus(); return true; }
  if (!strcmp(cmd, "global"))  { motionSetGlobal(atoi(arg) != 0); printStatus(); return true; }
  if (!strcmp(cmd, "verbose")) { sVerbose = atoi(arg) != 0; Serial.printf("motion: verbose=%d\n", sVerbose); return true; }
  if (!strcmp(cmd, "thr")) {
    int pct = atoi(arg);
    if (pct < 1 || pct > 99) { Serial.println("motion: usage: motion thr <1-99>"); return true; }
    portENTER_CRITICAL(&sMux); sSnap.thr = pct / 100.0f; snapBump(); portEXIT_CRITICAL(&sMux);
    printStatus(); return true;
  }
  if (!strcmp(cmd, "cap")) {
    if (!strcmp(arg, "off") || !arg[0]) { motionSetCapture(-1); Serial.println("motion: capture off"); return true; }
    int l = ml_label_from_name(arg);
    if (l < 0) { Serial.printf("motion: unknown label '%s' (motion classes)\n", arg); return true; }
    motionSetCapture(l);
    Serial.printf("motion: capturing label=%s (MCAP lines on serial; do the gesture, pause, repeat)\n", arg);
    return true;
  }
  if (!strcmp(cmd, "inject")) {
    int l = ml_label_from_name(arg);
    if (l < 0 || l > ML_CIRCLE) { Serial.printf("motion: usage: motion inject <imu class> [variant] (motion classes)\n"); return true; }
    if (!sFeedOn) { Serial.println("motion: engine not running (open the Motion app or 'motion global 1')"); return true; }
    int var = v >= 0 ? v : 0;
    const MotionTraceRef *r = nullptr;
    for (int i = 0; i < kMotionTraceCount; i++)
      if (kMotionTraces[i].label == l && kMotionTraces[i].variant == var) r = &kMotionTraces[i];
    if (!r) { Serial.printf("motion: no trace %s v%d\n", arg, var); return true; }
    sInjPos = 0;
    sInj = r;
    EWLOGI("MOTION", "inject=%s v=%d samples=%ld", arg, var, (long)r->count);
    return true;
  }
  if (!strcmp(cmd, "bench")) {
    int nf = atoi(arg); if (nf <= 0) nf = 240;
    if (!sSnap.ready) { Serial.println("motion: model not ready"); return true; }
    sBenchDone = false; sBenchReq = nf;
    uint32_t t0 = millis();
    while (!sBenchDone && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(20));
    if (!sBenchDone) { Serial.println("motion: bench timeout"); return true; }
    EWLOGI("MOTION", "bench n=%d us_per_fwd=%.1f min=%.1f max=%.1f tok_s=%.0f",
           nf, sBenchUs, sBenchMin, sBenchMax, sBenchUs > 0 ? 1e6f / sBenchUs : 0.0f);
    return true;
  }
  Serial.println("motion: commands: motion [status|classes|inject <class> [v]|dry 0|1|global 0|1|"
                 "cap <label>|off|thr <pct>|bench [n]|verbose 0|1]");
  return true;
}
