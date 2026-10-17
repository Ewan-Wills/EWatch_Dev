#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "pins.h"
#include "haptic.h"
#include "ewlog.h"

// DRV2603 takes 10..250 kHz PWM; 20 kHz x 10 bit fits the S3's 80 MHz LEDC
// clock (20k * 1024 = 20.5 MHz) and gives ~0.25 % level steps.
static const uint32_t HAPTIC_FREQ_HZ  = 20000;
static const uint8_t  HAPTIC_RES_BITS = 10;
// Channel 2 = LEDC timer 1. Channels 0+1 share timer 0 with the backlight
// (ch 0, 12 kHz 8-bit, set up after us): on ch 1 the backlight silently
// reconfigured our timer and every 10-bit duty >= 256 read as 100 %.
static const uint8_t  HAPTIC_LEDC_CH  = 2;
static const uint16_t DUTY_MID        = 512;   // 50 % = coast

// ---------------------------------------------------------------------------
// Effect library. K = full forward (kick), B = full reverse (brake).
// Tuned on the watch with `hap meter` (2026-10-16, V2.0, strength 70 %):
//   * the coin ERM spins up slowly: a 25 ms kick only reaches ~50 mg,
//     35 ms ~110 mg, 60 ms ~330 mg, steady full drive ~860 mg
//   * it starts turning at an effective level of ~35 (see MIN_SPIN)
//   * full brake for ~1/3 of the kick stops it in ~30 ms (vs ~100 ms coast)
// So crisp = 30-70 ms kick + 12-25 ms brake; soft = ramps that coast out.
// ---------------------------------------------------------------------------
#define S(l, ms)        { (int8_t)(l), (int8_t)(l), (uint16_t)(ms) }
#define R(a, b, ms)     { (int8_t)(a), (int8_t)(b), (uint16_t)(ms) }
static const int8_t K = 127, B = -127;

static const HapticStep kTick[]   = { S(K, 36), S(B, 14) };
static const HapticStep kClick[]  = { S(K, 45), S(B, 17) };
static const HapticStep kDouble[] = { S(K, 45), S(B, 17), S(0, 110), S(K, 45), S(B, 17) };
static const HapticStep kSnap[]   = { S(K, 70), S(B, 25) };
static const HapticStep kThud[]   = { S(K, 50), S(60, 90), S(B, 22) };

static const HapticStep kBump[]   = { R(45, 115, 70), R(115, 45, 110) };
static const HapticStep kSwell[]  = { R(45, K, 520), S(B, 28) };
static const HapticStep kFade[]   = { S(K, 50), R(K, 40, 550) };
static const HapticStep kWave[]   = { S(K, 25), R(55, K, 350), R(K, 55, 350), S(B, 18) };

static const HapticStep kBuzz[]   = { S(K, 25), S(105, 350), S(B, 26) };
#define PURR1 S(K, 35), S(B, 10), S(0, 25)
static const HapticStep kPurr[]   = { PURR1, PURR1, PURR1, PURR1, PURR1,
                                      PURR1, PURR1, PURR1, PURR1, PURR1 };
static const HapticStep kRumble[] = { S(K, 40), S(70, 50), S(120, 40), S(55, 60), S(K, 35),
                                      S(80, 45), S(50, 50), S(125, 40), S(65, 55), S(110, 45),
                                      S(B, 25) };
static const HapticStep kEngine[] = { S(K, 45), S(B, 12), S(0, 90),  S(K, 42), S(B, 11), S(0, 65),
                                      S(K, 40), S(B, 10), S(0, 45),  S(K, 38), S(B, 9),  S(0, 30),
                                      S(K, 36), S(B, 8),  S(0, 18),  S(K, 36), S(B, 6),  S(0, 10),
                                      S(K, 240), S(B, 26) };

static const HapticStep kHeart[]  = { S(K, 55), S(B, 20), S(0, 110), S(K, 38), S(B, 14), S(0, 520),
                                      S(K, 55), S(B, 20), S(0, 110), S(K, 38), S(B, 14) };
static const HapticStep kKnock[]  = { S(K, 55), S(B, 20), S(0, 150), S(K, 45), S(B, 16), S(0, 150),
                                      S(K, 35), S(B, 12) };
static const HapticStep kGallop[] = { S(K, 38), S(B, 14), S(0, 75), S(K, 38), S(B, 14), S(0, 75),
                                      S(K, 75), S(B, 26) };

static const HapticStep kSuccess[] = { S(K, 40), S(B, 15), S(0, 110), S(K, 65), S(B, 24) };
static const HapticStep kError[]   = { S(K, 90), S(B, 26), S(0, 70), S(K, 90), S(B, 26), S(0, 70),
                                       S(K, 90), S(B, 26) };
static const HapticStep kWarning[] = { S(K, 25), S(105, 230), S(B, 25), S(0, 140),
                                       S(K, 60), S(B, 22), S(0, 140),
                                       S(K, 25), S(105, 230), S(B, 25) };
