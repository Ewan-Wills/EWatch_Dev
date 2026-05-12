#include <Arduino.h>
#include "pins.h"
#include "i2c_bus.h"
#include "touch.h"

CST816S touchpad(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);
volatile bool touchPending = false;

static void IRAM_ATTR touchISR() {
  touchPending = true;
}

void touchBegin() {
  // CST816S 1.3+ has two begin() overloads; pass Wire explicitly to disambiguate.
  touchpad.begin(Wire);
  touchpad.disable_auto_sleep();

  // The CST816S library re-initialises Wire internally and may drop the bus
  // back to the default 100 kHz. Restore our 400 kHz target.
  i2cRestoreClock();

  // Drain any spurious touches that fired while the panel was waking up.
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {
    if (touchpad.available()) touchpad.gesture();
  }

  touchpad.attachUserInterrupt(touchISR);
  Serial.println("Touch  : CST816S ready");
}