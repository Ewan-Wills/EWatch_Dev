// EWatch v2 board support for the stand-alone Claude Face firmware: the
// minimum of AuroraOS' drivers this face needs (display + backlight,
// DRV2603 haptics, CST816S touch, RV-3028 RTC, MMA8451 accelerometer,
// battery divider), cut down from AuroraOS src/drivers/ and
// src/core/controller.cpp.
//
// Threading:
//   * Wire (touch, RTC, accel) belongs to the UI loop (main.cpp loop()) ONLY.
//     Other tasks ask for an RTC write with rtcRequestSet(); the UI loop
//     applies it with rtcTakeRequest() + rtcWrite().
//   * gfx / the canvas / the backlight: UI loop only.
//   * hapticBuzz(): any task (queues to the haptic task).
#pragma once
#include <stdint.h>
#include <Arduino_GFX_Library.h>
#include "pins.h"

// ---- power latch (see powerOffNow in main.cpp) ----------------------------
// Pre-load HIGH on the output register, THEN enable the driver: the
// pinMode-first order flashes LOW for ~1 us, enough to droop the rail.
static inline void latchPower() {
  digitalWrite(PIN_LDO_LATCH, HIGH);
  pinMode(PIN_LDO_LATCH, OUTPUT);
}
static inline void unlatchPower() { digitalWrite(PIN_LDO_LATCH, LOW); }
static inline bool buttonPressed() { return digitalRead(PIN_BTN) == HIGH; }

// ---- display ----------------------------------------------------------------
extern Arduino_GFX *gfx;                   // null until displayBegin() succeeds
bool            displayBegin();           // panel on, cleared, backlight OFF
Arduino_Canvas *frameCanvas();            // full-frame 240x280 canvas (PSRAM)

// Backlight: LEDC channel 0, 12 kHz, 8 bit (0 = off, 255 = full).
void    backlightSet(uint8_t duty);
uint8_t backlightGet();
void    backlightFadeTo(uint8_t target, uint16_t ms);   // non-blocking
void    backlightService();                             // UI loop: advance a fade

// ---- haptics (DRV2603: EN gate + PWM on LEDC channel 1) ---------------------
void hapticBegin();
void hapticBuzz(uint8_t intensity, uint16_t durationMs);   // any task, drops if busy

// ---- I2C bus (UI loop only) -------------------------------------------------
void i2cBegin();                         // also creates the RTC request queue
void i2cRestoreClock();
bool i2cPing(uint8_t addr);

// ---- RTC (RV-3028 holds LOCAL wall time, years from 2000, wday 0 = Sunday) -
struct RtcTime {
  uint16_t year;
  uint8_t  month, day, weekday, hour, minute, second;
};
bool rtcRead(RtcTime &t);
bool rtcWrite(const RtcTime &t);
void rtcRequestSet(const RtcTime &t);     // any task -> queue
bool rtcTakeRequest(RtcTime &t);          // UI loop

// ---- accelerometer (MMA8451) --------------------------------------------------
// accelBegin() puts it in a 50 Hz low-power scheme with EVERY interrupt
// source disabled and IPOL active-high, so INT1 (GPIO3, a strapping pin)
// and INT2 are never asserted.
bool accelBegin();
bool accelRead(int16_t &x, int16_t &y, int16_t &z);

// ---- battery -------------------------------------------------------------------
bool batteryRead(float &volts, uint8_t &pct);

// ---- touch (CST816S) -------------------------------------------------------------
void touchReset();                        // RST pulse (the chip sleeps at cold boot)
bool touchBegin();                        // false if the chip never ACKed
bool touchReadHeld(uint16_t &x, uint16_t &y);   // live finger state (poll)
// Gesture latched by the chip since the last call (ISR-flagged). 0 = none.
uint8_t touchTakeGesture(uint16_t &x, uint16_t &y);
void touchSelfHeal();                     // re-init after ~2 s of NACKs
