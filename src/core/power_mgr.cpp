#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include "pins.h"
#include "display.h"
#include "touch.h"
#include "model.h"
#include "event.h"
#include "view.h"
#include "controller.h"
#include "wifi_svc.h"
#include "ewlog.h"
#include "power_mgr.h"

// ---------------------------------------------------------------------------
// Config + state
// ---------------------------------------------------------------------------
static PowerConfig sCfg = { 8, 15, 45, 300 };
static PowerState  sState = PowerState::Active;
static uint32_t    sLastActivity = 0;
static uint32_t    sStateEnterMs = 0;
static bool        sSelfTestReq = false;
static bool        sSafeMode = false;   // crash-loop give-up: never sleep

void powerSetSafeMode(bool on) { sSafeMode = on; }

// Stats (acceptance evidence for `pwr stats`).
static uint64_t sTimeInState[4] = { 0, 0, 0, 0 };
static uint32_t sWakeCount = 0;
static uint64_t sWakeMsSum = 0;
static uint32_t sWakeMsMax = 0;
static uint32_t sLightSleepEntries = 0;
static uint32_t sSimulatedSleeps = 0;
static uint32_t sSpuriousWakes = 0;    // wake pin asserted but no real event
static uint64_t sWifiAssocMs = 0;
static int32_t  sBatBootMv = -1;      // -1 = NA

static const char *stateName(PowerState s) {
  switch (s) {
    case PowerState::Active:    return "ACTIVE";
    case PowerState::Idle:      return "IDLE";
    case PowerState::ScreenOff: return "SCREEN_OFF";
    default:                    return "DEEP";
  }
}
static const char *stateAbbrev(PowerState s) {
  switch (s) {
    case PowerState::Active:    return "ACT";
    case PowerState::Idle:      return "IDLE";
    case PowerState::ScreenOff: return "OFF";
    default:                    return "DEEP";
  }
}

static void setState(PowerState next, const char *reason) {
  if (next == sState) return;
  uint32_t now = millis();
  uint32_t inPrev = now - sStateEnterMs;
  sTimeInState[(int)sState] += inPrev;
  EWLOGI("PWR", "state=%s prev=%s reason=%s prev_ms=%lu",
         stateName(next), stateName(sState), reason, (unsigned long)inPrev);
  sState = next;
  sStateEnterMs = now;
  ewlogSetState(stateAbbrev(next));
}

PowerState powerState() { return sState; }
const PowerConfig &powerConfig() { return sCfg; }

bool powerDevMode() {
#if defined(EW_DEV_MODE) && EW_DEV_MODE
  return true;
#else
  return (bool)Serial;      // USB CDC host attached
#endif
}

// ---------------------------------------------------------------------------
// CPU frequency governor — 80 MHz base, 240 MHz only on demand.
// ---------------------------------------------------------------------------
static bool sPerf240 = false;

void powerPerfDemand(bool need240) {
  if (need240 == sPerf240) return;
  sPerf240 = need240;
  setCpuFrequencyMhz(need240 ? 240 : 80);
  EWLOGI("SYS", "cpu_mhz=%d", need240 ? 240 : 80);
}

// ---------------------------------------------------------------------------
// RV-3028 minute-tick interrupt (periodic time update on INT, active low).
// Registers: STATUS=0x0E (UF bit5), CONTROL1=0x0F (USEL bit4: 1 = minute),
// CONTROL2=0x10 (UIE bit5). Direct Wire access — only used while the I/O
// task is suspended, so the bus is ours.
// ---------------------------------------------------------------------------
static bool rtcRead8(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 1) != 1) return false;
  val = Wire.read();
  return true;
}
static void rtcWrite8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
static void rtcMinuteTick(bool enable) {
  uint8_t c1 = 0, c2 = 0;
  if (!rtcRead8(0x0F, c1) || !rtcRead8(0x10, c2)) return;
  if (enable) { c1 |= 0x10; c2 |= 0x20; }     // USEL=minute, UIE=1
  else        { c2 = (uint8_t)(c2 & ~0x20); } // UIE=0
  rtcWrite8(0x0F, c1);
  rtcWrite8(0x10, c2);
  rtcWrite8(0x0E, 0x00);                      // clear UF so INT releases
}

