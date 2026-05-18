#include <Wire.h>
#include <esp_task_wdt.h>
#include "pins.h"
#include "power.h"
#include "i2c_bus.h"
#include "touch.h"
#include "display.h"
#include "haptic.h"
#include "controller.h"
#include "view.h"
#include "event.h"
#include "apptimer.h"

// ---------- I/O request queue (cross-task -> taskIO) ----------
struct RTCWrite {
  uint8_t  h, m, s;
  uint8_t  weekday;
  uint8_t  day, month;
  uint16_t year;
};
static QueueHandle_t rtcWriteQueue = nullptr;

void requestSetRTC(uint8_t h, uint8_t m, uint8_t s,
                   uint8_t weekday, uint8_t day, uint8_t month, uint16_t year) {
  if (!rtcWriteQueue) return;
  RTCWrite r{ h, m, s, weekday, day, month, year };
  xQueueSend(rtcWriteQueue, &r, 0);
}

// ---------- low-level I/O ----------
static uint8_t mmaAddr = 0;

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool readRTC(uint8_t &h, uint8_t &m, uint8_t &s,
             uint8_t &weekday, uint8_t &day, uint8_t &month, uint16_t &year) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 7) != 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  s       = bcd2dec(b[0] & 0x7F);
  m       = bcd2dec(b[1] & 0x7F);
  h       = bcd2dec(b[2] & 0x3F);
  weekday = b[3] & 0x07;
  day     = bcd2dec(b[4] & 0x3F);
  month   = bcd2dec(b[5] & 0x1F);
  year    = 2000 + bcd2dec(b[6]);
  return true;
}

bool writeRTC(uint8_t h, uint8_t m, uint8_t s,
              uint8_t weekday, uint8_t day, uint8_t month, uint16_t year) {
  uint8_t yy = (year >= 2000) ? (year - 2000) : 0;
  if (yy > 99) yy = 99;
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  Wire.write(dec2bcd(s)     & 0x7F);
  Wire.write(dec2bcd(m)     & 0x7F);
  Wire.write(dec2bcd(h)     & 0x3F);
  Wire.write(weekday        & 0x07);
  Wire.write(dec2bcd(day)   & 0x3F);
  Wire.write(dec2bcd(month) & 0x1F);
  Wire.write(dec2bcd(yy));
  return Wire.endTransmission() == 0;
}

static void mmaActivate() {
  if (!mmaAddr) return;
  Wire.beginTransmission(mmaAddr);
  Wire.write(0x2A); Wire.write(0x01);
  Wire.endTransmission();
}

bool readAccel(int16_t &x, int16_t &y, int16_t &z) {
  if (!mmaAddr) return false;
  Wire.beginTransmission(mmaAddr);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)mmaAddr, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  x = (int16_t)((b[0] << 8) | b[1]) >> 2;
  y = (int16_t)((b[2] << 8) | b[3]) >> 2;
  z = (int16_t)((b[4] << 8) | b[5]) >> 2;
  return true;
}

// Live "is finger on screen" poll. The CST816S library updates state only on
// ISR events, so we go straight to the chip to detect ongoing contact and
// release. Reads regs 0x02 (FingerNum) + 0x03..0x06 (XH/XL/YH/YL).
static bool readTouchHeld(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 5) != 5) return false;
  uint8_t pts = Wire.read();
  uint8_t xh  = Wire.read();
  uint8_t xl  = Wire.read();
  uint8_t yh  = Wire.read();
  uint8_t yl  = Wire.read();
  if (pts == 0) return false;
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

bool readBattery(float &volts, uint8_t &pct) {
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  delay(2);
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_BAT_MON_ADC);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  uint32_t adc_mV = acc / 8;
  if (adc_mV == 0) return false;
  volts = (adc_mV / 1000.0f) / 0.769f;
  float p = (volts - 3.30f) * (100.0f / 0.90f);
  if (p < 0) p = 0; if (p > 100) p = 100;
  pct = (uint8_t)(p + 0.5f);
  return true;
}

