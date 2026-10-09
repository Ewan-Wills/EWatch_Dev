#include <Arduino.h>
#include <Wire.h>
#include "pins.h"
#include "i2c_bus.h"

void i2cBegin() {
  if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ)) {
    Serial.println("ERROR: Wire.begin() failed");
    return;
  }
  Serial.printf("I2C    : SDA=GPIO%d  SCL=GPIO%d  %d Hz\n",
                PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
}

void i2cRestoreClock() {
  Wire.setClock(I2C_FREQ_HZ);
}

bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;     // repeated start
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  out = Wire.read();
  return true;
}

// Friendly name for a known I2C address, or nullptr if unrecognised. Lets the
// boot scan report *which* peripheral answered at each address so the addresses
// in pins.h can be adjusted per-board when a different part is fitted.
static const char *i2cDeviceName(uint8_t addr) {
  switch (addr) {
    case I2C_ADDR_TOUCH:     return "CST816S touch";
    case I2C_ADDR_MMA8451_A: return "MMA8451 accel (SA0=0)";
    case I2C_ADDR_MMA8451_B: return "MMA8451 accel (SA0=1)";
    case I2C_ADDR_RV3028:    return "RV-3028 RTC";
    case 0x34:               return "AXP2101 PMIC";
    default:                 return nullptr;
  }
}

void i2cScan() {
  Serial.println("I2C scan (full 0x00-0x7F grid; ## = device responds):");
  Serial.println("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
  uint8_t found = 0;
  for (uint8_t row = 0; row < 0x80; row += 0x10) {
    Serial.printf("%02x:", row);
    for (uint8_t col = 0; col < 0x10; col++) {
      uint8_t a = row + col;
      if (a < 0x08 || a >= 0x78) {        // reserved addresses — never probed
        Serial.print("   ");
      } else if (i2cPing(a)) {
        Serial.print(" ##");
        found++;
      } else {
        Serial.print(" --");
      }
    }
    Serial.println();
  }
  Serial.printf("  %u device(s) found:\n", found);
  for (uint8_t a = 0x08; a < 0x78; a++) {
    if (i2cPing(a)) {
      const char *name = i2cDeviceName(a);
      Serial.printf("  - 0x%02X  %s\n", a, name ? name : "(unknown device)");
    }
  }

  // Echo the addresses the firmware expects so a missing/relocated device is
  // obvious against the scan list above — update these defines in pins.h.
  Serial.printf("  configured: touch=0x%02X  accel=0x%02X/0x%02X  rtc=0x%02X\n",
                I2C_ADDR_TOUCH, I2C_ADDR_MMA8451_A, I2C_ADDR_MMA8451_B,
                I2C_ADDR_RV3028);
}

void probeRV3028() {
  if (!i2cPing(I2C_ADDR_RV3028)) {
    Serial.println("RV-3028 : not found");
    return;
  }
  uint8_t id;
  if (i2cReadReg(I2C_ADDR_RV3028, 0x28, id)) {           // ID register
    Serial.printf("RV-3028 : @0x%02X  ID=0x%02X (HID=0x%X VID=0x%X)\n",
                  I2C_ADDR_RV3028, id, id >> 4, id & 0x0F);
  } else {
    Serial.println("RV-3028 : ACK but read failed");
  }
}

void probeMMA8451() {
  uint8_t addr = 0;
  if      (i2cPing(I2C_ADDR_MMA8451_A)) addr = I2C_ADDR_MMA8451_A;
  else if (i2cPing(I2C_ADDR_MMA8451_B)) addr = I2C_ADDR_MMA8451_B;
  if (!addr) {
    Serial.println("MMA8451 : not found");
    return;
  }
  uint8_t id;
  if (i2cReadReg(addr, 0x0D, id)) {                      // WHO_AM_I
    const char *v = (id == 0x1A) ? "OK" : "WRONG ID";
    Serial.printf("MMA8451 : @0x%02X  WHO_AM_I=0x%02X  %s\n", addr, id, v);
  } else {
    Serial.printf("MMA8451 : @0x%02X  ACK but read failed\n", addr);
  }
}