// ---------------------------------------------------------------------------
// Activity + tick
// ---------------------------------------------------------------------------
void powerOnActivity() {
  sLastActivity = millis();
  if (sState == PowerState::Idle) {
    uint8_t br;
    { ModelLock lk; br = model.brightness; }
    backlightFadeTo(br, 80);
    setState(PowerState::Active, "input");
  }
}

// Screens that must never dim / turn off (user is reading them).
static bool screenBlocksSleep(Screen s) {
  return s == Screen::PowerOff || s == Screen::QRCode || s == Screen::Media;
}

// ---------------------------------------------------------------------------
// Fast wake path. Order is deliberate (§2 of the spec): panel RAM is intact,
// so draw first, DISPON second, and bring the backlight up LAST — perceived
// latency is the time to backlight.
// ---------------------------------------------------------------------------
static void fastWake(const char *cause, int64_t tWakeUs, bool fromRealSleep,
                     bool touchValid = false,
                     uint16_t touchX = 0, uint16_t touchY = 0) {
  // Fresh RTC time straight off the bus (the I/O task is still suspended and
  // won't refresh the model for another cycle) so the face never flashes a
  // stale time.
  uint8_t h, m, s, wd, dy, mo;
  uint16_t yr;
  if (readRTC(h, m, s, wd, dy, mo, yr)) {
    ModelLock lk;
    model.hour = h; model.minute = m; model.second = s;
    model.weekday = wd; model.day = dy; model.month = mo; model.year = yr;
    model.rtcOk = true;
    model.revision++;
  }

  // Screensaver has no business after a sleep — land on the watch face.
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  if (scr == Screen::ScreenSaver) switchTo(Screen::Watch);
  else if (currentView) currentView->render();

  panelDispOn();
  int64_t tVisibleUs = esp_timer_get_time();
  uint32_t wakeMs = (uint32_t)((tVisibleUs - tWakeUs) / 1000);

  sWakeCount++;
  sWakeMsSum += wakeMs;
  if (wakeMs > sWakeMsMax) sWakeMsMax = wakeMs;
  EWLOGI("WAKE", "cause=%s wake_ms=%lu real_sleep=%d",
         cause, (unsigned long)wakeMs, fromRealSleep ? 1 : 0);

  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  backlightSet(0);
  backlightFadeTo(br, 60);

  // Touch event processing AFTER the screen is visible. The event was
  // already captured (and validated) by the wake loop's single register
  // read — just deliver it.
  if (touchValid) {
    Event e = makeEvent(EventType::Touch);
    e.x = touchX; e.y = touchY;
    postEvent(e);
  }
}

// ---------------------------------------------------------------------------
// SCREEN_OFF phase: DISPOFF + light sleep in a loop. Handles background wake
// causes (RTC minute tick, WiFi sync windows, deep-sleep deadline) without
// lighting the screen; returns only when the user wakes the watch. In dev
// mode (USB attached) the sleep is simulated so serial stays alive.
// ---------------------------------------------------------------------------
enum { WK_NONE = 0, WK_BUTTON, WK_TOUCH, WK_MOTION, WK_RTC, WK_TIMER };

// Whether raise-to-wake participates this screen-off phase (threshold > 0).
static bool sRaiseWakeEnabled = true;

// Touch detection here MUST use the ISR flag, not the pin level: the CST816S
// signals events as ~150 µs low pulses on INT. A 20 ms poll loop essentially
// never samples during the pulse (that was "touch won't wake it" in dev
// mode), but the edge interrupt latches touchPending every time. The level
// check stays as a fallback for a finger currently resting on the glass.
static int classifyGpioWake() {
  if (digitalRead(PIN_BTN))               return WK_BUTTON;
  if (touchPending || digitalRead(PIN_TOUCH_INT) == LOW) {
    touchPending = false;
    return WK_TOUCH;
  }
  if (sRaiseWakeEnabled && digitalRead(PIN_MMA_INT2)) return WK_MOTION;
  if (digitalRead(PIN_RTC_INT) == LOW)    return WK_RTC;
  return WK_NONE;
}

// Simulated light sleep: poll the same wake pins + the timer deadline with
// delay(), feeding the WDT, so USB CDC keeps working for development.
static int fakeSleepWait(uint32_t maxMs) {
  uint32_t t0 = millis();
  for (;;) {
    esp_task_wdt_reset();
    int wk = classifyGpioWake();
    if (wk != WK_NONE) return wk;
    if (millis() - t0 >= maxMs) return WK_TIMER;
    delay(20);
  }
}

