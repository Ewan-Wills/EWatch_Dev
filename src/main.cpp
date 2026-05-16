// EWatch v2 — boot only. Hardware bring-up + FreeRTOS task launch. All
// runtime logic lives in controller.cpp (I/O + tasks) and view.cpp (UI).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
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
  eventQueueInit();
  viewsInit();                  // paints watch face background under the dark panel

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
  Serial.printf("Setup complete (wake=%d); tasks running.\n", (int)wake);
}

void loop() {
  // Everything is in tasks. Idle the loopTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
