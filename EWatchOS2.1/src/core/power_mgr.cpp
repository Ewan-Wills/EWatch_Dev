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
#include "bluetooth.h"   // btRemoteOn(): no real light sleep while BLE is up
#if EW_APP_MOUSE
#include "btmouse.h"      // mouseConnected(): same, while a BLE mouse is linked
#endif  // EW_APP_MOUSE
#if EW_APP_WAKESOAK
#include "wake_soak.h"
#include "wake_gest.h"
#endif  // EW_APP_WAKESOAK
#include "power_mgr.h"
#if EW_APP_WRITE
#include "write_text.h"
#endif  // EW_APP_WRITE
#include "ewsim.h"

// ---------------------------------------------------------------------------
// Config + state
// ---------------------------------------------------------------------------
static PowerConfig sCfg = { 8, 15, 45, 300 };
static PowerState  sState = PowerState::Active;
static uint32_t    sLastActivity = 0;
static uint32_t    sStateEnterMs = 0;
static bool        sSelfTestReq = false;
static bool        sFifoTestReq = false;   // `pwr fifotest`
static int8_t      sRaiseTestReq = -1;   // `pwr raisetest [fresh]`: 0 = keep baseline, 1 = fresh
static bool        sSafeMode = false;   // crash-loop give-up: never sleep
static uint16_t    sDeepTestSec = 0;   // RAM only: deep sleep on USB (bench test)
static bool        sSleepReq = false;   // powerRequestSleep(): skip the timeout

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
  // USB host present (SOF packets), NOT HWCDC's `connected` flag: that
  // drops when nobody has the port open and the TX ring stalls, which sent
  // the watch into real light sleep (serial dead) between test commands.
  return HWCDC::isPlugged();
#endif
}

// ---------------------------------------------------------------------------
// CPU frequency governor — 80 MHz base, 240 MHz only on demand.
// ---------------------------------------------------------------------------
static bool sPerf240 = false;
static bool sPerfPinned = false;

void powerPerfDemand(bool need240) {
  if (sPerfPinned) return;          // BLE up: never retune the clock under it
  if (need240 == sPerf240) return;
  sPerf240 = need240;
  setCpuFrequencyMhz(need240 ? 240 : 80);
  EWLOGI("SYS", "cpu_mhz=%d", need240 ? 240 : 80);
}

