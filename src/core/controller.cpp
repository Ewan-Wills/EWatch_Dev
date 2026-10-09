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

// ---------- low-level I/O ----------
static uint8_t mmaAddr = 0;

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
  if (!touchPresent) return false;        // no controller — skip the I2C read
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

// ---------- init ----------
void controllerInit() {
  pinMode(PIN_BTN,      INPUT);
  pinMode(PIN_MMA_INT1, INPUT);
  pinMode(PIN_MMA_INT2, INPUT);

  if      (i2cPing(I2C_ADDR_MMA8451_A)) mmaAddr = I2C_ADDR_MMA8451_A;
  else if (i2cPing(I2C_ADDR_MMA8451_B)) mmaAddr = I2C_ADDR_MMA8451_B;
  mmaActivate();

  // CST816S IrqCtl (0xFA): EnTouch + EnChange + EnMotion. Without EnChange the
  // chip stops firing IRQs while a finger is held still. Skip when no
  // controller answered at boot.
  if (touchPresent) {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(0xFA);
    Wire.write(0x70);
    Wire.endTransmission();
  }
}

// ---------- FreeRTOS tasks ----------

// Single I/O task — owns the I2C bus to avoid Wire-singleton races. Runs at
// 50 Hz; samples the accelerometer (steering) and button every cycle and
// drains touch gestures from the CST816S.
static void taskIO(void *) {
  // Subscribe to the system task watchdog. If this loop ever stalls for more
  // than the WDT timeout the chip resets, and the panic-loop guard in main()
  // catches the repeat and drops the LDO latch.
  esp_task_wdt_add(nullptr);
  bool     lastBtn = false;
  uint32_t btnDownMs = 0;
  bool     veryLongFired = false;

  bool     fingerDown = false;
  uint32_t lastHoldPostMs = 0;
  uint16_t lastTouchX = 0, lastTouchY = 0;

  for (;;) {
    esp_task_wdt_reset();

    // ---- Touch state machine ----
    // The polled FingerNum register is the source of truth for press/hold/
    // release; the ISR is only used to fish out gesture codes. This dodges the
    // CST816S quirk where the chip stops emitting interrupts while a finger is
    // held stationary.
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

    // Drain ISR-reported gestures (Touch state already comes from the poll).
    if (touchPending) {
      touchPending = false;
      if (touchpad.available()) {
        Gesture g = (Gesture)touchpad.data.gestureID;
        if (g != Gesture::None) {
          // Dedup: the chip can fire multiple ISRs for one physical swipe.
          // Suppress repeats within 400 ms.
          static Gesture  lastG = Gesture::None;
          static uint32_t lastGMs = 0;
          uint32_t now = millis();
          if (g != lastG || now - lastGMs > 400) {
            Event ge = makeEvent(EventType::Gesture);
            ge.x = touchpad.data.x;
            ge.y = touchpad.data.y;
            ge.gesture = g;
            postEvent(ge);
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

    // ---- Accelerometer (Wire) — tilt steering ----
    int16_t ax = 0, ay = 0, az = 0;
    bool accOk = readAccel(ax, ay, az);
    {
      ModelLock lk;
      model.ax = ax; model.ay = ay; model.az = az; model.imuOk = accOk;
      model.revision++;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Drop the LDO latch and stop. Called from the render task in response to the
// global power-off trigger (3 s button hold).
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

// Activity-flagging predicate: which event types count as "user is active".
static bool isActivity(EventType t) {
  return t == EventType::Touch       ||
         t == EventType::TouchHold   ||
         t == EventType::Gesture     ||
         t == EventType::ButtonDown  ||
         t == EventType::ButtonShort;
}

// Render task: a continuous game loop. Drains input events into the active
// view, then renders one frame every iteration (the view paces itself off the
// millis() delta and blits its own framebuffer). Touches the display from a
// single task; the I/O task never paints. Also enforces the auto-sleep timeout
// — the game sets sleepTimeoutSec = 0 while actively driving so a tilt-only
// session never sleeps, and restores it on the menu.
static void taskRender(void *) {
  // Subscribe to the system task watchdog. A frozen view (infinite redraw,
  // stuck loop, deadlock) will fail to feed within the WDT window and trigger
  // a chip reset — covered by the panic-loop guard in main().
  esp_task_wdt_add(nullptr);
  uint32_t lastActivity = millis();
  for (;;) {
    esp_task_wdt_reset();

    // Drain all pending input events (non-blocking) into the active view.
    Event e;
    while (xQueueReceive(eventQueue, &e, 0) == pdPASS) {
      if (e.type == EventType::ButtonVeryLong) shutdownNow();
      if (isActivity(e.type)) lastActivity = millis();
      if (currentView) currentView->onEvent(e);
    }

    // Auto-sleep when idle.
    uint16_t timeoutSec;
    { ModelLock lk; timeoutSec = model.sleepTimeoutSec; }
    if (timeoutSec > 0 &&
        (millis() - lastActivity) > (uint32_t)timeoutSec * 1000) {
      enterDeepSleep();   // does not return
    }

    // Render one frame, then yield a tick so the idle / loop tasks (and their
    // watchdogs) get serviced.
    if (currentView) currentView->render();
    vTaskDelay(1);
  }
}

void controllerStartTasks() {
  // Stack sizes tuned from observed high-water marks; render recurses through
  // Arduino_GFX + the framebuffer blit so keep it generous.
  xTaskCreatePinnedToCore(taskIO,     "io",     4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(taskRender, "render", 6144, nullptr, 4, nullptr, 1);
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
  { ModelLock lk;
    wkTouch  = model.wakeOnTouch;
    wkBtn    = model.wakeOnButton;
    wkImu    = model.wakeOnImu;
    toOffSec = model.sleepToOffSec; }

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
  // MMA8451: read TRANSIENT_SRC (0x1E) to clear any latched jolt event so INT1
  // deasserts before we arm ext1.
  if (mmaAddr) {
    Wire.beginTransmission(mmaAddr);
    Wire.write(0x1E);
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom((int)mmaAddr, 1);
      if (Wire.available()) (void)Wire.read();
    }
  }
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

  // Auto-power-off timer: if nothing wakes us within toOffSec, a timer wake
  // fires and main() drops the LDO latch on the next boot (full power-off).
  if (toOffSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)toOffSec * 1000000ULL);
  }

  // Hold the LDO latch through sleep so the rail stays up.
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
  // Does not return.
}
