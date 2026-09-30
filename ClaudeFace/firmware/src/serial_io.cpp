// USB serial: write mutex, logging, and the link / console task. See
// serial_io.h. Replaces AuroraOS' ewlog console for this firmware.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "serial_io.h"
#include "cf_link.h"

static SemaphoreHandle_t sMutex = nullptr;

void serialBegin() {
  // The bridge sends STATE + SESSIONS back to back (~280 B); the default
  // 256 B RX queue would drop the tail between polls. Must precede begin()
  // (resizing later races the USB ISR).
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  sMutex = xSemaphoreCreateRecursiveMutex();
}

SerialLock::SerialLock() { if (sMutex) xSemaphoreTakeRecursive(sMutex, portMAX_DELAY); }
SerialLock::~SerialLock() { if (sMutex) xSemaphoreGiveRecursive(sMutex); }

void serialWriteRaw(const char *line, size_t n) {
  if (!line || !n) return;
  SerialLock lk;
  if (Serial) Serial.write((const uint8_t *)line, n);
}

void logf(const char *tag, const char *fmt, ...) {
  char buf[200];
  int n = snprintf(buf, sizeof buf, "[%lu.%03lu] %s ",
                   (unsigned long)(millis() / 1000UL),
                   (unsigned long)(millis() % 1000UL), tag);
  if (n < 0) return;
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof buf - 2) len = sizeof buf - 2;   // truncated: keep the newline
  buf[len++] = '\n';
  serialWriteRaw(buf, len);
}

// ---------------------------------------------------------------------------
// Link / console task
// ---------------------------------------------------------------------------
static void handleLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  // Claude Face frames first: they are the traffic that matters.
  if (strncmp(line, "#CF", 3) == 0) { cfLinkFeedLine(line); return; }
  if (cfLinkConsole(line)) return;          // "cf ..." commands
  SerialLock lk;
  if (strcmp(line, "help") != 0) Serial.printf("unknown: %s\n", line);
  Serial.println("commands: cf stats | cf set quiet <start> <end> | "
                 "cf set ambient <0-100> | cf set haptics <0|1> | "
                 "cf buzz <blocked|done|cross|reset> | cf hello");
}

static void linkTask(void *) {
  esp_task_wdt_add(nullptr);
  static char line[320];                    // STATE frame ~170 chars
  size_t pos = 0;
  bool overflow = false;
  for (;;) {
    esp_task_wdt_reset();
    cfLinkService();                        // HELLO on attach + queued EVENTs
    while (Serial && Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (!overflow && pos) { line[pos] = '\0'; handleLine(line); }
        pos = 0;
        overflow = false;
      } else if (pos < sizeof(line) - 1) {
        line[pos++] = c;
      } else {
        overflow = true;                    // drop the whole over-long line
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void serialStartLinkTask() {
  cfLinkInit();
  // Core 0, below the UI loop's priority on core 1. 6 KiB: frame parsing,
  // printf-heavy `cf stats`, NVS writes from `cf set`.
  xTaskCreatePinnedToCore(linkTask, "cflink", 6144, nullptr, 1, nullptr, 0);
}
