// Input events — produced by the controller, consumed by the active view.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class EventType : uint8_t {
  Touch,         // finger-down (first contact)
  TouchHold,     // finger still on screen (~30 Hz while held)
  TouchUp,       // finger released
  Gesture,       // swipe / double-click / long-press from CST816S
  ButtonDown,    // raw SW2 transition
  ButtonUp,
  ButtonShort,   // released after < 1000 ms — used for "back"
  ButtonVeryLong,// held > 3000 ms — global power-off
  ImuMotion,     // detected motion (raised from accel delta heuristic)
  Tick,
};

// Maps to fbiego/CST816S gestureID values.
enum class Gesture : uint8_t {
  None        = 0x00,
  SwipeDown   = 0x01,
  SwipeUp     = 0x02,
  SwipeLeft   = 0x03,
  SwipeRight  = 0x04,
  SingleTap   = 0x05,
  DoubleTap   = 0x0B,
  LongPress   = 0x0C,
};

struct Event {
  EventType type;
  uint16_t  x;
  uint16_t  y;
  Gesture   gesture;
};

static inline Event makeEvent(EventType t) {
  Event e; e.type = t; e.x = 0; e.y = 0; e.gesture = Gesture::None;
  return e;
}

extern QueueHandle_t eventQueue;
void eventQueueInit();
void postEvent(const Event &e);