static void enterScreenOff() {
  setState(PowerState::ScreenOff, "idle");
  sLightSleepEntries++;

  uint8_t raiseThr;
  { ModelLock lk; raiseThr = model.motionWakeThreshold; }
  bool raiseWake = raiseThr > 0;
  sRaiseWakeEnabled = raiseWake;

  // Visual off: fade out, panel DISPOFF (RAM retained — no re-init on wake).
  backlightFadeTo(0, 150);
  panelDispOff();

  // Peripheral hygiene for the off phase.
  controllerSuspendIo();              // quiesce the I2C bus (owns WDT bookkeeping)

  // Drain every latched interrupt source BEFORE arming the wake pins,
  // otherwise a stale assertion wakes us the instant we sleep (this was the
  // "screen goes black then lights right back up" bug):
  //   * CST816S: read the current event frame so its INT releases.
  //   * MMA8451: read FF_MT_SRC to release a latched motion INT2.
  //   * RV-3028: clear the status flags so a pending UF doesn't hold INT low.
  { uint8_t g, pts; uint16_t tx, ty; touchReadEventDirect(g, pts, tx, ty); }
  touchPending = false;
  mmaClearMotionLatch();
  rtcWrite8(0x0E, 0x00);

  // NOTE: the CST816S deliberately STAYS in dynamic mode through screen-off.
  // Letting it auto-sleep looked like free power, but on this chip the sleep
  // state stops scanning entirely — a finger never raises INT and the watch
  // becomes impossible to wake by touch. The scan current is the price of a
  // touch-wakeable dark screen.
  rtcMinuteTick(true);                // minute tick for background wakes

  // Anything queued before the screen went dark is stale.
  if (eventQueue) xQueueReset(eventQueue);

  // Let the INT lines settle at their idle levels before they become wake
  // sources (mirrors the deep-sleep path's settle).
  vTaskDelay(pdMS_TO_TICKS(250));
  mmaClearMotionLatch();              // motion during the settle doesn't count

  // The touch INT pad floats between the chip's active-low pulses — without
  // a pull-up it drifts LOW and instantly "wakes" us. Belt-and-braces here
  // even though touchBegin() now leaves the pull-up on.
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

  // GPIO wake sources (light sleep, level-triggered). Raise-to-wake only
  // when the user hasn't disabled it (Settings -> Wake, threshold 0 = off).
  gpio_wakeup_enable((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN,       GPIO_INTR_HIGH_LEVEL);
  if (raiseWake) gpio_wakeup_enable((gpio_num_t)PIN_MMA_INT2, GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_RTC_INT,   GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  bool dev = powerDevMode();
  if (dev) sSimulatedSleeps++;
  // Pin snapshot right before sleeping — if anything below reads "active"
  // (btn=1 / tint=0 / mma2=1 / rtc=0) that pin is the self-wake culprit.
  EWLOGD("PWR", "off_pins btn=%d tint=%d mma2=%d rtc=%d",
         digitalRead(PIN_BTN), digitalRead(PIN_TOUCH_INT),
         digitalRead(PIN_MMA_INT2), digitalRead(PIN_RTC_INT));
  EWLOGI("PWR", "sleep=%s raise_wake=%d", dev ? "simulated" : "light",
         raiseWake ? 1 : 0);

  int wake = WK_NONE;
  int64_t tWakeUs = 0;
  bool     touchValid = false;
  uint16_t touchWX = 0, touchWY = 0;

  for (;;) {
    esp_task_wdt_reset();

    // Timer horizon: the sooner of the deep-sleep deadline and the next
    // WiFi sync window (if the radio has one scheduled).
    uint32_t now = millis();
    uint32_t deepAtMs = sLastActivity + (uint32_t)sCfg.deepMin * 60000UL;
    uint32_t horizon = (deepAtMs > now) ? (deepAtMs - now) : 1;
    uint32_t syncIn = wifiSvcNextWindowInMs();
    if (syncIn < horizon) horizon = syncIn;
    if (horizon < 50) horizon = 50;

    // Real light sleep would kill an active radio (softAP beacons / an open
    // sync window). While the radio is busy, fall back to the polled wait —
    // screen stays dark either way and the radio finishes its work.
    if (dev || wifiSvcRadioActive()) {
      wake = fakeSleepWait(horizon);
      tWakeUs = esp_timer_get_time();
    } else {
      esp_sleep_enable_timer_wakeup((uint64_t)horizon * 1000ULL);
      esp_light_sleep_start();
      tWakeUs = esp_timer_get_time();
      esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
      if (c == ESP_SLEEP_WAKEUP_GPIO) wake = classifyGpioWake();
      else if (c == ESP_SLEEP_WAKEUP_TIMER) wake = WK_TIMER;
      else wake = WK_NONE;
      if (wake == WK_NONE) wake = classifyGpioWake();   // pulse gone: ISR flag
      // A GPIO wake whose pulse ended before we sampled and whose ISR ran
      // right after resume still shows up as touchPending on the next
      // classify — handled by the loop coming back around.
    }

    // ---- background causes: service and go back to sleep ----
    if (wake == WK_RTC) {
      rtcWrite8(0x0E, 0x00);          // clear UF -> release INT
      EWLOGD("PWR", "rtc_tick");
      continue;
    }
    if (wake == WK_TIMER) {
      uint32_t idleMs = millis() - sLastActivity;
      if (idleMs >= (uint32_t)sCfg.deepMin * 60000UL) {
        // Deep-sleep deadline. enterDeepSleep() does full shutdown; the
        // wake from there is a reboot.
        setState(PowerState::Deep, "no_motion_deadline");
        enterDeepSleep();             // does not return
      }
      // Otherwise: a WiFi sync window came due. Let the radio task run its
      // window while the screen stays dark, then re-sleep.
      if (wifiSvcNextWindowInMs() == 0) {
        EWLOGD("PWR", "sync_window_service");
        wifiSvcKickWindow();
        uint32_t t0 = millis();
        while (wifiSvcRadioActive() && millis() - t0 < 90000UL) {
          esp_task_wdt_reset();
          delay(100);
        }
      }
      continue;
    }
    if (wake == WK_MOTION) {
      mmaClearMotionLatch();
      break;                          // raise-to-wake -> fast wake path
    }
    if (wake == WK_TOUCH) {
      // Touch-wake sensitivity (Settings -> Wake, "Touch wake"):
      //   0 = any:  every INT pulse wakes — no register validation at all.
      //   1 = tap:  a real event (finger down OR latched gesture) required;
      //             empty register = EMI phantom, stay asleep.
      //   2 = firm: the finger must still be down at the validation read —
      //             the strictest phantom filter, may miss ultra-quick taps.
      uint8_t sense;
      { ModelLock lk; sense = model.touchWakeSense; }
      uint8_t g = 0, pts = 0;
      bool got = touchReadEventDirect(g, pts, touchWX, touchWY);
      bool accept = (sense == 0) ||
                    (sense == 1 && got && (g != 0 || pts > 0)) ||
                    (sense >= 2 && got && pts > 0);
      if (!accept) {
        sSpuriousWakes++;
        EWLOGD("PWR", "spurious_touch got=%d g=%u pts=%u tint=%d",
               got ? 1 : 0, g, pts, digitalRead(PIN_TOUCH_INT));
        vTaskDelay(pdMS_TO_TICKS(30));    // let the INT line release
        continue;
      }
      EWLOGD("TOUCH", "wake_read sense=%u gesture=%u pts=%u x=%u y=%u",
             sense, g, pts, touchWX, touchWY);
      touchValid = pts > 0;
      break;
    }
    if (wake == WK_BUTTON) break;
    // WK_NONE (spurious GPIO wake with no pin still asserted): log + re-sleep.
    if (!dev) {
      sSpuriousWakes++;
      EWLOGD("PWR", "spurious_none btn=%d tint=%d mma2=%d rtc=%d",
             digitalRead(PIN_BTN), digitalRead(PIN_TOUCH_INT),
             digitalRead(PIN_MMA_INT2), digitalRead(PIN_RTC_INT));
    }
  }

  // ---- leaving the off phase: user wake ----
  gpio_wakeup_disable((gpio_num_t)PIN_TOUCH_INT);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN);
  gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT2);
  gpio_wakeup_disable((gpio_num_t)PIN_RTC_INT);

  rtcMinuteTick(false);
  // Bring the touch chip back to dynamic mode. On a touch wake the chip is
  // awake (the finger woke it) and this ACKs; after a button/motion wake it
  // may have auto-slept — then it NACKs everything and only a RST pulse
  // helps. Do the recovery AFTER the screen is visible (it costs ~70 ms).
  bool touchAwake = touchSetMonitorMode(false);

  const char *cause = (wake == WK_BUTTON) ? "button"
                    : (wake == WK_TOUCH)  ? "touch"
                    : "motion";
  fastWake(cause, tWakeUs, !dev, touchValid, touchWX, touchWY);

  if (!touchAwake) {
    EWLOGW("TOUCH", "wake_reinit=1 (chip slept during screen-off)");
    touchRecover();
  }

  // The wake event was already delivered; don't let the stale ISR flag
  // double-dispatch it through the I/O task's gesture drain.
  touchPending = false;

  controllerResumeIo();
  sLastActivity = millis();
  setState(PowerState::Active, cause);
}

