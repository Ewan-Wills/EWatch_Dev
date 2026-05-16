// EWatch v2 — boot only. Hardware bring-up + FreeRTOS task launch. All
// runtime logic lives in controller.cpp (I/O + tasks) and view.cpp (UI).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include "pins.h"
#include "power.h"
#include "i2c_bus.h"
#include "display.h"
#include "touch.h"
#include "haptic.h"

#include "model.h"
#include "event.h"
#include "view.h"
#include "controller.h"
#include "storage.h"
#include "apptimer.h"
#include "wifi_svc.h"

// ----- Touch wake validation ------------------------------------------------
// The CST816S INT line can be tripped by EM noise (USB charging, body
// capacitance, etc.) when no finger is present. Poll the chip after a touch
// wake; if no finger / gesture is reported within ~150 ms, treat it as a
// false alarm and go straight back to sleep without ever lighting up the UI.
static bool touchReallyPresent() {
  // First read snapshots both gesture (0x01) and FingerNum (0x02). A finger
  // currently down OR a freshly-latched gesture both count as "real".
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0 &&
      Wire.requestFrom((int)I2C_ADDR_TOUCH, 2) == 2) {
    uint8_t g   = Wire.read();
    uint8_t pts = Wire.read();
    if (g != 0 || pts > 0) return true;
  }
  // Fallback: poll FingerNum for ~120 ms to catch a slightly delayed finger.
  for (int i = 0; i < 4; i++) {
    delay(30);
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(0x02);
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom((int)I2C_ADDR_TOUCH, 1) == 1) {
      if (Wire.read() > 0) return true;
    }
  }
  return false;
}

// Map ESP reset reasons to short strings for both serial + on-screen logs.
static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    default:                return "?";
  }
}
static bool isCrashReason(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT;
}

// ----- High-level exception safety net --------------------------------------
// The watch holds its own power via PIN_LDO_LATCH, so if the firmware locks
// up the user can't power-cycle without pulling the battery. Two layers of
// defense:
//
//   1. Per-task watchdog (Task WDT) — every long-running task feeds it on
//      each loop iteration; if any task stalls > kTaskWdtSec the chip resets.
//      The TWDT is configured below in setup() right before tasks launch.
//
//   2. Reset-loop guard — a counter in RTC_DATA_ATTR memory (survives soft
//      reset, lost on full power-off) increments on every crash/watchdog
//      boot. If it crosses kPanicGiveUpLimit we drop the LDO latch and the
//      watch powers off — the user has to press SW2 again to retry fresh,
//      which guarantees the persistent hang state is broken.
//
//   3. Stable-run timer — once the firmware has been alive for kStableSec
//      seconds, reset the counter back to 0. A real bug pattern still racks
//      up the count; a single one-off crash doesn't bias future boots.
RTC_DATA_ATTR static uint32_t gConsecutivePanics = 0;
static const uint32_t kPanicGiveUpLimit = 3;
static const uint32_t kTaskWdtSec       = 20;
static const uint32_t kStableSec        = 30;

static void checkPanicLoopAndMaybeShutdown(esp_reset_reason_t r) {
  if (!isCrashReason(r)) {
    if (gConsecutivePanics) {
      Serial.printf("Clean boot — clearing panic counter (was %lu).\n",
                    (unsigned long)gConsecutivePanics);
    }
    gConsecutivePanics = 0;
    return;
  }
  gConsecutivePanics++;
  Serial.printf("Panic-loop counter: %lu / %lu (reason=%s)\n",
                (unsigned long)gConsecutivePanics,
                (unsigned long)kPanicGiveUpLimit,
                resetReasonStr(r));
  if (gConsecutivePanics > kPanicGiveUpLimit) {
    Serial.println("!!! Too many consecutive panics — dropping LDO latch !!!");
    Serial.flush();
    // Drop the rail. The chip stops getting power within ~µs; we spin here
    // just so we don't fall through to anything that might re-latch.
    unlatchPower();
    for (;;) delay(100);
  }
}

