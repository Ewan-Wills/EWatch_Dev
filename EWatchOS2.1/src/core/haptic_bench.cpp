// Haptic bench: `hap ...` console commands + a vibration meter that uses the
// on-board MMA8451 to measure what each effect actually does to the case.
//
//   hap list                     effect names
//   hap play NAME | hap buzz I MS | hap stop
//   hap raw L MS [L MS ...]      custom steps (level -127..127, ms), max 16
//   hap meter NAME|raw|none|all  play + record the vibration envelope
//
// Meter output (one pair per effect):
//   HM <name> drive=<ms> peak=<mg> on=<ms> tail=<ms> floor=<mg>
//   HMD <name> <bin ms> <mg> <mg> ...      envelope from the trigger
// peak = loudest 10 ms bin (RMS of the high-passed |a|, milli-g)
// on   = first bin >= 30 % of peak; tail = time after the drive ends until
//        the envelope stays under 15 % of peak (how well the brake works)
// floor = median of the 50 ms before the trigger (sensor noise)
//
// `hap meter none` records with the motor idle: it must report a peak near
// the floor, or the meter is measuring something other than the motor.
//
// The accelerometer belongs to the I/O task, so the console only queues the
// request and hapticMeterPoll() (called from taskIO) does the recording,
// blocking that task for up to ~2.5 s. Bench use only.
#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include "haptic.h"
#include "haptic_bench.h"
#include "haptic_roles.h"
#include "controller.h"
#include "i2c_bus.h"
#include "ewsim.h"        // ewsimEnsureAwake: the meter needs the I/O task running

static HapticStep   sRaw[16];
static HapticEffect sRawFx = { "raw", sRaw, 0, 1 };

static volatile int  sMeterReq  = -3;   // -3 idle, -2 none, -1 raw, >=0 effect
static volatile bool sMeterAll  = false;

static const int BIN_MS = 10, PRE_MS = 50, POST_MS = 300, MAX_BINS = 260;

static uint32_t driveMs(const HapticEffect &e) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < e.count; i++) t += e.steps[i].ms;
  return t;
}

static void mmaW(uint8_t a, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(a); Wire.write(reg); Wire.write(v); Wire.endTransmission();
}

