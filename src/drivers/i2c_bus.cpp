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

void i2cScan() {
  Serial.println("I2C scan:");
  uint8_t found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    if (i2cPing(a)) {
      Serial.printf("  - 0x%02X\n", a);
      found++;
    }
  }
  Serial.printf("  %u device(s) found\n", found);
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