// One-shot esp_timer that resets the panic counter once we've stayed up for
// `kStableSec` seconds. Runs in the timer task; the access is a single u32
// write so no lock is needed.
static void panicCounterStableReset(void *) {
  if (gConsecutivePanics) {
    Serial.printf("Stable for %lus — clearing panic counter (was %lu).\n",
                  (unsigned long)kStableSec,
                  (unsigned long)gConsecutivePanics);
  }
  gConsecutivePanics = 0;
}
static void armPanicCounterReset() {
  static esp_timer_handle_t h = nullptr;
  if (h) return;
  esp_timer_create_args_t args = {};
  args.callback        = &panicCounterStableReset;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name            = "panicrst";
  esp_timer_create(&args, &h);
  esp_timer_start_once(h, (uint64_t)kStableSec * 1000000ULL);
}

// Install the Task WDT before any user task subscribes. ESP-IDF v4 signature:
// esp_task_wdt_init(timeout_seconds, panic_on_timeout). Arduino-ESP32 already
// inits it for the loopTask at boot; calling it again with our timeout is a
// no-op for the loopTask subscription but sets the timeout we want.
static void installTaskWatchdog() {
  esp_err_t err = esp_task_wdt_init(kTaskWdtSec, /*panic=*/true);
  if (err != ESP_OK) {
    Serial.printf("esp_task_wdt_init err=%d\n", (int)err);
  } else {
    Serial.printf("Task WDT armed: %lus, panic-on-timeout\n",
                  (unsigned long)kTaskWdtSec);
  }
  // Subscribe loopTask (current). The render/io/wifi tasks subscribe themselves
  // at the top of their loop bodies so each one feeds its own watchdog.
  esp_task_wdt_add(nullptr);
}

// Show a crash-recovery banner on screen if the previous boot ended in a
// panic / watchdog / brownout. The actual backtrace lives on the serial
// console; we just surface the reason here so the failure isn't invisible
// when the watch is running headless. Waits up to 8 s for SW2, then continues.
static void showCrashScreenIfRecovering() {
  esp_reset_reason_t r = esp_reset_reason();
  if (!isCrashReason(r)) return;
  const char *reason = resetReasonStr(r);
  Serial.printf("\n!!! CRASH RECOVERY: previous reset reason = %s !!!\n", reason);
  if (!gfx) return;

  pinMode(PIN_BTN, INPUT);
  backlightSet(160);
  gfx->fillScreen(0);
  gfx->fillRect(0, 0, 240, 36, 0xF800);            // red banner
  gfx->setTextColor(0xFFFF, 0xF800);
  gfx->setTextSize(2);
  gfx->setCursor(36, 10);
  gfx->print("CRASH RECOVERY");

  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setTextSize(1);
  gfx->setCursor(10, 52);
  gfx->print("Last reset reason:");

  gfx->setTextSize(3);
  gfx->setTextColor(0xFFE0, 0x0000);               // yellow
  int len = (int)strlen(reason);
  int x = (240 - len * 18) / 2;
  if (x < 6) x = 6;
  gfx->setCursor(x, 80);
  gfx->print(reason);

  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setCursor(10, 140); gfx->print("See USB serial monitor for");
  gfx->setCursor(10, 154); gfx->print("backtrace + 'rst:' messages.");

  gfx->setTextColor(0x8410, 0x0000);
  gfx->setCursor(10, 210); gfx->print("Continuing in 8s, or press SW2");
  gfx->setCursor(10, 224); gfx->print("to resume immediately.");

  uint32_t t0 = millis();
  while (millis() - t0 < 8000) {
    if (digitalRead(PIN_BTN)) break;
    delay(50);
  }
  gfx->fillScreen(0);
  backlightOff();          // return to clean state; caller re-enables after first frame
}

static void reArmFromSpuriousWakeAndSleep() {
  // Load the user's persisted wake config so re-sleep matches their settings.
  Preferences p;
  p.begin("ewatch", /*readOnly=*/true);
  bool     wkT   = p.getBool  ("wkTouch",  true);
  bool     wkB   = p.getBool  ("wkButton", true);
  bool     wkI   = p.getBool  ("wkImu",    false);
  uint16_t toOff = p.getUShort("sleepOff", 30);
  p.end();

  // Drain CST816S touch buffer so the INT line de-asserts before we re-arm.
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
    while (Wire.available()) (void)Wire.read();
  }
  delay(80);

  if (wkT) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  }
  uint64_t mask = 0;
  if (wkB) mask |= 1ULL << PIN_BTN;
  if (wkI) mask |= 1ULL << PIN_MMA_INT1;
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (toOff > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)toOff * 1000000ULL);
  }

  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();   // does not return
}

