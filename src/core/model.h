// Shared application state. All fields are guarded by `modelMutex` — readers
// and writers must hold a ModelLock for the duration of their access. Fields
// are pure data; I/O lives in controller.cpp and views.
//
// This is the FirstOS model trimmed to what BasicDigital needs: time/date,
// the sensors the controller samples, and the power/sleep/display/haptic
// preferences the boot + sleep machinery depends on. WiFi, timezone and
// watch-face-style fields are gone — BasicDigital has no UI for them.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class Screen : uint8_t {
  Watch,            // digital watch face
  Settings,         // tiny menu: Set Time / Set Date / Sleep / Power Off
  SettingsTime,     // sub-page: clock set
  SettingsDate,     // sub-page: calendar set
  SettingsSleep,    // sub-page: auto-sleep + auto-power-off timeouts
};

struct Model {
  // Time (RTC)
  uint8_t hour = 0, minute = 0, second = 0;
  // Date (RTC). year stored as full 4-digit value (e.g. 2026).
  uint16_t year = 2025;
  uint8_t  month = 1, day = 1;
  uint8_t  weekday = 0;   // 0=Sun .. 6=Sat (RV-3028 convention varies; uses whatever was set)
  bool    rtcOk = false;

  // Accelerometer (raw 14-bit signed, ±2g)
  int16_t ax = 0, ay = 0, az = 0;
  bool    imuOk = false;

  // Battery
  float   vbat = 0.f;
  uint8_t batPct = 0;
  bool    batOk = false;

  // Button (PIN_BTN active-high)
  bool button = false;

  // Active screen
  Screen screen = Screen::Watch;

  // Bumped by the controller on any field change to nudge the renderer.
  uint32_t revision = 0;

  // Wake sources from deep sleep — used by enterDeepSleep(). BasicDigital has
  // no settings page to toggle these, so all three are on by default: the
  // watch wakes on a screen tap, an SW2 press, or a wrist jolt (IMU), exactly
  // as FirstOS does once every source is enabled.
  bool wakeOnTouch  = true;
  bool wakeOnButton = true;
  bool wakeOnImu    = true;

  // Auto-sleep: enter deep sleep after this many seconds of no input.
  // 0 disables auto-sleep entirely.
  uint16_t sleepTimeoutSec = 30;

  // After this many seconds in deep sleep with no wake event, drop the LDO
  // latch and fully power off. 0 disables it — BasicDigital stays in wakeable
  // deep sleep indefinitely so touch / IMU / button can always bring it back.
  // (A full power-off is still reachable via the Power Off menu or a 3 s SW2
  // hold; after that only SW2 revives it, which is the intended behaviour.)
  uint16_t sleepToOffSec = 0;

  // MMA8451 TRANSIENT_THS register value used for IMU wake-on-jolt. Each LSB
  // is 0.063 g; default 0x20 = ~2.0 g.
  uint8_t  imuWakeThreshold = 0x20;

  // Display preferences.
  uint8_t  brightness  = 200;       // backlight PWM duty 0..255 (16 = ~6%)
  uint16_t bgColor     = 0x0000;    // RGB565 — BLACK (screen background)
  uint16_t fgColor     = 0xFFFF;    // RGB565 — WHITE (primary text / chrome)
  uint16_t accentColor = 0x000F;    // RGB565 — NAVY  (buttons, highlights)
  uint16_t lineColor   = 0x7BEF;    // RGB565 — DARKGREY (dividers, outlines)

  // Haptic feedback strength as a percentage (0 = off, 100 = full motor).
  // Every hapticBuzz() call scales its PWM intensity by this factor.
  uint8_t  hapticStrength = 100;
};

extern Model            model;
extern SemaphoreHandle_t modelMutex;

void modelInit();

// RAII scoped mutex.
class ModelLock {
public:
  ModelLock()  { xSemaphoreTake(modelMutex, portMAX_DELAY); }
  ~ModelLock() { xSemaphoreGive(modelMutex); }
  ModelLock(const ModelLock&) = delete;
  ModelLock& operator=(const ModelLock&) = delete;
};