static const HapticStep kNotify[]  = { R(45, 115, 60), R(115, 45, 90), S(0, 150), R(45, 115, 60), R(115, 45, 90) };
#define ALARM3 S(K, 120), S(B, 26), S(0, 80), S(K, 120), S(B, 26), S(0, 80), S(K, 120), S(B, 26)
static const HapticStep kAlarm[]   = { ALARM3, S(0, 300), ALARM3 };
static const HapticStep kLongPr[]  = { R(45, 110, 280), S(K, 35), S(B, 20) };
static const HapticStep kTogOn[]   = { S(K, 40), S(B, 15), S(0, 95), S(K, 60), S(B, 22) };
static const HapticStep kTogOff[]  = { S(K, 60), S(B, 22), S(0, 95), S(K, 40), S(B, 15) };

#define FX(n, a, p) { n, a, (uint8_t)(sizeof(a) / sizeof(a[0])), p }
static const HapticEffect kEffects[(int)Haptic::Count] = {
  FX("Tick", kTick, 0),       FX("Click", kClick, 0),     FX("DoubleClick", kDouble, 1),
  FX("Snap", kSnap, 1),       FX("Thud", kThud, 1),
  FX("Bump", kBump, 0),       FX("Swell", kSwell, 1),     FX("Fade", kFade, 1),
  FX("Wave", kWave, 1),
  FX("Buzz", kBuzz, 1),       FX("Purr", kPurr, 1),       FX("Rumble", kRumble, 1),
  FX("Engine", kEngine, 1),
  FX("Heartbeat", kHeart, 2), FX("Knock", kKnock, 2),     FX("Gallop", kGallop, 1),
  FX("Success", kSuccess, 2), FX("Error", kError, 2),     FX("Warning", kWarning, 2),
  FX("Notify", kNotify, 2),   FX("Alarm", kAlarm, 3),     FX("LongPress", kLongPr, 1),
  FX("ToggleOn", kTogOn, 1),  FX("ToggleOff", kTogOff, 1),
};

const HapticEffect &hapticEffect(Haptic h) {
  return kEffects[(uint8_t)h < (uint8_t)Haptic::Count ? (uint8_t)h : 0];
}

int hapticFind(const char *name) {
  for (int i = 0; i < (int)Haptic::Count; i++)
    if (strcasecmp(kEffects[i].name, name) == 0) return i;
  return -1;
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
struct Req {
  const HapticStep *steps;    // nullptr -> use inl[]
  uint8_t           count;
  uint8_t           priority;
  HapticStep        inl[3];   // legacy hapticBuzz builds its pulse here
};

static QueueHandle_t     sQueue   = nullptr;
static TaskHandle_t      sTask    = nullptr;
static volatile bool     sAbort   = false;
static volatile bool     sBusy    = false;
static volatile uint8_t  sPlayPrio = 0;
static volatile uint8_t  g_strengthPct = 100;   // uint8_t writes are atomic
static int8_t            sLevel   = 0;          // last level written (task-owned)
static bool              sEnabled = false;

// The ERM starts turning at an effective level of ~35 and is weak until ~45
// (measured: level 35 -> ~100 mg, 49 -> ~410 mg steady).
static const int MIN_SPIN = 45;

// Brownout guard: full drive held for more than ~200 ms spikes motor current
// enough to drop the soft-latch rail on a low battery (seen in Tunnel, see
// apps/games/tunnel.cpp). Whatever an effect asks for, forward drive above
// HOLD_MAX is capped once it has been held for HOLD_MS.
static const int      HOLD_MAX = 105;
static const uint32_t HOLD_MS  = 150;
static uint32_t       sHighSince = 0;   // millis() when drive went above HOLD_MAX (0 = not)

static void writeLevel(int level) {
  if (level > 127) level = 127;
  if (level < -127) level = -127;
  if (level > HOLD_MAX) {
    uint32_t now = millis();
    if (!sHighSince) sHighSince = now | 1;
    else if (now - sHighSince > HOLD_MS) level = HOLD_MAX;
  } else {
    sHighSince = 0;
  }
  // Strength scales only the part above the spin threshold, so a lower
  // setting feels softer instead of dropping effects below where the motor
  // turns at all. Levels at or under MIN_SPIN (ramp starts) pass through.
  int mag = level < 0 ? -level : level;
  if (mag > MIN_SPIN) mag = MIN_SPIN + (mag - MIN_SPIN) * (int)g_strengthPct / 100;
  int scaled = level < 0 ? -mag : mag;
  sLevel = (int8_t)scaled;
  uint32_t duty = scaled <= -127 ? 0 : DUTY_MID + scaled * 511 / 127;   // 1023 = 100 %
  if (!sEnabled) { digitalWrite(PIN_MOTOR_EN, HIGH); sEnabled = true; }
  ledcWrite(HAPTIC_LEDC_CH, duty);
}

static void motorOff() {
  sHighSince = 0;
  ledcWrite(HAPTIC_LEDC_CH, DUTY_MID);
  digitalWrite(PIN_MOTOR_EN, LOW);
  sEnabled = false;
  sLevel = 0;
}

// Wait `ms`, returning false early if a higher-priority request aborted us.
static bool waitMs(uint32_t ms) {
  if (!ms) return !sAbort;
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
  return !sAbort;
}

// Brake proportional to how fast the motor was being driven. A full-speed
// ERM stops in ~20 ms of full reverse; longer and it starts spinning back.
static void brakeFromLevel(int8_t lvl) {
  if (lvl <= 20) return;
  writeLevel(B);
  vTaskDelay(pdMS_TO_TICKS(8 + 18 * lvl / 127));
}

static bool playSteps(const HapticStep *st, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    const HapticStep &s = st[i];
    if (s.from == s.to || s.ms < 4) {
      writeLevel(s.to);
      if (!waitMs(s.ms)) return false;
      continue;
    }
    // Ramp in 2 ms slices.
    for (uint16_t t = 0; t < s.ms; t += 2) {
      writeLevel(s.from + ((int)s.to - s.from) * (int)t / (int)s.ms);
      if (!waitMs(2)) return false;
    }
    writeLevel(s.to);
  }
  return true;
}