// ---------- init ----------
void controllerInit() {
  pinMode(PIN_BTN,        INPUT);
  pinMode(PIN_RTC_INT,    INPUT_PULLUP);
  pinMode(PIN_MMA_INT1,   INPUT);
  pinMode(PIN_MMA_INT2,   INPUT);
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);

  if      (i2cPing(I2C_ADDR_MMA8451_A)) mmaAddr = I2C_ADDR_MMA8451_A;
  else if (i2cPing(I2C_ADDR_MMA8451_B)) mmaAddr = I2C_ADDR_MMA8451_B;
  mmaActivate();

  // CST816S IrqCtl (0xFA): EnTouch + EnChange + EnMotion. Without EnChange the
  // chip stops firing IRQs while a finger is held still, which broke the
  // tap-and-hold ramp logic in Settings. Polling reg 0x02 also has to work
  // reliably for the same flow.
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0xFA);
  Wire.write(0x70);
  Wire.endTransmission();

  rtcWriteQueue = xQueueCreate(2, sizeof(RTCWrite));
}

// ---------- FreeRTOS tasks ----------

// Single I/O task — owns the I2C bus to avoid Wire-singleton races. Runs at
// 50 Hz; samples accel every cycle (for shake), button every cycle, RTC every
// 4th cycle (~12 Hz), battery every 50th cycle (~1 Hz). Drains pending RTC
// writes posted by other tasks at the start of each cycle.
static void taskIO(void *) {
  // Subscribe to the system task watchdog. If this loop ever stalls for more
  // than the WDT timeout the chip resets, and the panic-loop guard in main()
  // catches the repeat and drops the LDO latch.
  esp_task_wdt_add(nullptr);
  bool     lastBtn = false;
  uint32_t btnDownMs = 0;
  bool     veryLongFired = false;

  int16_t  pax = 0, pay = 0, paz = 0;
  uint32_t motionAcc = 0, motionWindowStart = 0, lastMotionMs = 0;

  uint32_t cycle = 0;
  bool     fingerDown = false;
  uint32_t lastHoldPostMs = 0;
  uint16_t lastTouchX = 0, lastTouchY = 0;

  for (;;) {
    esp_task_wdt_reset();

    // ---- Drain pending RTC writes from other tasks ----
    RTCWrite r;
    while (rtcWriteQueue && xQueueReceive(rtcWriteQueue, &r, 0) == pdPASS) {
      writeRTC(r.h, r.m, r.s, r.weekday, r.day, r.month, r.year);
    }

    // ---- Touch state machine ----
    // We treat the polled FingerNum register as the source of truth for the
    // press/hold/release state; the ISR is only used to fish out gesture
    // codes (which are reported alongside the press frame). This dodges the
    // CST816S quirk where the chip stops emitting interrupts while a finger
    // is held stationary — the previous design relied on those.
    uint16_t hx = 0, hy = 0;
    bool live = readTouchHeld(hx, hy);

    if (live && !fingerDown) {
      fingerDown = true;
      lastTouchX = hx; lastTouchY = hy;
      Event e = makeEvent(EventType::Touch);
      e.x = hx; e.y = hy;
      postEvent(e);
      lastHoldPostMs = millis();
    } else if (live && fingerDown) {
      lastTouchX = hx; lastTouchY = hy;
      if (millis() - lastHoldPostMs >= 30) {
        lastHoldPostMs = millis();
        Event e = makeEvent(EventType::TouchHold);
        e.x = hx; e.y = hy;
        postEvent(e);
      }
    } else if (!live && fingerDown) {
      fingerDown = false;
      Event e = makeEvent(EventType::TouchUp);
      e.x = lastTouchX; e.y = lastTouchY;
      postEvent(e);
    }

    // Drain ISR-reported gestures (only — Touch state already comes from poll).
    if (touchPending) {
      touchPending = false;
      if (touchpad.available()) {
        Gesture g = (Gesture)touchpad.data.gestureID;
        if (g != Gesture::None) {
          // Dedup: in continuous-report mode the chip can fire multiple
          // ISRs for one physical swipe. Suppress repeats within 400 ms.
          static Gesture  lastG = Gesture::None;
          static uint32_t lastGMs = 0;
          uint32_t now = millis();
          if (g != lastG || now - lastGMs > 400) {
            if (g == Gesture::SwipeRight) {
              // Global "back": every view already handles ButtonShort as the
              // back affordance, so route swipe-right through the same path
              // instead of inventing a new event type. Suppressed only in the
              // 3D viewer, which uses drag-to-rotate and would otherwise lose
              // horizontal swipes.
              Screen scr;
              { ModelLock lk; scr = model.screen; }
              if (scr != Screen::Viewer3D) {
                Event back = makeEvent(EventType::ButtonShort);
                postEvent(back);
              }
            } else {
              Event ge = makeEvent(EventType::Gesture);
              ge.x = touchpad.data.x;
              ge.y = touchpad.data.y;
              ge.gesture = g;
              postEvent(ge);
            }
          }
          lastG = g; lastGMs = now;
        }
      }
    }

    // ---- Button (GPIO, no Wire) ----
    bool btn = digitalRead(PIN_BTN);
    if (btn != lastBtn) {
      if (btn) {
        btnDownMs = millis();
        veryLongFired = false;
        Event e = makeEvent(EventType::ButtonDown);
        postEvent(e);
      } else {
        uint32_t held = millis() - btnDownMs;
        Event e = makeEvent(EventType::ButtonUp);
        postEvent(e);
        if (held < 1000 && !veryLongFired) {
          Event s = makeEvent(EventType::ButtonShort);
          postEvent(s);
        }
      }
      { ModelLock lk; model.button = btn; model.revision++; }
      lastBtn = btn;
    }
    if (btn && !veryLongFired && (millis() - btnDownMs > 3000)) {
      Event e = makeEvent(EventType::ButtonVeryLong);
      postEvent(e);
      veryLongFired = true;
    }

    // ---- Accel + shake (Wire) ----
    int16_t ax = 0, ay = 0, az = 0;
    bool accOk = readAccel(ax, ay, az);
    if (accOk) {
      uint32_t d = (uint32_t)abs(ax - pax) + abs(ay - pay) + abs(az - paz);
      pax = ax; pay = ay; paz = az;
      if (millis() - motionWindowStart > 300) {
        motionWindowStart = millis();
        motionAcc = 0;
      }
      motionAcc += d;
      // Higher threshold so a deliberate jolt is required, not casual movement.
      if (motionAcc > 18000 && millis() - lastMotionMs > 800) {
        lastMotionMs = millis();
        motionAcc = 0;
        Event e = makeEvent(EventType::ImuMotion);
        postEvent(e);
      }
    }

    // ---- RTC + battery (lower rate) ----
    uint8_t h = 0, mm = 0, s = 0;
    uint8_t wd = 0, dy = 0, mo = 1;
    uint16_t yr = 2025;
    bool rtcOk = false;
    bool ranRtc = (cycle % 4 == 0);
    if (ranRtc) rtcOk = readRTC(h, mm, s, wd, dy, mo, yr);

    float vbat = 0; uint8_t pct = 0; bool batOk = false;
    bool ranBat = (cycle % 50 == 0);
    if (ranBat) batOk = readBattery(vbat, pct);

    bool tickEvent = false;
    {
      ModelLock lk;
      model.ax = ax; model.ay = ay; model.az = az; model.imuOk = accOk;
      if (ranRtc) {
        if (rtcOk && s != model.second) tickEvent = true;
        model.hour = h; model.minute = mm; model.second = s;
        model.weekday = wd; model.day = dy; model.month = mo; model.year = yr;
        model.rtcOk = rtcOk;
      }
      if (ranBat) {
        model.vbat = vbat; model.batPct = pct; model.batOk = batOk;
      }
      model.revision++;
    }

    if (tickEvent) {
      Event e = makeEvent(EventType::Tick);
      postEvent(e);
    }

    // ---- Countdown timer expiry ----
    // Checked on every RTC read. timerArmed() is persistent (RTC memory), so
    // this also catches a timer that came due while the watch was asleep.
    if (ranRtc && rtcOk && timerArmed()) {
      uint32_t nowEpoch = rtcEpochSec(yr, mo, dy, h, mm, s);
      if (nowEpoch >= timerDeadlineEpoch()) {
        timerMarkFired();
        hapticBuzz(400, 220);                 // strong initial alarm buzz
        Event e = makeEvent(EventType::TimerExpired);
        postEvent(e);
      }
    }

    cycle++;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Drop the LDO latch and stop. Called from the render task in response to
// global power-off triggers (3 s button hold) or from PowerOffView.
static void shutdownNow() {
  if (gfx) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(80, 130);
    gfx->print("bye");
  }
  hapticBuzz(180, 200);
  vTaskDelay(pdMS_TO_TICKS(80));
  backlightOff();
  unlatchPower();
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

// Activity-flagging predicate: which event types count as "user is awake".
static bool isActivity(EventType t) {
  return t == EventType::Touch       ||
         t == EventType::TouchHold   ||
         t == EventType::Gesture     ||
         t == EventType::ButtonDown  ||
         t == EventType::ButtonShort ||
         t == EventType::ImuMotion;
}

// Render task: drains events into the active view and re-renders on dirty.
// Touches the display from a single task; the I/O task never paints. Drains
// up to N events per cycle so a fast TouchHold stream doesn't queue up.
// Also enforces the auto-sleep timeout: after sleepTimeoutSec of no activity
// we enter deep sleep with the configured wake sources.
static void taskRender(void *) {
  // Subscribe to the system task watchdog. A frozen view (infinite redraw,
  // stuck animation, deadlock) will fail to feed within the WDT window and
  // trigger a chip reset — covered by the panic-loop guard in main().
  esp_task_wdt_add(nullptr);
  uint32_t lastRender = 0;
  uint32_t lastSeenRev = 0;
  uint32_t lastActivity = millis();
  for (;;) {
    esp_task_wdt_reset();
    Event e;
    bool any = false;
    int drained = 0;
    while (drained < 8 &&
           xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(any ? 0 : 50)) == pdPASS) {
      any = true; drained++;
      if (e.type == EventType::ButtonVeryLong) {
        shutdownNow();
      }
      if (e.type == EventType::TimerExpired) {
        // Surface the alarm no matter what screen we're on.
        lastActivity = millis();
        switchTo(Screen::Timer);
        continue;                       // don't also dispatch to the old view
      }
      if (isActivity(e.type)) lastActivity = millis();
      if (currentView) currentView->onEvent(e);
    }

    // Auto-sleep check
    uint16_t timeoutSec;
    Screen   scr;
    { ModelLock lk;
      timeoutSec = model.sleepTimeoutSec;
      scr        = model.screen; }
    // Never auto-sleep mid-action: the power screen, or while the user is
    // looking at a QR code / image (they asked for those to stay lit).
    bool blockSleep = (scr == Screen::PowerOff)
                   || (scr == Screen::QRCode)
                   || (scr == Screen::Media);
    if (timeoutSec > 0 && !blockSleep &&
        (millis() - lastActivity) > (uint32_t)timeoutSec * 1000) {
      enterDeepSleep();   // does not return
    }

    uint32_t rev;
    { ModelLock lk; rev = model.revision; }
    bool dirty = any || (rev != lastSeenRev) || (millis() - lastRender > 1000);
    if (dirty && currentView) {
      currentView->render();
      lastRender = millis();
      lastSeenRev = rev;
    }
  }
}

static TaskHandle_t ioTaskHandle = nullptr;

void controllerStartTasks() {
  // Stack sizes tuned from observed high-water marks:
  //   io:     peak ~2.2 KiB; 4 KiB leaves a healthy ~1.8 KiB margin.
  //   render: peak ~2.1 KiB but view->render() recurses through Arduino_GFX +
  //           Adafruit FreeFont code paths and the carousel's inline frame
  //           loop — keep 6 KiB so a deep app render can't tip into overflow.
  xTaskCreatePinnedToCore(taskIO,     "io",     4096, nullptr, 5, &ioTaskHandle, 0);
  xTaskCreatePinnedToCore(taskRender, "render", 6144, nullptr, 4, nullptr,        1);
}

// ---------- Deep sleep ----------
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

// MMA8451 jolt-wake setup. Uses the TRANSIENT block (not motion/FF), which
// runs the accel through a high-pass filter so gravity / slow tilts are
// ignored — only sharp acceleration changes raise INT1. Tune via
// TRANSIENT_THS (in 0.063 g units) and TRANSIENT_COUNT (debounce).
static void configureMmaForJoltWake() {
  if (!mmaAddr) return;
  uint8_t threshold;
  { ModelLock lk; threshold = model.imuWakeThreshold; }
  if (threshold < 0x04) threshold = 0x04;   // clamp to a sane minimum

  auto wr = [&](uint8_t reg, uint8_t val) {
    Wire.beginTransmission(mmaAddr);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
  };
  wr(0x2A, 0x00);        // CTRL_REG1 STANDBY
  wr(0x1D, 0x1E);        // TRANSIENT_CFG: ELE + Z/Y/X transient enabled
  wr(0x1F, threshold);   // TRANSIENT_THS: from settings (LSB = 0.063 g)
  wr(0x20, 0x05);        // TRANSIENT_COUNT: 5 samples debounce
  wr(0x2C, 0x02);        // CTRL_REG3: IPOL=1 (active high), PP_OD=0
  wr(0x2D, 0x20);        // CTRL_REG4: enable TRANSIENT interrupt
  wr(0x2E, 0x20);        // CTRL_REG5: route TRANSIENT to INT1
  wr(0x2A, 0x01);        // CTRL_REG1 ACTIVE
}

void enterDeepSleep() {
  bool wkTouch, wkBtn, wkImu;
  uint16_t toOffSec;
  uint8_t  rtcH = 0, rtcM = 0, rtcS = 0, rtcDay = 1, rtcMon = 1;
  uint16_t rtcYear = 2025;
  bool     rtcOk = false;
  { ModelLock lk;
    wkTouch  = model.wakeOnTouch;
    wkBtn    = model.wakeOnButton;
    wkImu    = model.wakeOnImu;
    toOffSec = model.sleepToOffSec;
    rtcH = model.hour; rtcM = model.minute; rtcS = model.second;
    rtcDay = model.day; rtcMon = model.month; rtcYear = model.year;
    rtcOk = model.rtcOk; }

  // Stop the I/O task so we own the I2C bus for the pre-sleep drain.
  if (ioTaskHandle) vTaskSuspend(ioTaskHandle);
  vTaskDelay(pdMS_TO_TICKS(40));    // let any in-flight Wire xfer finish

  if (wkImu) configureMmaForJoltWake();

  // ---- Drain pending interrupt sources so we don't immediately wake up ----
  // CST816S: read regs 0x01..0x06 to clear the current touch frame.
  {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(0x01);
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
      while (Wire.available()) (void)Wire.read();
    }
  }
  // MMA8451: read TRANSIENT_SRC (0x1E) to clear any latched jolt event so
  // INT1 deasserts before we arm ext1. Also read FF_MT_SRC (0x16) defensively
  // in case prior firmware left motion-detect latched.
  if (mmaAddr) {
    auto drainReg = [&](uint8_t reg) {
      Wire.beginTransmission(mmaAddr);
      Wire.write(reg);
      if (Wire.endTransmission(false) == 0) {
        Wire.requestFrom((int)mmaAddr, 1);
        if (Wire.available()) (void)Wire.read();
      }
    };
    drainReg(0x1E);
    drainReg(0x16);
  }
  // Touch ISR flag: spurious if the chip raised INT during the screens we
  // just animated. Discard.
  touchPending = false;

  // Settle: wait for the user's finger to lift and the INT lines to deassert.
  vTaskDelay(pdMS_TO_TICKS(300));

  // CST816S touch INT is active-low -> ext0 with level=0.
  if (wkTouch) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  }

  // Button (active-high) and MMA INT1 (configured active-high above) -> ext1
  // with ANY_HIGH. ext0 + ext1 can both be enabled.
  uint64_t mask = 0;
  if (wkBtn) mask |= 1ULL << PIN_BTN;
  if (wkImu) mask |= 1ULL << PIN_MMA_INT1;
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Timer wake-up. An armed countdown timer takes priority over the
  // auto-power-off interval: we wake exactly when it should alarm. Otherwise
  // the auto-power-off timer (sleepToOffSec) is armed as before. main() tells
  // the two apart on wake by checking timerArmed() + the RTC deadline.
  bool armedTimerWake = false;
  if (rtcOk && timerArmed()) {
    uint32_t nowEpoch = rtcEpochSec(rtcYear, rtcMon, rtcDay, rtcH, rtcM, rtcS);
    uint32_t remain   = timerRemainingSec(nowEpoch);
    if (remain == 0) remain = 1;          // fire almost immediately
    esp_sleep_enable_timer_wakeup((uint64_t)remain * 1000000ULL);
    armedTimerWake = true;
  }
  if (!armedTimerWake && toOffSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)toOffSec * 1000000ULL);
  }

  // Hold the LDO latch through sleep so the rail stays up.
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
  // Does not return.
}
