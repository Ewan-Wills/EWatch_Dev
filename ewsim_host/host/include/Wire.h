// ewsim_host: I2C bus with emulated RV-3028 (RTC from the simulated clock),
// MMA8451 (flat, face-up unless ewsim tilts it) and CST816S (no finger:
// simulated touches come in through ewsim, like ewt on the real watch).
#pragma once
#include <Arduino.h>
class TwoWire : public Stream {
public:
  bool begin(int sda = -1, int scl = -1, uint32_t freq = 0);
  bool begin(uint8_t addr, int sda, int scl, uint32_t freq) { (void)addr; return begin(sda, scl, freq); }
  bool end() { return true; }
  bool setClock(uint32_t) { return true; }
  void setTimeOut(uint16_t) {}
  uint16_t getTimeOut() { return 50; }
  void beginTransmission(uint16_t address);
  void beginTransmission(uint8_t address) { beginTransmission((uint16_t)address); }
  void beginTransmission(int address) { beginTransmission((uint16_t)address); }
  uint8_t endTransmission(bool sendStop);
  uint8_t endTransmission(void) { return endTransmission(true); }
  size_t requestFrom(uint16_t address, size_t size, bool sendStop);
  uint8_t requestFrom(uint16_t address, uint8_t size, bool sendStop) { return (uint8_t)requestFrom(address, (size_t)size, sendStop); }
  uint8_t requestFrom(uint16_t address, uint8_t size, uint8_t sendStop) { return (uint8_t)requestFrom(address, (size_t)size, (bool)sendStop); }
  size_t requestFrom(uint8_t address, size_t len, bool stopBit) { return requestFrom((uint16_t)address, len, stopBit); }
  uint8_t requestFrom(uint16_t address, uint8_t size) { return requestFrom(address, size, true); }
  uint8_t requestFrom(uint8_t address, uint8_t size, uint8_t sendStop) { return requestFrom((uint16_t)address, size, sendStop); }
  uint8_t requestFrom(uint8_t address, uint8_t size) { return requestFrom((uint16_t)address, size, true); }
  uint8_t requestFrom(int address, int size, int sendStop) { return requestFrom((uint16_t)address, (uint8_t)size, (bool)sendStop); }
  uint8_t requestFrom(int address, int size) { return requestFrom((uint16_t)address, (uint8_t)size, true); }
  size_t write(uint8_t) override;
  size_t write(const uint8_t *, size_t) override;
  int available(void) override;
  int read(void) override;
  int peek(void) override;
  void flush(void) override {}
  inline size_t write(const char *s) { return write((uint8_t *)s, strlen(s)); }
  inline size_t write(unsigned long n) { return write((uint8_t)n); }
  inline size_t write(long n) { return write((uint8_t)n); }
  inline size_t write(unsigned int n) { return write((uint8_t)n); }
  inline size_t write(int n) { return write((uint8_t)n); }
private:
  uint16_t txAddr = 0;
  uint8_t txBuf[64];
  size_t txLen = 0;
  uint8_t rxBuf[64];
  size_t rxLen = 0, rxPos = 0;
  int lastReg = -1;          // register pointer from a write without stop
};
extern TwoWire Wire;
extern TwoWire Wire1;