// ---------------------------------------------------------------------------
// Self test (`pwr selftest`): exercises the real screen-off -> fast-wake
// pipeline three times with a synthetic wake, so wake_ms evidence can be
// collected at a desk over USB without physically tapping the watch.
// ---------------------------------------------------------------------------
static void runSelfTest() {
  EWLOGI("PWR", "selftest=start cycles=3");
  for (int i = 0; i < 3; i++) {
    backlightFadeTo(0, 100);
    panelDispOff();
    controllerSuspendIo();
    touchSetMonitorMode(true);
    delay(400);                       // "asleep"
    int64_t tWakeUs = esp_timer_get_time();
    touchSetMonitorMode(false);
    fastWake("selftest", tWakeUs, false);
    controllerResumeIo();
    delay(250);
  }
  sLastActivity = millis();
  EWLOGI("PWR", "selftest=done");
  powerStatsPrint(Serial);
}

void powerRequestSelfTest() { sSelfTestReq = true; }

// ---------------------------------------------------------------------------
// Tick — called once per render-task loop.
// ---------------------------------------------------------------------------
void powerTick() {
  // Safe mode: the idle progression is entirely disabled — a crash loop got
  // us here, and the priority is a screen that stays on + serial that stays
  // reachable, not battery life.
  if (sSafeMode) return;

  if (sSelfTestReq) {
    sSelfTestReq = false;
    runSelfTest();
    return;
  }

  uint16_t saverSec;
  uint8_t  brightness;
  Screen   scr;
  { ModelLock lk;
    sCfg.offSec = model.sleepTimeoutSec;      // NVS-backed, user-editable
    saverSec    = model.screensaverTimeoutSec;
    brightness  = model.brightness;
    scr         = model.screen; }

  if (screenBlocksSleep(scr)) { sLastActivity = millis(); return; }

  // A WiFi sync window in progress counts as activity: don't dim, don't
  // start the screensaver, and don't sleep out from under a connection the
  // user is likely watching (e.g. on the WiFi settings page).
  if (wifiSvcWindowActive()) { sLastActivity = millis(); return; }

  uint32_t idleMs = millis() - sLastActivity;

  // Screensaver (between dim and screen-off; only from the watch face, and
  // only if it's configured to fire before the screen turns off).
  if (saverSec > 0 && scr == Screen::Watch &&
      idleMs > (uint32_t)saverSec * 1000UL &&
      (sCfg.offSec == 0 || saverSec < sCfg.offSec)) {
    switchTo(Screen::ScreenSaver);
    scr = Screen::ScreenSaver;
  }

  switch (sState) {
    case PowerState::Active:
      if (sCfg.dimSec > 0 && idleMs > (uint32_t)sCfg.dimSec * 1000UL &&
          scr != Screen::ScreenSaver) {       // saver sets its own level
        uint8_t dim = (uint8_t)((uint16_t)brightness * 20 / 100);
        if (dim < 8) dim = 8;
        backlightFadeTo(dim, 200);
        setState(PowerState::Idle, "t_dim");
      }
      break;
    case PowerState::Idle:
      break;
    default:
      break;
  }

  if (sCfg.offSec > 0 && idleMs > (uint32_t)sCfg.offSec * 1000UL) {
    enterScreenOff();                 // blocks until user wake (or deep sleep)
  }
}

