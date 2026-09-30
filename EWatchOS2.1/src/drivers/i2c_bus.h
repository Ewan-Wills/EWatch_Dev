// I2C driver — shared bus for all on-board peripherals.
//
// Devices on the bus:
//   * 0x15 — CST816S touch controller        (drivers/touch.cpp)
//   * 0x1C / 0x1D — MMA8451 accelerometer    (drivers/controller.cpp readAccel)
//   * 0x52 — RV-3028 RTC                     (drivers/controller.cpp readRTC)
//
// Pins are defined in pins.h; the bus runs at 400 kHz. The CST816S library
// re-init's Wire internally and may drop the clock back to 100 kHz —
// touchBegin() calls i2cRestoreClock() to undo that.
//
// All Wire transactions must happen on the I/O task to keep the singleton
// Wire object thread-safe. Cross-task RTC writes go through the
// requestSetRTC() queue in controller.cpp.
#pragma once
#include <stdint.h>

void i2cBegin();
void i2cRestoreClock();
void i2cScan();
bool i2cPing(uint8_t addr);
bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &out);

// One-line status prints to Serial — called from boot for liveness output.
void probeRV3028();
void probeMMA8451();
