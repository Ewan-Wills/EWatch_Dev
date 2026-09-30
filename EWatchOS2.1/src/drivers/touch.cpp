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
  // Pull the INT line up BEFORE any interrupt machinery touches it: the pad
  // floats between the chip's active-low pulses, and installing a FALLING
  // interrupt on a floating pin can start an edge storm the instant the
  // vector is enabled — starving core 1 long enough to trip the interrupt
  // watchdog (observed as an INT_WDT boot loop inside gpio_isr_register /
  // ipc_task).
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

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

  // Re-assert the pull-up: the library's begin()/attach path reconfigures
  // the pin as plain INPUT. pinMode after attachInterrupt is safe on the
  // ESP32 core (the INT config stays).
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

  Serial.println("Touch  : CST816S ready");
}

// Direct register writes below deliberately bypass the CST816S library (it
// re-inits Wire on some calls); plain Wire transactions leave the bus clock
// alone, so no i2cRestoreClock() dance is needed here.
static bool touchWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint32_t sTouchFailStreak = 0;
uint32_t touchConsecutiveFailures() { return sTouchFailStreak; }
void touchNoteReadResult(bool ok) {
  if (ok) sTouchFailStreak = 0;
  else if (sTouchFailStreak < 0xFFFFFFFF) sTouchFailStreak++;
}

bool touchSetMonitorMode(bool monitor) {
  if (!touchPresent) return true;
  // 0xFE DisAutoSleep: 0 = auto-sleep (monitor mode) enabled, non-zero =
  // stay in dynamic scan mode. Monitor mode keeps INT alive at low power.
  // A NACK means the chip already dropped into its sleep state (it stops
  // ACKing there) — the caller must touchRecover().
  return touchWriteReg(0xFE, monitor ? 0x00 : 0x01);
}

void touchRecover() {
  if (!touchPresent) return;
  // Short RST pulse (the chip boots in ~50 ms), then re-apply the volatile
  // registers a reset wipes: IrqCtl (EnTouch|EnChange|EnMotion — the hold
  // logic depends on EnChange) and DisAutoSleep. Finish with a drain so a
  // stale event frame can't re-assert INT.
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(5);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(60);
  touchWriteReg(0xFA, 0x70);
  touchWriteReg(0xFE, 0x01);
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
    while (Wire.available()) (void)Wire.read();
  }
  touchPending = false;
  sTouchFailStreak = 0;
}

void touchDeepSleepNow() {
  if (!touchPresent) return;
  // 0xA5 PowerMode = 0x03: deep sleep. Only a RST pulse wakes it — deep-sleep
  // (reboot) path only.
  touchWriteReg(0xA5, 0x03);
}

bool touchReadEventDirect(uint8_t &gesture, uint8_t &points,
                          uint16_t &x, uint16_t &y) {
  if (!touchPresent) return false;
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 6) != 6) return false;
  gesture      = Wire.read();            // 0x01 GestureID
  points       = Wire.read();            // 0x02 FingerNum
  uint8_t xh   = Wire.read();            // 0x03..0x06 XH XL YH YL
  uint8_t xl   = Wire.read();
  uint8_t yh   = Wire.read();
  uint8_t yl   = Wire.read();
  x = (uint16_t)(((xh & 0x0F) << 8) | xl);
  y = (uint16_t)(((yh & 0x0F) << 8) | yl);
  return true;
}