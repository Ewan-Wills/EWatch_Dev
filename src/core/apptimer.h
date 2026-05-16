// Persistent state for the Timer and Stopwatch apps.
//
// Both apps anchor their timing to an RTC-derived epoch (seconds since
// 2000-01-01) instead of millis(), because millis() resets across deep
// sleep. The state lives in RTC_DATA_ATTR memory so it survives deep sleep
// (it is lost on a full power-off, which is acceptable).
//
// The countdown timer arms an absolute deadline; the controller's I/O task
// checks for expiry on every RTC read and enterDeepSleep() arms a hardware
// timer wake-up so the watch can alarm even while asleep.
#pragma once
#include <stdint.h>

// Seconds since 2000-01-01 00:00:00 from RTC fields. Monotonic as long as the
// RTC is set sensibly; used as the common time base for both apps.
uint32_t rtcEpochSec(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second);

// ---------- Timer ----------
void     timerArm(uint32_t nowEpoch, uint32_t durationSec);
void     timerCancel();
bool     timerArmed();
uint32_t timerDeadlineEpoch();
uint32_t timerDurationSec();
uint32_t timerRemainingSec(uint32_t nowEpoch);   // 0 once the deadline passes
void     timerMarkFired();                        // armed -> fired
bool     timerFired();
void     timerClearFired();

// ---------- Stopwatch ----------
// Whole seconds are anchored to the RTC epoch so they survive deep sleep;
// the millisecond fraction comes from millis() and is precise only within a
// single boot. After a wake the running segment is re-based: the slept
// duration folds into the accumulator at second resolution and a fresh
// ms-precise segment starts. Callers pass both the RTC epoch and millis().
void     stopwatchStart(uint32_t nowEpoch, uint32_t nowMs);
void     stopwatchStop(uint32_t nowEpoch, uint32_t nowMs);
void     stopwatchReset();
bool     stopwatchRunning();
uint32_t stopwatchElapsedMs(uint32_t nowEpoch, uint32_t nowMs);
