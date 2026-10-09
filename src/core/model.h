// Shared application state for the pseudo-3D racer. All fields are guarded by
// `modelMutex` — readers and writers must hold a ModelLock for the duration of
// their access. Fields are pure data; I/O lives in controller.cpp and the view.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// OS-level screens. The game runs as a single Racer view; its own start /
// playing / game-over states are managed internally by RacerView, not here.
enum class Screen : uint8_t {
  Racer,
};

struct Model {
  // Accelerometer (raw 14-bit signed, ±2g). Tilt steering reads ax/ay.
  int16_t ax = 0, ay = 0, az = 0;
  bool    imuOk = false;

  // Button (PIN_BTN active-high) — boost / brake.
  bool button = false;

  // Active screen.
  Screen screen = Screen::Racer;

  // Bumped by the controller on any field change to nudge the renderer.
  uint32_t revision = 0;

  // Wake sources from deep sleep.
  bool wakeOnTouch  = true;
  bool wakeOnButton = true;
  bool wakeOnImu    = false;

  // Auto-sleep: enter deep sleep after this many seconds of no input.
  // 0 disables auto-sleep entirely — the game sets this while actively playing.
  uint16_t sleepTimeoutSec = 30;

  // After this many seconds in deep sleep with no wake event, drop the LDO
  // latch and fully power off. 0 disables (sleep forever until wake source).
  uint16_t sleepToOffSec = 30;

  // MMA8451 TRANSIENT_THS register value used for IMU wake-on-jolt. Each LSB
  // is 0.063 g; default 0x20 = ~2.0 g.
  uint8_t  imuWakeThreshold = 0x20;

  // Display backlight PWM duty 0..255.
  uint8_t  brightness = 200;

  // Haptic feedback strength as a percentage (0 = off, 100 = full motor).
  // Every hapticBuzz() call scales its PWM intensity by this factor.
  uint8_t  hapticStrength = 100;

  // Best distance travelled, in metres. Persisted to NVS by Storage.
  uint32_t bestDistance = 0;
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
