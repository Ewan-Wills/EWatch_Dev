// Shared I2C bus: RTC (RV-3028 @ 0x52), accelerometer (MMA8451 @ 0x1C/0x1D),
// touch (CST816S @ 0x15). Touch driver re-init's Wire internally — call
// i2cRestoreClock() afterwards if you need to be sure of the bus speed.
#pragma once
#include <stdint.h>

void i2cBegin();
void i2cRestoreClock();
void i2cScan();
bool i2cPing(uint8_t addr);
bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &out);

// Convenience probes that print a one-line status to Serial.
void probeRV3028();
void probeMMA8451();
