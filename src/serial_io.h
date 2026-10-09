// USB serial for the stand-alone Claude Face firmware.
//
// One USB-CDC port carries three things: the bridge's "#CF" protocol lines,
// a tiny console ("cf ...", "help"), and log lines. Every write to Serial
// goes through ONE recursive mutex (SerialLock), and each line is written
// in a single call, so a log line can never land in the middle of a "#CF"
// frame (the bridge would drop both).
//
// The link task (serialStartLinkTask) is the only reader: it assembles
// lines (320-byte buffer: a STATE frame is ~170 chars), hands "#CF" lines to
// cf_link and the rest to the console, and runs cfLinkService() (HELLO on
// boot / USB re-attach, queued EVENTs).
#pragma once
#include <stddef.h>
#include <stdint.h>

void serialBegin();                        // RX buffer 1024 + begin + mutex
void serialStartLinkTask();                // core 0, feeds its own task WDT

// RAII: hold the Serial write mutex for a multi-line block (recursive, so
// logf / serialWriteRaw inside it are fine).
class SerialLock {
public:
  SerialLock();
  ~SerialLock();
private:
  SerialLock(const SerialLock &);
  SerialLock &operator=(const SerialLock &);
};

// One pre-formatted line (must end in '\n'), one write, under the mutex.
// Dropped when no USB host is attached (no blocking on a dead port).
void serialWriteRaw(const char *line, size_t n);

// "[uptime_s] TAG message\n", one write, under the mutex.
void logf(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
