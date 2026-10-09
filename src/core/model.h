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
  SettingsFont,     // sub-page: watch face font picker
  SettingsHaptics,  // sub-page: vibration strength
  SettingsMemory,   // sub-page: heap / PSRAM / flash usage
  SettingsWifi,     // sub-page: WiFi enable + mode + status
  SettingsKnownNets,// sub-page: list of saved networks (delete only)
  SensorTest, TouchGestures, ImuGestures,
  Viewer3D,         // user app (outside src/apps/system)
  Media,            // user app: image / video viewer
  QRCode,           // user app: phone / email / website share QR
  Stopwatch,        // user app: count-up stopwatch
  Timer,            // user app: countdown timer (wakes from sleep)
  PowerOff
};

enum class WifiMode : uint8_t { AP = 0, Client = 1 };

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
  uint8_t  brightness  = 200;       // backlight PWM duty 0..255 (16 = ~6%)
  uint16_t bgColor     = 0x0000;    // RGB565 — BLACK (screen background)
  uint16_t fgColor     = 0xFFFF;    // RGB565 — WHITE (primary text / chrome)
  uint16_t accentColor = 0x000F;    // RGB565 — NAVY  (buttons, app cards, highlights)
  uint16_t lineColor   = 0x7BEF;    // RGB565 — DARKGREY (dividers, outlines)

  // Haptic feedback strength as a percentage (0 = off, 100 = full motor).
  // Every hapticBuzz() call scales its PWM intensity by this factor, so this
  // single slider tames every alert/UI buzz on the device.
  uint8_t  hapticStrength = 100;

  // Local-time offset from UTC in minutes. Used by the NTP sync button to
  // convert SNTP's UTC reading into local time before writing the RTC.
  // -720..+840 covers every real-world timezone including half-hour offsets.
  // Default 60 = UK BST. Saved over NVS, so any change persists across boots.
  int16_t  tzOffsetMin = 60;

  // Watch face style index. Indexes into kWatchFaceStyles in view.cpp; each
  // style is a size + draw-mode profile for the time / seconds / date lines.
  // Stored as uint8_t so the enum can grow without breaking the persisted
  // schema; the watch face clamps invalid values back to 0.
  uint8_t  watchFaceStyle = 0;

  // WiFi configuration (persisted).
  bool     wifiEnabled = false;
  WifiMode wifiMode    = WifiMode::AP;

  // WiFi live status — owned by wifi_svc, read by views / icon. Connected
  // means STA mode is associated; apClients counts SoftAP stations.
  bool     wifiConnected = false;
  int8_t   wifiRssi      = 0;
  char     wifiSsid[33]  = "";       // STA-connected SSID or AP SSID
  uint8_t  wifiApClients = 0;
  // Current radio IPv4 (0 = none). softAP IP in AP mode, DHCP-assigned IP in
  // STA mode. Stored as IPAddress::operator uint32_t() (little-endian bytes).
  uint32_t wifiIpV4      = 0;
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
