// Shared application state. All fields are guarded by `modelMutex` — readers
// and writers must hold a ModelLock for the duration of their access. Fields
// are pure data; I/O lives in controller.cpp and views.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class Screen : uint8_t {
  Watch, AppList,
  SystemApps,       // sub-page of AppList — Settings/diagnostics/PowerOff
  Settings,         // sub-menu of settings sections
  SettingsTime,     // sub-page: clock set
  SettingsDate,     // sub-page: calendar set
  SettingsSleep,    // sub-page: idle timeout + wake sources
  SettingsDisplay,  // sub-page: brightness + colors
  SettingsMemory,   // sub-page: heap / PSRAM / flash usage
  SensorTest, TouchGestures, ImuGestures,
  Viewer3D,         // user app (outside src/apps/system)
  Media,            // user app: image / video viewer
  QRCode,           // user app: phone / email / website share QR
  AnimDemo,         // user app: anim framework showcase
  PowerOff
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

  // Wake sources from deep sleep — toggleable in Settings, used by Sleep app.
  bool wakeOnTouch  = true;
  bool wakeOnButton = true;
  bool wakeOnImu    = false;

  // Auto-sleep: enter deep sleep after this many seconds of no input.
  // 0 disables auto-sleep entirely.
  uint16_t sleepTimeoutSec = 5;

  // After this many seconds in deep sleep with no wake event, drop the LDO
  // latch and fully power off. 0 disables (sleep forever until wake source).
  uint16_t sleepToOffSec = 30;

  // MMA8451 TRANSIENT_THS register value used for IMU wake-on-jolt. Each LSB
  // is 0.063 g; default 0x20 = ~2.0 g. UI exposes this in 0.25 g steps.
  uint8_t  imuWakeThreshold = 0x20;

  // Display preferences.
  uint8_t  brightness = 200;        // backlight PWM duty 0..255 (16 = ~6%)
  uint16_t bgColor    = 0x0000;     // RGB565 — BLACK
  uint16_t fgColor    = 0xFFFF;     // RGB565 — WHITE
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