void setup() {
  // If we just woke from deep sleep the LDO latch was held by the RTC IO
  // block; release the hold before re-asserting it as a regular GPIO.
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool fromSleep = (wake != ESP_SLEEP_WAKEUP_UNDEFINED);
  if (fromSleep) {
    gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
    gpio_deep_sleep_hold_dis();
    // A timer wake is one of two things:
    //   - the countdown Timer app came due  -> boot normally, alarm fires
    //     once the I/O task confirms the RTC deadline has passed.
    //   - the auto-power-off interval elapsed -> drop the rail and stop.
    if (wake == ESP_SLEEP_WAKEUP_TIMER && !timerArmed()) {
      digitalWrite(PIN_LDO_LATCH, LOW);
      pinMode(PIN_LDO_LATCH, OUTPUT);
      for (;;) delay(1000);
    }
  }
  latchPower();

  // BEFORE anything else can hang: if the last 3 boots all ended in panic
  // or watchdog, drop the rail and stop. Otherwise the watch will sit there
  // burning battery in a hung loop the user can't break without pulling
  // the battery.
  esp_reset_reason_t resetReason = esp_reset_reason();
  checkPanicLoopAndMaybeShutdown(resetReason);

  Serial.begin(115200);
  // On battery the USB-CDC never enumerates, so !Serial stays true for the
  // full timeout. That dominated wake-from-sleep latency. Skip the wait on
  // wake; on cold boot keep a short window so attached-USB logs aren't lost.
  if (!fromSleep) {
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 300) delay(10);
  }
  Serial.println("\n=== EWatch boot ===");

  i2cBegin();

  // Touch-triggered wake: validate that a finger is actually present before
  // bringing up the rest of the system. False alarms from EMI / charging
  // would otherwise flash the UI on for a moment for no reason.
  if (wake == ESP_SLEEP_WAKEUP_EXT0) {
    if (!touchReallyPresent()) {
      Serial.println("Touch wake rejected (no finger); going back to sleep.");
      reArmFromSpuriousWakeAndSleep();   // does not return
    }
    Serial.println("Touch wake confirmed.");
  }

  i2cScan();
  probeRV3028();
  probeMMA8451();

  hapticBegin();

  if (!displayBegin()) {
    Serial.println("display init failed; halting");
    while (true) delay(1000);
  }
  // displayBegin leaves the backlight OFF and the panel cleared to BLACK. We
  // keep it that way through the rest of init so the user never sees the
  // ST7789's power-up garbage. Crash screen (below) turns it on briefly if
  // it has something to say, then drops it again.
  showCrashScreenIfRecovering();

  touchBegin();
  controllerInit();

  modelInit();
  Storage::begin();
  Storage::load();              // restore persisted settings before tasks run
  hapticSetStrengthPct(model.hapticStrength);   // honor saved strength immediately
  eventQueueInit();
  viewsInit();                  // paints watch face background under the dark panel

  // Arm the task watchdog BEFORE tasks launch so they can subscribe at the
  // top of their loops. Each task feeds its own watchdog; if any stalls
  // longer than kTaskWdtSec the chip resets (caught by the panic-loop guard
  // on the next boot).
  installTaskWatchdog();

  wifiSvcInit();
  controllerStartTasks();
  wifiSvcStartTask();

  // Give the render task a brief window to paint the first complete frame
  // (time, date, icons), then bring the backlight up at the user's level.
  // The user sees a clean watch face appear instead of a partial paint.
  delay(80);
  backlightSet(model.brightness);

  // Quieter cue if we woke from sleep — single short buzz instead of the
  // boot-confirmation buzz.
  hapticBuzz(150, fromSleep ? 40 : 80);

  // 30 s from now, clear the panic-loop counter. If we survive that window
  // the firmware is healthy enough that the next crash counts as fresh.
  armPanicCounterReset();

  Serial.printf("Setup complete (wake=%d); tasks running.\n", (int)wake);
}

void loop() {
  // Everything else is in tasks. Idle the loopTask, but feed the WDT so we
  // don't trip the watchdog from this thread.
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(1000));
}