// ---------------------------------------------------------------------------
// Init / stats
// ---------------------------------------------------------------------------
void powerInit() {
  Preferences p;
  p.begin("ewatch", true);
  sCfg.dimSec  = p.getUShort("pDim",     sCfg.dimSec);
  sCfg.deepMin = p.getUShort("pDeepMin", sCfg.deepMin);
  sCfg.syncSec = p.getUShort("pSync",    sCfg.syncSec);
  p.end();
  { ModelLock lk;
    sCfg.offSec = model.sleepTimeoutSec;
#if defined(BAT_MON_SUPPORTED)
    if (model.batOk) sBatBootMv = (int32_t)(model.vbat * 1000.0f);
#endif
  }
  sLastActivity = millis();
  sStateEnterMs = millis();
  ewlogSetState("ACT");
  // 80 MHz base clock; animated views raise it on demand.
  setCpuFrequencyMhz(80);
  EWLOGI("SYS", "cpu_mhz=80 t_dim=%u t_off=%u t_deep_min=%u t_sync=%u",
         sCfg.dimSec, sCfg.offSec, sCfg.deepMin, sCfg.syncSec);
}

void powerNoteWifiAssoc(uint32_t assocMs) { sWifiAssocMs += assocMs; }

// Console: `pwr set dim|off|deep|sync <n>` — persists to NVS and applies
// immediately (spec: all timeouts NVS-overridable).
bool powerSetConfig(const char *key, uint16_t val) {
  Preferences p;
  if (strcmp(key, "dim") == 0) {
    sCfg.dimSec = val;
    p.begin("ewatch", false); p.putUShort("pDim", val); p.end();
  } else if (strcmp(key, "off") == 0) {
    { ModelLock lk; model.sleepTimeoutSec = val; }
    sCfg.offSec = val;
    p.begin("ewatch", false); p.putUShort("sleepSec", val); p.end();
  } else if (strcmp(key, "deep") == 0) {
    sCfg.deepMin = val;
    p.begin("ewatch", false); p.putUShort("pDeepMin", val); p.end();
  } else if (strcmp(key, "sync") == 0) {
    sCfg.syncSec = val;
    p.begin("ewatch", false); p.putUShort("pSync", val); p.end();
  } else {
    return false;
  }
  EWLOGI("SYS", "cfg %s=%u", key, val);
  return true;
}

