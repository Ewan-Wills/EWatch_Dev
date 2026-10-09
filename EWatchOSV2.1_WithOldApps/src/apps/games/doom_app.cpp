// DoomView — EWatchOS adapter around the standalone doom.cpp game.
//
// Doom was written as a bare-metal firmware: it reads the CST816S touch point
// straight off the I2C bus and polls the side button itself. Under EWatchOS
// the I/O task owns that bus at 50 Hz, so the adapter parks it with
// controllerSuspendIo() for the whole session (the same cooperative pause the
// light-sleep path uses) and gives the bus to the game. While suspended no
// events are produced — which is fine, the game does its own input.
//
// The render loop never returns while the game is live (same pattern as
// Tunnel Racer), so it feeds the task WDT itself. Exit = hold the side button
// ~2 s (doomExitRequested()), which lands back on the app launcher.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "pins.h"
#include "event.h"
#include "view.h"
#include "display.h"
#include "controller.h"
#include "ewlog.h"
#include "doom.h"

class DoomView : public View {
public:
  // Nonzero so taskRender raises the CPU to 240 MHz before entering our
  // blocking loop (the raycaster was written for it; 80 MHz halves the fps).
  uint16_t desiredFrameMs() const override { return 33; }

  void onEnter() override {
    controllerSuspendIo();          // game owns the I2C bus + button now
    ioSuspended = true;
    doomInit();
    EWLOGI("APP", "doom enter");
  }

  void onExit() override {
    resumeIoIfHeld();
    EWLOGI("APP", "doom exit");
  }

  void render() override {
    // No canvas = doomInit bailed (PSRAM exhausted) and doomFrame() never
    // reaches its button poll — without this guard the watch is unexitable.
    if (!frameCanvas()) {
      delay(1500);                  // let the on-screen error be read
      exitToLauncher();
      return;
    }
    // Own the frame loop: ~30 fps, WDT fed, until the exit gesture.
    for (;;) {
      esp_task_wdt_reset();
      uint32_t t0 = millis();
      doomFrame();
      if (doomExitRequested()) {
        exitToLauncher();
        return;
      }
      int32_t budget = 33 - (int32_t)(millis() - t0);
      if (budget > 0) delay(budget);
    }
  }

  // Events can't arrive while the I/O task is parked; nothing to do here.
  void onEvent(const Event &) override {}

private:
  bool ioSuspended = false;
  void resumeIoIfHeld() {
    if (ioSuspended) { controllerResumeIo(); ioSuspended = false; }
  }

  // The exit hold fires while the button is still physically down. Resuming
  // taskIO at that moment makes it see button-high with no history and
  // synthesize a fresh ButtonDown/ButtonShort — which the launcher would eat
  // as "back", dumping the user on the watch face (or worse, a 3 s carry-over
  // hold becomes a surprise power-off). Wait for the release, drop anything
  // stale in the queue, THEN hand the bus back and switch.
  void exitToLauncher() {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BTN) && millis() - t0 < 4000) {
      esp_task_wdt_reset();
      delay(20);
    }
    if (eventQueue) xQueueReset(eventQueue);
    resumeIoIfHeld();
    switchTo(Screen::AppList);
  }
};

static DoomView gDoom;
View *doomViewPtr() { return &gDoom; }