// Pin the clock at 240 MHz while the BT controller runs, so the governor
// never retunes it under a live controller (precaution; not proven harmful).
void powerPerfPin(bool pin) {
  if (pin) { powerPerfDemand(true); sPerfPinned = true; }
  else     { sPerfPinned = false; }
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

// Screens that must never dim / turn off (user is reading or playing them).
// Games run their own frame loops and starve powerTick anyway while live, but
// menu/game-over screens DO return between frames — without this they'd dim
// mid-session. Torch is explicitly "screen stays on until I leave".
static bool screenBlocksSleep(Screen s) {
  switch (s) {
    case Screen::PowerOff:
#if EW_APP_WAKESOAK
    case Screen::WakeSoak:     // soak prompt / stats: the wearer is answering
#endif  // EW_APP_WAKESOAK
    case Screen::QRCode:
    case Screen::Media:
#if defined(EW_APP_SPOTIFY) && EW_APP_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
    case Screen::Spotify:      // now-playing remote: user is watching it
#endif
    case Screen::Torch:
    case Screen::Bluetooth:    // light sleep would stop the BLE scan
#if EW_APP_BOOKS
    case Screen::BookLibrary:  // user is browsing books
    case Screen::BookReader:   // user is reading
#endif  // EW_APP_BOOKS
#if EW_APP_USBHOST
    case Screen::UsbHost:      // ...and the USB host stack (no serial to wake us)
#endif  // EW_APP_USBHOST
#if EW_APP_MOTION
    case Screen::Motion:       // live motion-LM view: user is watching it
#endif  // EW_APP_MOTION
#if EW_APP_BLECONTROLS
    case Screen::BleControls:  // ...and drop the cloud panel's link
#endif  // EW_APP_BLECONTROLS
#if EW_APP_SCREENSHARE
    case Screen::Screenshare:  // live USB stream: redraws with no touch input
#endif  // EW_APP_SCREENSHARE
#if EW_APP_HIDCTL
    case Screen::HidCtl:       // Notes: serial typing isn't touch "activity"
#endif  // EW_APP_HIDCTL
#if EW_APP_FLYPET
    case Screen::FlyPet:       // the pet animates continuously: keep it lit
#endif  // EW_APP_FLYPET
#if EW_APP_LLMTERM
    case Screen::LlmTerm:      // tokens stream in over USB with no touch activity
#endif  // EW_APP_LLMTERM
#if EW_APP_PRINTSTREAM
    case Screen::PrintStream:  // live camera + stats: keep the display lit
#endif  // EW_APP_PRINTSTREAM
#if EW_APP_STOCKS
    case Screen::Stocks:       // live feed from PC: stay lit while receiving
#endif  // EW_APP_STOCKS
#if EW_APP_DOOM
    case Screen::Doom:
#endif  // EW_APP_DOOM
    case Screen::TunnelRacer:
    case Screen::Starfox:
#if EW_APP_RACER
    case Screen::Racer:
#endif  // EW_APP_RACER
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
    case Screen::Island:
#endif
      return true;
#if EW_APP_WRITE
    case Screen::Write:        // awake while someone is writing (< 60 s idle)
      return writeKeepAwake();
#endif  // EW_APP_WRITE
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Fast wake path. Order is deliberate (§2 of the spec): panel RAM is intact,
// so draw first, DISPON second, and bring the backlight up LAST — perceived
// latency is the time to backlight.
// ---------------------------------------------------------------------------
// Speculative render for motion wakes: draw the face into panel RAM while the
// screen is still DISPOFF and the raise check is still deciding, so an
// accepted raise only needs DISPON (the 72 ms render is off the critical
// path). A rejected raise costs the render and nothing visible.
static int8_t sPreRenderMin = -1;          // minute drawn, -1 = none
static void preRenderRtc() {                // I2C: before the check task starts
  uint8_t h, m, s, wd, dy, mo; uint16_t yr;
  if (readRTC(h, m, s, wd, dy, mo, yr)) {
    ModelLock lk;
    model.hour = h; model.minute = m; model.second = s;
    model.weekday = wd; model.day = dy; model.month = mo; model.year = yr;
    model.rtcOk = true; model.revision++;
  }
}
static void preRenderFace() {               // SPI only: safe alongside the check
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  if (scr == Screen::ScreenSaver) switchTo(Screen::Watch);
  if (currentView) currentView->render();
  { ModelLock lk; sPreRenderMin = (int8_t)model.minute; }
}

static void fastWake(const char *cause, int64_t tWakeUs, bool fromRealSleep,
                     bool touchValid = false,
                     uint16_t touchX = 0, uint16_t touchY = 0) {
  // Fresh RTC time straight off the bus (the I/O task is still suspended and
  // won't refresh the model for another cycle) so the face never flashes a
  // stale time.
  int64_t tA = esp_timer_get_time();
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
  // Note: switchTo() does NOT paint (the Aurora face's onEnter draws nothing),
  // so render explicitly in BOTH branches — DISPON below must reveal a fresh
  // frame, never the stale screensaver logo left in panel RAM.
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  if (scr == Screen::ScreenSaver) switchTo(Screen::Watch);
  int64_t tB = esp_timer_get_time();
  bool fresh;
  { ModelLock lk; fresh = sPreRenderMin >= 0 && sPreRenderMin == (int8_t)model.minute; }
  sPreRenderMin = -1;
  if (!fresh && currentView) currentView->render();   // pre-rendered: already in panel RAM
  int64_t tC = esp_timer_get_time();

  panelDispOn();
  // Light first, log after: each log line is a synchronous flash append
  // (tens of ms) and used to sit between DISPON and the backlight.
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  backlightSet(br);                            // instant: backlightFadeTo logs (flash) first
  int64_t tVisibleUs = esp_timer_get_time();   // backlight up = what the eye sees
  EWLOGD("WAKE", "stages pre=%ld rtc=%ld render=%ld dispon+bl=%ld (ms)",
         (long)((tA - tWakeUs) / 1000), (long)((tB - tA) / 1000),
         (long)((tC - tB) / 1000), (long)((tVisibleUs - tC) / 1000));
  uint32_t wakeMs = (uint32_t)((tVisibleUs - tWakeUs) / 1000);

  sWakeCount++;
  sWakeMsSum += wakeMs;
  if (wakeMs > sWakeMsMax) sWakeMsMax = wakeMs;
  EWLOGI("WAKE", "cause=%s wake_ms=%lu real_sleep=%d",
         cause, (unsigned long)wakeMs, fromRealSleep ? 1 : 0);

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

// Whether raise-to-wake / touch-wake participate this screen-off phase
// (Settings -> Sleep "Wake on" toggles; raise also needs a threshold > 0).
static bool sRaiseWakeEnabled = true;
static bool sTouchWakeEnabled = true;

// Touch detection here MUST use the ISR flag, not the pin level: the CST816S
// signals events as ~150 µs low pulses on INT. A 20 ms poll loop essentially
// never samples during the pulse (that was "touch won't wake it" in dev
// mode), but the edge interrupt latches touchPending every time. The level
// check stays as a fallback for a finger currently resting on the glass.
static int classifyGpioWake() {
  if (digitalRead(PIN_BTN))               return WK_BUTTON;
  if (ewsimTakeWake())                    return WK_BUTTON;   // host (ewsim)
#if EW_APP_MOUSE
  if (mouseTakeWake())                    return WK_BUTTON;   // BLE mouse moved
#endif  // EW_APP_MOUSE
  if (touchPending || digitalRead(PIN_TOUCH_INT) == LOW) {
    touchPending = false;
    if (sTouchWakeEnabled) return WK_TOUCH;
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

// ---- raise check --------------------------------------------------------
// v5 (2026-10-09), tuned on the labelled soak (62 motion triggers, 64 traces,
// soak_logs/wakegest-20261009-085029.csv): "facing" alone did not separate
// glances from everyday movement (the screen is face-up-ish much of the
// day), and a 10 deg turn from the screen-off pose let 48 of 50 unwanted
// triggers through. The TURN is now measured from where the wrist was
// ~640 ms BEFORE the trigger (MMA FIFO history, armed at screen-off), and
// must be >= 25 deg, ending facing >= 0.8 g, settled 30 ms. Replaying the
// labelled traces through this logic: unwanted woken 7/60 (v4: 48/50); the
// one replayable confirmed glance (a small tilt from already face-up, held
// still) wakes. Only 2 positives exist, so this is a best attempt pending
// more glance data. Falls back to the screen-off
// pose when the FIFO has too little history.
// The MMA motion INT only says "something moved over the threshold". The
// wake soak (2026-10-08, 2.4 h worn) showed 63 of 69 such wakes unwanted —
// walking, typing, reaching. So a motion wake now has to look like a glance:
// within kRaiseWindowMs the watch must SETTLE (kRaiseSteadyN steady samples)
// with the screen TILTED UP toward the user, and that settled orientation must
// differ from the last settled one (sBase*) by kRaiseTurnDeg, so a bump on a
// face-up desk doesn't count. The screen stays dark while this runs.
// MMA8451 raw: 4096 = 1 g; screen face up = az NEGATIVE (verified on a desk).
static const uint32_t kRaiseWindowMs = 1500;
static const uint32_t kRaiseSampleMs = 5;     // MMA at 200 Hz during the check
static const int      kRaiseSteadyN  = 6;      // x 5 ms = settled for 30 ms
static const int      kRaiseJitter   = 600;    // |dx|+|dy|+|dz| per 5 ms sample (~0.15 g)
// Facing: gravity along the direction halfway between "screen up" (-z) and
// "screen upright, top edge up" (-y): (-ay - az)/sqrt2 >= 0.8 g. That cone
// (~37 deg) spans flat face-up through held upright facing the eyes (the
// live glances were ay -0.93 g, az -0.25 g), and excludes face down, upside
// down, and the screen turned sideways (arm hanging).
static const int      kRaiseFacing   = 4634;   // 0.8 g * 4096 * sqrt2
static const float    kRaiseTurnCos  = 0.906f; // cos 25 deg
enum { RAISE_OK = 0, RAISE_NOT_FACING, RAISE_NOT_STEADY, RAISE_NO_TURN, RAISE_NO_IMU };
static const char *const kRaiseWhy[] = { "ok", "not_facing", "not_steady", "no_turn", "no_imu" };
struct RaiseResult { bool ok; uint8_t why; int16_t ax, ay, az; uint16_t ms; };
static bool    sRaiseCheck = true;                // `pwr set rchk 0|1` (RAM only)
static volatile bool sRaiseAsync = false;          // check running on the helper task
static uint16_t sRaiseReads = 0, sRaiseNew = 0;   // last check: reads, reads that differed
static bool    sBaseOk = false;
static int16_t sBaseX = 0, sBaseY = 0, sBaseZ = 0;

static void raiseSetBaseline() {
  int16_t x, y, z;
  sBaseOk = readAccel(x, y, z);
  if (sBaseOk) { sBaseX = x; sBaseY = y; sBaseZ = z; }
}

static bool raiseTurned(int16_t x, int16_t y, int16_t z) {
  if (!sBaseOk) return true;
  float a = sqrtf((float)x * x + (float)y * y + (float)z * z);
  float b = sqrtf((float)sBaseX * sBaseX + (float)sBaseY * sBaseY + (float)sBaseZ * sBaseZ);
  if (a < 1 || b < 1) return true;
  float c = ((float)x * sBaseX + (float)y * sBaseY + (float)z * sBaseZ) / (a * b);
  return c < kRaiseTurnCos;
}

// Render task, I/O task suspended (owns the I2C bus).
static RaiseResult raiseCheckLoop(uint32_t t0);
static RaiseResult raiseCheck(uint32_t t0) {
  mmaSetCheckRate(true);
  RaiseResult r = raiseCheckLoop(t0);
  mmaSetCheckRate(false);
  return r;
}
// Check on core 0 (the I/O task's core; that task is suspended, so the I2C
// bus is ours) while the render task draws the face on core 1: accel on
// I2C, panel on SPI, so the two really overlap. Saves up to the 72 ms
// render on a quick glance.
struct RaiseJob { uint32_t t0; RaiseResult r; TaskHandle_t waiter; };
static void raiseTaskFn(void *arg) {
  RaiseJob *j = (RaiseJob *)arg;
  j->r = raiseCheck(j->t0);
  xTaskNotifyGive(j->waiter);
  vTaskDelete(nullptr);
}
static RaiseResult raiseCheckOverlapped(uint32_t t0, void (*work)()) {
  RaiseJob job = { t0, { false, RAISE_NOT_STEADY, 0, 0, 0, 0 }, xTaskGetCurrentTaskHandle() };
  sRaiseAsync = true;
  if (xTaskCreatePinnedToCore(raiseTaskFn, "raise", 3072, &job, 6, nullptr, 0) != pdPASS) {
    sRaiseAsync = false;                   // no task: do it in line
    work();
    return raiseCheck(t0);
  }
  work();
  while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) esp_task_wdt_reset();
  sRaiseAsync = false;
  return job.r;
}

static RaiseResult raiseCheckLoop(uint32_t t0) {
  RaiseResult r = { false, RAISE_NOT_STEADY, 0, 0, 0, 0 };
  sRaiseReads = sRaiseNew = 0;
  int16_t px = 0, py = 0, pz = 0;
  bool have = false, settled = false;
  int steady = 0;
  int16_t vx = 0, vy = 0, vz = 0;
  while (millis() - t0 < kRaiseWindowMs) {
    if (!sRaiseAsync) esp_task_wdt_reset();   // the helper task isn't WDT-subscribed
    int16_t x, y, z;
    if (!readAccel(x, y, z)) {               // no IMU data: old behaviour (wake)
      r.ok = true; r.why = RAISE_NO_IMU; break;
    }
    // The first read after the rate switch is stale (an old FIFO/OUT value:
    // the soak traces showed it repeating a sample from up to 640 ms back).
    if (sRaiseReads++ == 0) { vTaskDelay(pdMS_TO_TICKS(kRaiseSampleMs)); continue; }
    r.ax = x; r.ay = y; r.az = z;
#if EW_APP_WAKESOAK
    gestCheckSample(x, y, z);               // wake soak trace (100 Hz kept)
#endif  // EW_APP_WAKESOAK
    if (!have || x != px || y != py || z != pz) sRaiseNew++;
    if (have) {
      int d = abs(x - px) + abs(y - py) + abs(z - pz);
      int32_t m2 = (int32_t)x * x + (int32_t)y * y + (int32_t)z * z;
      bool calm = d < kRaiseJitter && m2 > 2458L * 2458L && m2 < 5734L * 5734L;   // 0.6-1.4 g
      steady = calm ? steady + 1 : 0;
    }
    px = x; py = y; pz = z; have = true;
    if (steady >= kRaiseSteadyN) {
      settled = true;
      vx = x; vy = y; vz = z;                // the pose the verdict is about
      if (-(int)y - (int)z < kRaiseFacing) r.why = RAISE_NOT_FACING;
      else if (!raiseTurned(x, y, z)) r.why = RAISE_NO_TURN;
      else { r.ok = true; r.why = RAISE_OK; break; }
      steady = 0;                            // keep watching: may still be mid-raise
    }
    vTaskDelay(pdMS_TO_TICKS(kRaiseSampleMs));
  }
  r.ms = (uint16_t)(millis() - t0);
  if (settled) { r.ax = vx; r.ay = vy; r.az = vz; }   // else: last raw sample
  if (!r.ok && settled) { sBaseOk = true; sBaseX = vx; sBaseY = vy; sBaseZ = vz; }
  return r;
}

// STANDBY -> ACTIVE (needed to arm the FIFO) restarts the MMA's high-pass
// filter, and that step registers as a TRANSIENT: the first label-mode build
// woke itself ~40 ms after every screen-off. Let the filter settle, then
// drop the latched event.
static void fifoArmQuiet() {
  mmaFifoArm(true);
  vTaskDelay(pdMS_TO_TICKS(150));
  mmaClearMotionLatch();
}

static void enterScreenOff() {
  setState(PowerState::ScreenOff, "idle");
  sLightSleepEntries++;

  uint8_t raiseThr; bool wkTouch, wkImu; uint16_t toDeepSec;
  { ModelLock lk; raiseThr = model.motionWakeThreshold;
    wkTouch = model.wakeOnTouch; wkImu = model.wakeOnImu; toDeepSec = model.sleepToOffSec; }
  bool raiseWake = wkImu && raiseThr > 0;      // "Wake on IMU" ticked AND a threshold
  sRaiseWakeEnabled = raiseWake;
  sTouchWakeEnabled = wkTouch;                 // "Wake on Touch"
  const uint32_t offStartMs = millis();
  // Deep sleep looks like a crash (USB port gone, wake = reboot), so: never
  // while USB is attached, and never sooner than 60 s of screen-off.
  if (toDeepSec > 0 && toDeepSec < 60) toDeepSec = 60;
  if (powerDevMode()) toDeepSec = 0;
  if (sDeepTestSec) { toDeepSec = sDeepTestSec; sDeepTestSec = 0; }   // one shot

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
  if (wkTouch) gpio_wakeup_enable((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
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
  EWLOGI("PWR", "sleep=%s raise_wake=%d touch_wake=%d to_deep_s=%u", dev ? "simulated" : "light",
         raiseWake ? 1 : 0, wkTouch ? 1 : 0, toDeepSec);

  int wake = WK_NONE;
  int64_t tWakeUs = 0;
  bool     touchValid = false;
  uint16_t touchWX = 0, touchWY = 0;
#if EW_APP_WAKESOAK
  int16_t  soakAx = 0, soakAy = 0, soakAz = 0;   // wake soak record
  uint16_t soakHeldMs = 0; uint8_t soakG = 0, soakPts = 0;
  RaiseResult wakeRaise = { false, 0, 0, 0, 0, 0 }; bool haveRaise = false;
  uint32_t soakTraceId = 0;
  uint8_t wakeSense = 0;
#else
  uint16_t wakeHeldMs = 0; uint8_t wakeG = 0, wakePts = 0;   // touch wake, logged after DISPON
  RaiseResult wakeRaise = { false, 0, 0, 0, 0, 0 }; bool haveRaise = false;
  uint8_t wakeSense = 0;
#endif  // EW_APP_WAKESOAK

  raiseSetBaseline();                 // raise check: where the wrist is now
#if EW_APP_WAKESOAK
  // Wake soak: keep the last ~640 ms of wrist motion in the MMA FIFO so each
  // wake's trace starts BEFORE its trigger (after the baseline read: with the
  // FIFO on, OUT regs return FIFO entries).
  const bool soakFifo = soakEnabled();
  const bool fifoOn = soakFifo || (raiseWake && sRaiseCheck);   // raise check needs the history
#else
  // Raise check: keep the last ~640 ms of wrist motion in the MMA FIFO, the
  // turn reference (after the baseline read: with the FIFO on, OUT regs
  // return FIFO entries).
  const bool fifoOn = raiseWake && sRaiseCheck;
#endif  // EW_APP_WAKESOAK
  if (fifoOn) fifoArmQuiet();

  for (;;) {
    esp_task_wdt_reset();

    // Timer horizon: the sooner of the deep-sleep deadline and the next
    // WiFi sync window (if the radio has one scheduled).
    // Deep sleep after "Sleep -> power off" seconds of screen-off (0 = never).
    uint32_t now = millis();
    uint32_t sinceOff = now - offStartMs;
    uint32_t deepIn = toDeepSec == 0 ? 0xFFFFFFFFu
                    : (sinceOff >= (uint32_t)toDeepSec * 1000UL ? 1 : (uint32_t)toDeepSec * 1000UL - sinceOff);
    uint32_t horizon = deepIn;
    uint32_t syncIn = wifiSvcNextWindowInMs();
    if (syncIn < horizon) horizon = syncIn;
    if (horizon < 50) horizon = 50;

    // Real light sleep would kill an active radio (softAP beacons / an open
    // sync window). While the radio is busy, fall back to the polled wait —
    // screen stays dark either way and the radio finishes its work.
    // Same for the BLE remote: bluedroid isn't set up for light-sleep modem
    // timing, so it would drop the phone's link.
#if EW_APP_MOUSE
    // ...and a connected BLE mouse: bluedroid isn't set up for light-sleep
    // modem timing and would drop the link.
    if (dev || wifiSvcRadioActive() || btRemoteOn() || mouseConnected()) {
#else
    if (dev || wifiSvcRadioActive() || btRemoteOn()) {
#endif  // EW_APP_MOUSE
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
      if (toDeepSec > 0 && millis() - offStartMs >= (uint32_t)toDeepSec * 1000UL) {
        // Deep-sleep deadline. enterDeepSleep() does full shutdown; the
        // wake from there is a reboot.
        setState(PowerState::Deep, "no_motion_deadline");
        // Disarm the light-sleep wake set first. Left armed, the GPIO wake
        // carried into deep sleep and the RV-3028 minute tick (RTC INT, low
        // level) rebooted the watch every minute (wake cause 7 = GPIO).
        rtcMinuteTick(false);
        gpio_wakeup_disable((gpio_num_t)PIN_TOUCH_INT);
        gpio_wakeup_disable((gpio_num_t)PIN_BTN);
        gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT2);
        gpio_wakeup_disable((gpio_num_t)PIN_RTC_INT);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
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
      if (sRaiseCheck) {
        uint32_t t0 = millis();
#if EW_APP_WAKESOAK
        uint32_t gid = gestBegin(GEST_SRC_MOTION);
#endif  // EW_APP_WAKESOAK
        if (fifoOn) {                      // pre-trigger history, before the rate switch
          int16_t pre[32][3];
          int n = mmaFifoRead(pre, 32);
          if (n >= 8) {                    // turn reference: the oldest ~100 ms
            int32_t sx = 0, sy = 0, sz = 0;
            for (int i = 0; i < 5; i++) { sx += pre[i][0]; sy += pre[i][1]; sz += pre[i][2]; }
            sBaseX = sx / 5; sBaseY = sy / 5; sBaseZ = sz / 5; sBaseOk = true;
          }
#if EW_APP_WAKESOAK
          gestPre(pre, n);
          if (sBaseOk) gestBase(sBaseX, sBaseY, sBaseZ);
#endif  // EW_APP_WAKESOAK
        }
        preRenderRtc();
        RaiseResult rr = raiseCheckOverlapped(t0, preRenderFace);   // FIFO off from here
#if EW_APP_WAKESOAK
        gestVerdict(rr.why, rr.ok, rr.ms);
#endif  // EW_APP_WAKESOAK
        wakeRaise = rr; haveRaise = true;  // logged after the screen is on
#if EW_APP_WAKESOAK
        soakTraceId = gid;
        // Label mode: wake and ask even when the check said no, so every
        // trigger gets a ground-truth answer (the verdict is in the row).
        if (!rr.ok && !soakLabelMode()) {
#else
        if (!rr.ok) {
#endif  // EW_APP_WAKESOAK
          EWLOGD("PWR", "raise %s ms=%u a=%d,%d,%d", kRaiseWhy[rr.why], rr.ms, rr.ax, rr.ay, rr.az);
          sPreRenderMin = -1;
          sSpuriousWakes++;
#if EW_APP_WAKESOAK
          SoakWake r = { "sleep_motion_rejected", millis() - offStartMs, rr.why, 0, 0, 0,
                         rr.ms, rr.ax, rr.ay, rr.az, gid };
          soakLogRejected(r);              // writes the trace too (answer known)
#endif  // EW_APP_WAKESOAK
          if (fifoOn) fifoArmQuiet();      // history for the next trigger
          else { vTaskDelay(pdMS_TO_TICKS(150)); mmaClearMotionLatch(); }   // rate switch = HPF step
          continue;
        }
#if EW_APP_WAKESOAK
        soakAx = rr.ax; soakAy = rr.ay; soakAz = rr.az;
        soakG = rr.why; soakHeldMs = rr.ms;
      } else {
        readAccel(soakAx, soakAy, soakAz);   // for the wake soak log
      }
#else
      }
#endif  // EW_APP_WAKESOAK
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
      // Out of the DARK screen a touch must also be held (Settings -> Wake,
      // "Sleep touch hold", default 0 = off): an optional filter for brushes.
      // The screensaver keeps its own, lighter dwell.
      uint16_t needMs;
      { ModelLock lk; needMs = model.touchSleepHoldMs; }
      uint32_t heldMs = 0;
      if (needMs > 0) {
        uint32_t t0 = millis();
        bool down = pts > 0;
        int misses = down ? 0 : 1;
        while (millis() - t0 < needMs) {
          esp_task_wdt_reset();
          vTaskDelay(pdMS_TO_TICKS(20));
          uint8_t g2 = 0, p2 = 0; uint16_t x2, y2;
          bool ok2 = touchReadEventDirect(g2, p2, x2, y2);
          if (ok2 && p2 > 0) { down = true; misses = 0; }
          else if (++misses >= 2) { down = false; break; }   // allow one dropped frame
        }
        heldMs = millis() - t0;
        if (!down || heldMs < needMs) {
          sSpuriousWakes++;
          EWLOGD("PWR", "short_touch held_ms=%lu need=%u", (unsigned long)heldMs, needMs);
#if EW_APP_WAKESOAK
          SoakWake r = { "sleep_touch_rejected", millis() - offStartMs, g, pts, touchWX, touchWY,
                         (uint16_t)heldMs, 0, 0, 0 };
          soakLogRejected(r);
#endif  // EW_APP_WAKESOAK
          vTaskDelay(pdMS_TO_TICKS(30));
          continue;
        }
      }
#if EW_APP_WAKESOAK
      soakHeldMs = (uint16_t)heldMs; soakG = g; soakPts = pts;
#else
      wakeHeldMs = (uint16_t)heldMs; wakeG = g; wakePts = pts;
#endif  // EW_APP_WAKESOAK
      wakeSense = sense;                   // wake_read is logged after DISPON
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
#if EW_APP_WAKESOAK
  uint32_t offMs = millis() - offStartMs;
#endif  // EW_APP_WAKESOAK
  fastWake(cause, tWakeUs, !dev, touchValid, touchWX, touchWY);
  // Every log line is a synchronous LittleFS append (tens of ms), so the
  // wake path's own logs wait until the screen is already on.
  if (haveRaise && wake == WK_MOTION)
    EWLOGD("PWR", "raise %s ms=%u a=%d,%d,%d", kRaiseWhy[wakeRaise.why], wakeRaise.ms,
           wakeRaise.ax, wakeRaise.ay, wakeRaise.az);
#if EW_APP_WAKESOAK
  // Wake soak trace. Touch/button: start it now and take the FIFO history
  // (the trigger was moments ago; done after DISPON so it costs no latency).
  if (soakFifo && wake != WK_MOTION) {
    soakTraceId = gestBegin(wake == WK_TOUCH ? GEST_SRC_TOUCH : GEST_SRC_BUTTON);
    if (soakTraceId) { int16_t pre[32][3]; gestPre(pre, mmaFifoRead(pre, 32)); }
  }
#endif  // EW_APP_WAKESOAK
  if (fifoOn) mmaFifoArm(false);           // awake: OUT regs must be live again
#if EW_APP_WAKESOAK
  if (soakFifo) gestPostStart();           // the I/O task feeds ~1 s of post-wake motion
#endif  // EW_APP_WAKESOAK
  if (wake == WK_TOUCH)
    EWLOGD("TOUCH", "wake_read sense=%u gesture=%u pts=%u x=%u y=%u held=%u",
#if EW_APP_WAKESOAK
           wakeSense, soakG, soakPts, touchWX, touchWY, soakHeldMs);
#else
           wakeSense, wakeG, wakePts, touchWX, touchWY, wakeHeldMs);
#endif  // EW_APP_WAKESOAK
#if EW_APP_WAKESOAK
  {
    SoakWake r = { wake == WK_BUTTON ? "sleep_button" : wake == WK_TOUCH ? "sleep_touch" : "sleep_motion",
                   offMs, soakG, soakPts, touchWX, touchWY, soakHeldMs, soakAx, soakAy, soakAz,
                   soakTraceId };
    soakOnWake(r, wake != WK_BUTTON);  // button = always intentional: logged, no prompt
  }
#endif  // EW_APP_WAKESOAK

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
void powerRequestRaiseTest(bool fresh) { sRaiseTestReq = fresh ? 1 : 0; }
void powerRequestFifoTest() { sFifoTestReq = true; }
void powerRequestSleep()    { sSleepReq = true; }

// ---------------------------------------------------------------------------
// Tick — called once per render-task loop.
// ---------------------------------------------------------------------------
void powerTick() {
  // Safe mode: the idle progression is entirely disabled — a crash loop got
  // us here, and the priority is a screen that stays on + serial that stays
  // reachable, not battery life.
  if (sSafeMode) { sSleepReq = false; return; }

  // Explicit request (button on the watch face): straight to screen-off,
  // even mid sync window — the off phase already keeps a busy radio alive.
  if (sSleepReq) {
    sSleepReq = false;
    enterScreenOff();                 // blocks until user wake (or deep sleep)
    return;
  }

  if (sFifoTestReq) {                 // bench: FIFO history read-back
    sFifoTestReq = false;
    controllerSuspendIo();
    mmaFifoArm(true);
    delay(800);                       // > 32 samples at 50 Hz: FIFO full + wrapped
    int16_t buf[32][3];
    int n = mmaFifoRead(buf, 32);
    int n2 = mmaFifoRead(buf + 0, 0); // drained: count must now be ~0
    mmaFifoArm(false);
    int16_t lx, ly, lz; bool live = readAccel(lx, ly, lz);
    controllerResumeIo();
    int distinct = 0;
    for (int i = 1; i < n; i++)
      if (buf[i][0] != buf[i-1][0] || buf[i][1] != buf[i-1][1] || buf[i][2] != buf[i-1][2]) distinct++;
    Serial.printf("fifotest n=%d first=%d,%d,%d last=%d,%d,%d distinct=%d live=%d,%d,%d ok=%d\n",
                  n, n ? buf[0][0] : 0, n ? buf[0][1] : 0, n ? buf[0][2] : 0,
                  n ? buf[n-1][0] : 0, n ? buf[n-1][1] : 0, n ? buf[n-1][2] : 0,
                  distinct, lx, ly, lz, live ? 1 : 0);
    (void)n2;
    return;
  }
  if (sRaiseTestReq >= 0) {           // bench: run the raise check on the live sensor
    bool fresh = sRaiseTestReq == 1;
    sRaiseTestReq = -1;
    controllerSuspendIo();
    if (fresh) sBaseOk = false; else raiseSetBaseline();
    RaiseResult rr = raiseCheck(millis());
    controllerResumeIo();
    Serial.printf("raisetest %s ok=%d ms=%u a=%d,%d,%d base=%d,%d,%d reads=%u new=%u\n", kRaiseWhy[rr.why],
                  rr.ok ? 1 : 0, rr.ms, rr.ax, rr.ay, rr.az, sBaseX, sBaseY, sBaseZ, sRaiseReads, sRaiseNew);
    return;
  }

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
  } else if (strcmp(key, "rchk") == 0) {       // raise check on/off (RAM only)
    sRaiseCheck = val != 0;
  } else if (strcmp(key, "dtest") == 0) {   // not saved: next screen-off
    sDeepTestSec = val;                          // deep-sleeps after val s, even on USB
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