static void runMeter(int which) {
  uint8_t a = mmaAddress();
  if (!a) { Serial.println("HM error no accelerometer"); return; }
  const HapticEffect *fx = which >= 0 ? &hapticEffect((Haptic)which)
                         : which == -1 ? &sRawFx : nullptr;
  const char *name = fx ? fx->name : "none";
  uint32_t drive = fx ? driveMs(*fx) : 500;
  int bins = (PRE_MS + drive + POST_MS) / BIN_MS;
  if (bins > MAX_BINS) bins = MAX_BINS;

  // Save the wake config, switch to 800 Hz / high-res / +-2 g / FIFO off.
  uint8_t c1 = 0, c2 = 0, xyz = 0, fs = 0;
  i2cReadReg(a, 0x2A, c1); i2cReadReg(a, 0x2B, c2);
  i2cReadReg(a, 0x0E, xyz); i2cReadReg(a, 0x09, fs);
  mmaW(a, 0x2A, 0x00);
  mmaW(a, 0x09, 0x00);
  mmaW(a, 0x0E, 0x00);
  mmaW(a, 0x2B, 0x02);
  mmaW(a, 0x2A, 0x01);   // DR=800 Hz, ACTIVE
  delay(5);

  static uint16_t env[MAX_BINS];
  float ema[3] = {0, 0, 0};
  bool  seeded = false;
  int16_t x, y, z;
  int64_t t0 = esp_timer_get_time();
  int64_t trig = t0 + PRE_MS * 1000;
  bool fired = false;
  for (int b = 0; b < bins; b++) {
    float acc = 0; int n = 0;
    int64_t binEnd = t0 + (int64_t)(b + 1) * BIN_MS * 1000;
    while (esp_timer_get_time() < binEnd) {
      int64_t next = esp_timer_get_time() + 1250;
      if (!fired && esp_timer_get_time() >= trig) {
        if (fx) hapticPlay(*fx);
        fired = true;
      }
      if (readAccel(x, y, z)) {
        float v[3] = {(float)x, (float)y, (float)z};
        if (!seeded) { for (int k = 0; k < 3; k++) ema[k] = v[k]; seeded = true; }
        float s = 0;
        for (int k = 0; k < 3; k++) {
          ema[k] += (v[k] - ema[k]) * 0.125f;       // ~16 Hz high-pass corner
          float d = v[k] - ema[k];
          s += d * d;
        }
        acc += s; n++;
      }
      while (esp_timer_get_time() < next) {}
    }
    // 4096 counts/g at +-2 g, 14 bit.
    env[b] = n ? (uint16_t)(sqrtf(acc / n) * 1000.0f / 4096.0f) : 0;
    esp_task_wdt_reset();
  }

  mmaW(a, 0x2A, 0x00);
  mmaW(a, 0x09, fs); mmaW(a, 0x0E, xyz); mmaW(a, 0x2B, c2);
  mmaW(a, 0x2A, c1);

  int pre = PRE_MS / BIN_MS;
  uint16_t sorted[PRE_MS / BIN_MS];
  for (int i = 0; i < pre; i++) sorted[i] = env[i];
  for (int i = 0; i < pre; i++) for (int j = i + 1; j < pre; j++)
    if (sorted[j] < sorted[i]) { uint16_t t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
  uint16_t floorMg = sorted[pre / 2];
  uint16_t peak = 0;
  for (int b = pre; b < bins; b++) if (env[b] > peak) peak = env[b];
  int on = -1, tail = -1;
  for (int b = pre; b < bins; b++) if (env[b] * 10 >= peak * 3) { on = (b - pre) * BIN_MS; break; }
  // Tail is timed from the end of the last FORWARD step, so brake time
  // counts as stopping time, not drive time.
  uint32_t fwdEnd = 0, tt = 0;
  if (fx) for (uint8_t i = 0; i < fx->count; i++) { tt += fx->steps[i].ms; if (fx->steps[i].to > 0) fwdEnd = tt; }
  else fwdEnd = drive;
  int endBin = pre + (int)(fwdEnd + BIN_MS - 1) / BIN_MS;
  for (int b = bins - 1; b >= pre; b--)
    if (env[b] * 100 >= peak * 15) { tail = (b + 1 - endBin) * BIN_MS; break; }
  if (tail < 0) tail = 0;

  // HWCDC drops the rest of a write after tx_timeout (100 ms) of full ring
  // (same as `log dump`): allow 1 s, and send each line as ONE write so
  // other tasks' log lines can't land inside it. The host checks n/sum.
  uint32_t sum = 0;
  for (int b = pre; b < bins; b++) sum += env[b];
  Serial.setTxTimeoutMs(1000);
  char buf[160];
  int len = snprintf(buf, sizeof buf, "HM %s drive=%lu fwd=%lu peak=%u on=%d tail=%d floor=%u n=%d sum=%lu\n",
                     name, (unsigned long)drive, (unsigned long)fwdEnd, peak, on, tail, floorMg,
                     bins - pre, (unsigned long)sum);
  Serial.write((const uint8_t *)buf, len);
  for (int b = pre; b < bins; b += 16) {
    len = snprintf(buf, sizeof buf, "HMD %s %d %d", name, BIN_MS, b - pre);
    for (int k = b; k < bins && k < b + 16; k++) len += snprintf(buf + len, sizeof buf - len, " %u", env[k]);
    buf[len++] = '\n';
    Serial.write((const uint8_t *)buf, len);
  }
  Serial.flush();
  Serial.setTxTimeoutMs(100);
}

void hapticMeterPoll() {
  int req = sMeterReq;
  if (req == -3) return;
  if (hapticBusy()) return;                  // let the previous effect finish
  if (sMeterAll) {
    runMeter(req);
    if (req + 1 < (int)Haptic::Count) { sMeterReq = req + 1; return; }
    sMeterAll = false;
    Serial.println("HM done");
    sMeterReq = -3;
    return;
  }
  sMeterReq = -3;
  runMeter(req);
  Serial.println("HM done");
}

bool hapticConsole(const char *line) {
  if (strncmp(line, "hap", 3) != 0 || (line[3] && line[3] != ' ')) return false;
  const char *arg = line[3] ? line + 4 : "";
  char cmd[12] = "", name[24] = "";
  sscanf(arg, "%11s %23s", cmd, name);
  if (!strcmp(cmd, "list") || !*cmd) {
    Serial.print("effects:");
    for (int i = 0; i < (int)Haptic::Count; i++) Serial.printf(" %s", hapticEffect((Haptic)i).name);
    Serial.printf("\npwm=%lu Hz strength=%u%%  usage: hap list|roles|role NAME|play NAME|buzz I MS|raw L MS ..|meter NAME|raw|none|all|stop\n",
                  (unsigned long)hapticPwmFreq(), hapticStrengthPct());
  } else if (!strcmp(cmd, "roles")) {
    Serial.setTxTimeoutMs(1000);          // a 36-line burst outruns the 100 ms default
    for (int i = 0; i < (int)HapticRole::Count; i++) {
      HapticRole r = (HapticRole)i;
      char buf[96];
      int len = snprintf(buf, sizeof buf, "%-13s -> %-11s %s\n", hapticRoleName(r),
                         hapticEffect(hapticRoleEffect(r)).name, hapticRoleUse(r));
      Serial.write((const uint8_t *)buf, len < (int)sizeof buf ? len : (int)sizeof buf - 1);
      delay(3);                           // let the host drain the 256 B CDC ring
    }
    Serial.flush();
    Serial.setTxTimeoutMs(100);
  } else if (!strcmp(cmd, "role")) {
    for (int i = 0; i < (int)HapticRole::Count; i++)
      if (!strcasecmp(hapticRoleName((HapticRole)i), name)) {
        haptic((HapticRole)i);
        Serial.printf("hap role %s -> %s\n", hapticRoleName((HapticRole)i),
                      hapticEffect(hapticRoleEffect((HapticRole)i)).name);
        return true;
      }
    Serial.printf("hap unknown role '%s'\n", name);
  } else if (!strcmp(cmd, "play")) {
    if (!strcasecmp(name, "raw")) { hapticPlay(sRawFx); Serial.println("hap play raw"); return true; }
    int i = hapticFind(name);
    if (i < 0) { Serial.printf("hap unknown '%s'\n", name); return true; }
    hapticPlay((Haptic)i);
    Serial.printf("hap play %s\n", hapticEffect((Haptic)i).name);
  } else if (!strcmp(cmd, "buzz")) {
    int in = 0, ms = 0;
    sscanf(arg, "%*s %d %d", &in, &ms);
    hapticBuzz((uint8_t)constrain(in, 0, 255), (uint16_t)constrain(ms, 0, 2000));
    Serial.printf("hap buzz %d %d\n", in, ms);
  } else if (!strcmp(cmd, "raw")) {
    const char *p = arg + 3;
    uint8_t n = 0;
    int l, ms, used;
    while (n < 16 && sscanf(p, "%d %d%n", &l, &ms, &used) == 2) {
      sRaw[n++] = { (int8_t)constrain(l, -127, 127), (int8_t)constrain(l, -127, 127),
                    (uint16_t)constrain(ms, 0, 2000) };
      p += used;
    }
    sRawFx.count = n;
    Serial.printf("hap raw %u steps (play with `hap play raw` / `hap meter raw`)\n", n);
  } else if (!strcmp(cmd, "meter")) {
    ewsimEnsureAwake();          // screen-off light sleep parks the I/O task
    if (!strcasecmp(name, "all"))  { sMeterAll = true; sMeterReq = 0; }
    else if (!strcasecmp(name, "none")) sMeterReq = -2;
    else if (!strcasecmp(name, "raw"))  sMeterReq = -1;
    else {
      int i = hapticFind(name);
      if (i < 0) { Serial.printf("hap unknown '%s'\n", name); return true; }
      sMeterReq = i;
    }
    Serial.println("hap meter queued (I/O task)");
  } else if (!strcmp(cmd, "stop")) {
    hapticStop();
    Serial.println("hap stop");
  } else {
    Serial.println("usage: hap list|roles|role NAME|play NAME|buzz I MS|raw L MS ..|meter NAME|raw|none|all|stop");
  }
  return true;
}
