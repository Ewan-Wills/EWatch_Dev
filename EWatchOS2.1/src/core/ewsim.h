// ewsim — host-driven test hooks (autonomous testing over USB serial).
//
// Lets a PC script drive the watch with no hands on it:
//   * simulated finger (tap / drag / swipe / long press), CST816S gesture
//     codes, side button (short / long / very long / raw down-up), accel
//     override (tilt) and shake — all injected at the controller's input
//     layer, so they go through exactly the same state machines as the real
//     sensors (swipe-right -> back, ButtonShort timing, ImuMotion heuristic,
//     activity / wake handling).
//   * `snap`: a screenshot. A tap on the display bus mirrors every pixel the
//     ST7789 receives into a 240x280 PSRAM shadow, so it works for views that
//     draw straight to gfx as well as canvas views.
//   * `state`: one-line status (screen id, power state, backlight, heap,
//     pixels pushed to the panel) for cheap assertions.
//   * `goto <screen id>`: jump straight to a screen (render task switches).
//
// Console protocol (lines on the ewlog console, see core/ewlog.cpp):
//   sim <seq> <cmd> [args]   ->  "S:<seq> OK ..." | "S:<seq> ERR ..."
// Commands block until the input sequence has finished, so the host can
// script deterministic steps. Host tool: Claude Knowledge Base
// firmware/knowledge/ewt.py (see ewatch-test-harness.md).
//
// Compile out with -DEW_SIM=0 (the bus tap then costs nothing).
#pragma once
#include <Arduino.h>
#include "event.h"

#ifndef EW_SIM
#define EW_SIM 1
#endif

class Arduino_DataBus;

// display.cpp: create the panel bus (a tapping bus when EW_SIM).
Arduino_DataBus *ewsimMakeBus(int8_t dc, int8_t cs, int8_t sck, int8_t mosi,
                              int8_t miso, uint8_t spiNum);

// ewlog console: returns true if the line was a `sim ...` command.
bool ewsimHandleLine(char *line);

#if EW_SIM
void ewsimTapPause(bool paused);   // count pixels but skip the shadow copy (fast blits)
bool ewsimStreaming();             // a snapshot is streaming: heavy views should idle + not log
// ---- controller.cpp taskIO hooks (called every 20 ms I/O cycle) ----
// Overrides the polled finger state while a simulated finger is down.
void ewsimTouch(bool &live, uint16_t &x, uint16_t &y);
// A pending simulated gesture (consumed once), else Gesture::None.
Gesture ewsimTakeGesture(uint16_t &x, uint16_t &y);
// Simulated side button (OR-ed with the real pin).
bool ewsimButton();
// Accel override while `tilt` / `shake` is active.
void ewsimAccel(bool &ok, int16_t &ax, int16_t &ay, int16_t &az);
// ---- controller.cpp taskRender hook: pending `goto`, else -1 ----
int  ewsimTakeGoto();
// ---- power_mgr.cpp screen-off wait: host asked the watch to wake ----
bool ewsimTakeWake();
#else
static inline void    ewsimTapPause(bool) {}
static inline bool    ewsimStreaming() { return false; }
static inline void    ewsimTouch(bool &, uint16_t &, uint16_t &) {}
static inline Gesture ewsimTakeGesture(uint16_t &, uint16_t &) { return Gesture::None; }
static inline bool    ewsimButton() { return false; }
static inline void    ewsimAccel(bool &, int16_t &, int16_t &, int16_t &) {}
static inline int     ewsimTakeGoto() { return -1; }
static inline bool    ewsimTakeWake() { return false; }
#endif
