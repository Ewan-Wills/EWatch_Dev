// Structured diagnostic logging.
//
// Format (one line per event, machine-parseable):
//   [<uptime_ms>|<state>] TAG: key=val key=val ...
//
// Tags: PWR WAKE WIFI TOUCH BAT SYS ERR
// Levels: ERROR(0) WARN(1) INFO(2) DEBUG(3).
//   * Compile-time floor: -DEW_LOG_LEVEL=n strips higher levels from the
//     binary entirely (macros compile to nothing).
//   * Runtime floor: NVS "logLvl", changeable live via `log level <n>`.
//
// Sinks:
//   a) USB serial (when connected),
//   b) persistent ring in LittleFS (two 128 KB halves, oldest-overwrite),
//   c) the last ~2 KB mirrored in RTC RAM so a crash / deep sleep keeps
//      context; recovered into the FS ring + serial on the next boot.
//
// Threading: call from task context only — never from an ISR (ISRs set
// flags/timestamps; the owning task logs). All sinks are serialised by an
// internal mutex.
//
// Serial console (line commands on USB): log dump | log clear |
// log level <n> | pwr stats | pwr selftest | help.
#pragma once
#include <Arduino.h>

enum : uint8_t {
  EWLOG_ERROR = 0,
  EWLOG_WARN  = 1,
  EWLOG_INFO  = 2,
  EWLOG_DEBUG = 3,
};

#ifndef EW_LOG_LEVEL
#define EW_LOG_LEVEL 3        // compile-time floor: keep everything by default
#endif

// Call order at boot: ewlogInit() right after Serial.begin (serial + RTC
// mirror only), ewlogMountFs() once it's safe to touch flash, then
// ewlogStartConsole() after tasks exist.
void ewlogInit();
void ewlogMountFs();
void ewlogStartConsole();

// Invalidate the RTC-RAM mirror without touching any other subsystem. Safe
// to call before ewlogInit() — the crash-loop guard uses it to guarantee a
// clean slate when it breaks a reboot loop.
void ewlogWipeRtcMirror();

// Current power state shown in every line's prefix ("BOOT" until set).
void ewlogSetState(const char *stateAbbrev);

void ewlogSetRuntimeLevel(uint8_t level);   // clamps to 0..3
uint8_t ewlogRuntimeLevel();

// Core emitter — prefer the EWLOG* macros so disabled levels vanish at
// compile time.
void ewlogWrite(uint8_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#if EW_LOG_LEVEL >= 0
#define EWLOGE(tag, ...) ewlogWrite(EWLOG_ERROR, tag, __VA_ARGS__)
#else
#define EWLOGE(tag, ...) do {} while (0)
#endif
#if EW_LOG_LEVEL >= 1
#define EWLOGW(tag, ...) ewlogWrite(EWLOG_WARN, tag, __VA_ARGS__)
#else
#define EWLOGW(tag, ...) do {} while (0)
#endif
#if EW_LOG_LEVEL >= 2
#define EWLOGI(tag, ...) ewlogWrite(EWLOG_INFO, tag, __VA_ARGS__)
#else
#define EWLOGI(tag, ...) do {} while (0)
#endif
#if EW_LOG_LEVEL >= 3
#define EWLOGD(tag, ...) ewlogWrite(EWLOG_DEBUG, tag, __VA_ARGS__)
#else
#define EWLOGD(tag, ...) do {} while (0)
#endif
