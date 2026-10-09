// ewsim_host: USB-CDC console (Serial). The script runner writes console lines
// into the RX buffer (exactly what ewt sends over USB) and reads the watch's
// replies from the TX log.
#pragma once
#include "Stream.h"
class HWCDC : public Stream {
public:
  size_t setRxBufferSize(size_t n) { return n; }
  size_t setTxBufferSize(size_t n) { return n; }
  void setTxTimeoutMs(uint32_t) {}
  void begin(unsigned long baud = 0) { (void)baud; }
  void end() {}
  int available(void) override;
  int availableForWrite(void) override { return 4096; }
  int peek(void) override;
  int read(void) override;
  size_t read(uint8_t *buffer, size_t size);
  size_t write(uint8_t) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  inline size_t read(char *buffer, size_t size) { return read((uint8_t *)buffer, size); }
  inline size_t write(const char *buffer, size_t size) { return write((const uint8_t *)buffer, size); }
  inline size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  inline size_t write(unsigned long n) { return write((uint8_t)n); }
  inline size_t write(long n) { return write((uint8_t)n); }
  inline size_t write(unsigned int n) { return write((uint8_t)n); }
  inline size_t write(int n) { return write((uint8_t)n); }
  void flush(void) override {}
  static bool isPlugged(void) { return true; }     // USB host present (dev mode)
  static bool isConnected(void) { return true; }
  static bool isCDC_Connected(void) { return true; }
  operator bool() const { return true; }
  void setDebugOutput(bool) {}
};
extern HWCDC Serial;
typedef HWCDC HardwareSerial;
extern HWCDC Serial0;
