// ClaudeFace on EWatchOS: serial output for the link. Input arrives through
// the ewlog console (core/ewlog.cpp routes "#CF" lines and "cf ..." commands).
#pragma once
#include <Arduino.h>

class SerialLock {          // keeps a multi-line reply in one piece
public:
  SerialLock();
  ~SerialLock();
  SerialLock(const SerialLock &) = delete;
  SerialLock &operator=(const SerialLock &) = delete;
};
void serialWriteRaw(const char *line, size_t n);
void logf(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