void powerStatsPrint(Stream &out) {
  uint64_t inState[4];
  for (int i = 0; i < 4; i++) inState[i] = sTimeInState[i];
  inState[(int)sState] += millis() - sStateEnterMs;

  uint32_t up = millis();
  uint32_t avgWake = sWakeCount ? (uint32_t)(sWakeMsSum / sWakeCount) : 0;
  uint32_t dutyPct = up ? (uint32_t)(sWifiAssocMs * 100 / up) : 0;

  out.printf("PWR STATS uptime_ms=%lu state=%s\n",
             (unsigned long)up, stateName(sState));
  out.printf("  time_ms ACTIVE=%lu IDLE=%lu SCREEN_OFF=%lu\n",
             (unsigned long)inState[0], (unsigned long)inState[1],
             (unsigned long)inState[2]);
  out.printf("  wakes=%lu avg_wake_ms=%lu max_wake_ms=%lu "
             "lightsleep_entries=%lu simulated=%lu spurious=%lu\n",
             (unsigned long)sWakeCount, (unsigned long)avgWake,
             (unsigned long)sWakeMsMax, (unsigned long)sLightSleepEntries,
             (unsigned long)sSimulatedSleeps, (unsigned long)sSpuriousWakes);
  out.printf("  wifi_assoc_ms=%lu wifi_duty_pct=%lu\n",
             (unsigned long)sWifiAssocMs, (unsigned long)dutyPct);
#if defined(BAT_MON_SUPPORTED)
  float vbatNow; bool batOk;
  { ModelLock lk; vbatNow = model.vbat; batOk = model.batOk; }
  if (batOk && sBatBootMv >= 0) {
    int32_t nowMv = (int32_t)(vbatNow * 1000.0f);
    out.printf("  bat_boot_mv=%ld bat_now_mv=%ld delta_mv=%ld\n",
               (long)sBatBootMv, (long)nowMv, (long)(nowMv - sBatBootMv));
  } else {
    out.printf("  bat=NA (monitor not reading)\n");
  }
#else
  out.printf("  bat=NA (V1 hardware, no ADC on divider)\n");
#endif
}
