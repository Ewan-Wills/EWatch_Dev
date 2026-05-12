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
    // Timer wake means we hit the sleep -> off limit with no user activity.
    // Drop the rail and stop — the chip loses power once the LDO unlatches.
    if (wake == ESP_SLEEP_WAKEUP_TIMER) {
      digitalWrite(PIN_LDO_LATCH, LOW);
      pinMode(PIN_LDO_LATCH, OUTPUT);
      for (;;) delay(1000);
    }
  }
  latchPower();

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
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
  backlightOn();
  gfx->fillScreen(BLACK);

  touchBegin();
  controllerInit();

  modelInit();
  Storage::begin();
  Storage::load();              // restore persisted settings before tasks run
  backlightSet(model.brightness);   // apply user-configured backlight level
  eventQueueInit();
  viewsInit();

  controllerStartTasks();

  // Quieter cue if we woke from sleep — single short buzz instead of the
  // boot-confirmation buzz.
  hapticBuzz(150, fromSleep ? 40 : 80);
  Serial.printf("Setup complete (wake=%d); tasks running.\n", (int)wake);
}

void loop() {
  // Everything is in tasks. Idle the loopTask.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
