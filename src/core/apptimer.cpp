#include "apptimer.h"
#include <esp_attr.h>

// RTC_DATA_ATTR keeps these alive across deep sleep (but not a full power-off).
RTC_DATA_ATTR static bool     gTimerArmed    = false;
RTC_DATA_ATTR static bool     gTimerFired    = false;
RTC_DATA_ATTR static uint32_t gTimerDeadline = 0;
RTC_DATA_ATTR static uint32_t gTimerDuration = 0;

RTC_DATA_ATTR static bool     gSwRunning  = false;
RTC_DATA_ATTR static uint32_t gSwAccumMs  = 0;     // ms from completed segments
RTC_DATA_ATTR static uint32_t gSwSegEpoch = 0;     // RTC epoch at segment start

// Per-boot only (lost across deep sleep): millis() base for the live segment.
static uint32_t gSwSegMs      = 0;
static bool     gSwSegMsValid = false;

static bool isLeap(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

uint32_t rtcEpochSec(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second) {
  static const uint16_t cum[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
  uint32_t days = 0;
  for (uint16_t y = 2000; y < year; y++) days += isLeap(y) ? 366u : 365u;
  if (month >= 1 && month <= 12) {
    days += cum[month - 1];
    if (month > 2 && isLeap(year)) days += 1;
  }
  if (day > 0) days += (uint32_t)(day - 1);
  return days * 86400u + (uint32_t)hour * 3600u
       + (uint32_t)minute * 60u + (uint32_t)second;
}

// ---------- Timer ----------
void timerArm(uint32_t nowEpoch, uint32_t durationSec) {
  gTimerArmed    = true;
  gTimerFired    = false;
  gTimerDuration = durationSec;
  gTimerDeadline = nowEpoch + durationSec;
}
void timerCancel() {
  gTimerArmed = false;
  gTimerFired = false;
}
bool     timerArmed()         { return gTimerArmed; }
uint32_t timerDeadlineEpoch() { return gTimerDeadline; }
uint32_t timerDurationSec()   { return gTimerDuration; }
uint32_t timerRemainingSec(uint32_t nowEpoch) {
  if (!gTimerArmed) return 0;
  if (nowEpoch >= gTimerDeadline) return 0;
  return gTimerDeadline - nowEpoch;
}
void timerMarkFired() {
  gTimerArmed = false;
  gTimerFired = true;
}
bool timerFired()      { return gTimerFired; }
void timerClearFired() { gTimerFired = false; }

// ---------- Stopwatch ----------
void stopwatchStart(uint32_t nowEpoch, uint32_t nowMs) {
  if (gSwRunning) return;
  gSwRunning    = true;
  gSwSegEpoch   = nowEpoch;
  gSwSegMs      = nowMs;
  gSwSegMsValid = true;
}
void stopwatchStop(uint32_t nowEpoch, uint32_t nowMs) {
  if (!gSwRunning) return;
  if (gSwSegMsValid && nowMs >= gSwSegMs) {
    gSwAccumMs += nowMs - gSwSegMs;                       // precise ms
  } else if (nowEpoch >= gSwSegEpoch) {
    gSwAccumMs += (nowEpoch - gSwSegEpoch) * 1000u;       // slept: second res
  }
  gSwRunning    = false;
  gSwSegMsValid = false;
}
void stopwatchReset() {
  gSwRunning    = false;
  gSwAccumMs    = 0;
  gSwSegEpoch   = 0;
  gSwSegMsValid = false;
}
bool stopwatchRunning() { return gSwRunning; }
uint32_t stopwatchElapsedMs(uint32_t nowEpoch, uint32_t nowMs) {
  if (!gSwRunning) return gSwAccumMs;
  // First read after a wake: the millis() base is stale. Fold the slept span
  // into the accumulator (second resolution) and restart an ms-precise
  // segment from now.
  if (!gSwSegMsValid) {
    if (nowEpoch >= gSwSegEpoch) gSwAccumMs += (nowEpoch - gSwSegEpoch) * 1000u;
    gSwSegEpoch   = nowEpoch;
    gSwSegMs      = nowMs;
    gSwSegMsValid = true;
  }
  uint32_t seg = (nowMs >= gSwSegMs) ? (nowMs - gSwSegMs) : 0;
  return gSwAccumMs + seg;
}