static void hapticTask(void *) {
  Req r;
  for (;;) {
    if (xQueueReceive(sQueue, &r, portMAX_DELAY) != pdPASS) continue;
    sAbort = false;
    sPlayPrio = r.priority;
    sBusy = true;
    bool done = playSteps(r.steps ? r.steps : r.inl, r.count);
    if (!done) brakeFromLevel(sLevel);
    // Back-to-back requests keep EN high (no 1.3 ms restart); otherwise stop.
    if (uxQueueMessagesWaiting(sQueue) == 0) { motorOff(); sBusy = false; }
    else writeLevel(0);
  }
}

void hapticBegin() {
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, LOW);
  ledcSetup(HAPTIC_LEDC_CH, HAPTIC_FREQ_HZ, HAPTIC_RES_BITS);
  ledcAttachPin(PIN_MOTOR_PWM, HAPTIC_LEDC_CH);
  ledcWrite(HAPTIC_LEDC_CH, DUTY_MID);

  sQueue = xQueueCreate(6, sizeof(Req));
  xTaskCreatePinnedToCore(hapticTask, "haptic", 2560, nullptr, 6, &sTask, 1);
}

static void submit(const Req &r) {
  if (!sQueue || g_strengthPct == 0) return;
  if (sBusy && r.priority > sPlayPrio) {
    xQueueReset(sQueue);          // drop the queued small stuff too
    sAbort = true;
    xTaskNotifyGive(sTask);
  }
  xQueueSend(sQueue, &r, 0);      // full queue: drop (UI feedback isn't worth backpressure)
}

void hapticPlay(const HapticEffect &e) {
  // One line per effect (not per legacy buzz): lets tests prove an alert
  // actually reached the motor driver, e.g. the timer alarm after sleep.
  if (sQueue && g_strengthPct) EWLOGI("HAP", "play %s prio=%u str=%u", e.name, e.priority, g_strengthPct);
  Req r{};
  r.steps = e.steps; r.count = e.count; r.priority = e.priority;
  submit(r);
}

void hapticPlay(Haptic h) { hapticPlay(hapticEffect(h)); }

void hapticStop() {
  if (!sQueue) return;
  xQueueReset(sQueue);
  if (sBusy) { sAbort = true; xTaskNotifyGive(sTask); }
}

uint32_t hapticPwmFreq() { return ledcReadFreq(HAPTIC_LEDC_CH); }

bool hapticBusy() { return sBusy || (sQueue && uxQueueMessagesWaiting(sQueue)); }

// Legacy pulse. Old firmware wrote `intensity` straight to the duty cycle,
// so anything under 128 actually drove the motor in REVERSE (and 128 did
// nothing). Now intensity maps onto forward drive above the level where the
// motor reliably spins, with a kick to spin up and a brake to stop.

void hapticBuzz(uint8_t intensity, uint16_t duration_ms) {
  if (duration_ms == 0) return;
  int a = MIN_SPIN + (127 - MIN_SPIN) * intensity / 255;
  uint16_t kick = duration_ms < 30 ? duration_ms : 30;   // spin-up, measured
  Req r{};
  r.steps = nullptr;
  r.priority = duration_ms >= 300 ? 2 : 0;
  r.inl[0] = S(K, kick);
  r.count = 1;
  if (duration_ms > kick) r.inl[r.count++] = S(a, duration_ms - kick);
  r.inl[r.count++] = S(B, 8 + 14 * a / 127);
  submit(r);
}

uint8_t hapticStrengthPct() { return g_strengthPct; }

void hapticSetStrengthPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_strengthPct = pct;
}
