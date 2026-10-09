#include <Arduino.h>
#include "pins.h"
#include "i2c_bus.h"
#include "touch.h"

CST816S touchpad(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);
volatile bool touchPending = false;
volatile bool touchPresent = false;

static void IRAM_ATTR touchISR() {
  touchPending = true;
}

void touchReset() {
  // Same sequence the CST816S library's begin() uses: idle high, 5 ms low
  // pulse, then high + settle. After this the chip is awake long enough to
  // ACK on I2C, so a scan can tell "missing/dead" apart from "asleep".
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(5);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);
}

void touchBegin() {
  // CST816S 1.3+ has two begin() overloads; pass Wire explicitly to disambiguate.
  // begin() hardware-resets the chip and reads its version registers.
  touchpad.begin(Wire);

  // The CST816S library re-initialises Wire internally and may drop the bus
  // back to the default 100 kHz. Restore our 400 kHz target before anyone else
  // (accel/RTC) uses the bus — do this regardless of touch presence.
  i2cRestoreClock();

  // Confirm the controller actually answered. On boards where the touch FPC or
  // its solder is open the chip never ACKs even after a clean reset; bail out
  // so the I/O task doesn't spam Wire errors against the watchdog every poll.
  touchPresent = i2cPing(I2C_ADDR_TOUCH);
  if (!touchPresent) {
    Serial.println("Touch  : CST816S NOT FOUND (0x15 silent) — touch disabled");
    return;
  }

  touchpad.disable_auto_sleep();

  // Drain any spurious touches that fired while the panel was waking up.
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {
    if (touchpad.available()) touchpad.gesture();
  }

  touchpad.attachUserInterrupt(touchISR);
  Serial.println("Touch  : CST816S ready");
}