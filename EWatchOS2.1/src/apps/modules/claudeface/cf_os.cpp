// ClaudeFace on EWatchOS: adapters for cf_board.h / cf_serial.h.
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "cf_board.h"
#include "cf_serial.h"
#include "controller.h"   // requestSetRTC
#include "ewlog.h"

void rtcRequestSet(const RtcTime &t) {
  requestSetRTC(t.hour, t.minute, t.second, t.weekday, t.day, t.month, t.year);
}

static SemaphoreHandle_t sMux = nullptr;
SerialLock::SerialLock() {
  if (!sMux) sMux = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(sMux, portMAX_DELAY);
}
SerialLock::~SerialLock() { xSemaphoreGiveRecursive(sMux); }

void serialWriteRaw(const char *line, size_t n) {
  SerialLock lk;
  Serial.write((const uint8_t *)line, n);
}

void logf(const char *tag, const char *fmt, ...) {
  char buf[200];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  EWLOGI(tag, "%s", buf);
}
