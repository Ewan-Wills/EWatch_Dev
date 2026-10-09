// EWatch v2 — DOOM. The watch's normal feature set has been stripped out;
// this firmware boots the hardware and runs a Doom-style raycaster game.
//
// main.cpp keeps only what the device needs to come up safely and stay
// recoverable: the LDO power latch, the panic-loop guard, the task watchdog,
// and the display/touch/haptic bring-up. Everything game-related lives in
// game/doom.cpp; the render+input loop runs from loop() below.

#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <driver/gpio.h>

#include "pins.h"
#include "power.h"
#include "i2c_bus.h"
#include "display.h"
#include "touch.h"
#include "haptic.h"
#include "doom.h"

// Map ESP reset reasons to short strings for the serial log.
static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "?";
  }
}
static bool isCrashReason(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT;
}

// ----- Power / watchdog safety nets -----------------------------------------
// The watch holds its own power via PIN_LDO_LATCH, so a firmware lock-up can't
// be cleared without pulling the battery. Two defenses, carried over from the
// watch firmware:
//   1. Task WDT — the game loop feeds it every frame; a stall resets the chip.
//   2. Reset-loop guard — a counter in RTC memory increments on every crash
//      boot. Past kPanicGiveUpLimit we drop the rail so the watch powers off
//      instead of sitting in a hung reboot loop.
//   3. Stable-run timer clears the counter once we've been alive kStableSec.
RTC_DATA_ATTR static uint32_t gConsecutivePanics = 0;
static const uint32_t kPanicGiveUpLimit = 3;
static const uint32_t kTaskWdtSec       = 20;
static const uint32_t kStableSec        = 30;

static void checkPanicLoopAndMaybeShutdown(esp_reset_reason_t r) {
  if (!isCrashReason(r)) { gConsecutivePanics = 0; return; }
  gConsecutivePanics++;
  Serial.printf("Panic-loop counter: %lu / %lu (reason=%s)\n",
                (unsigned long)gConsecutivePanics,
                (unsigned long)kPanicGiveUpLimit, resetReasonStr(r));
  if (gConsecutivePanics > kPanicGiveUpLimit) {
    Serial.println("!!! Too many consecutive panics — dropping LDO latch !!!");
    Serial.flush();
    unlatchPower();
    for (;;) delay(100);
  }
}

static void panicCounterStableReset(void *) { gConsecutivePanics = 0; }
static void armPanicCounterReset() {
  static esp_timer_handle_t h = nullptr;
  if (h) return;
  esp_timer_create_args_t args = {};
  args.callback        = &panicCounterStableReset;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name            = "panicrst";
  esp_timer_create(&args, &h);
  esp_timer_start_once(h, (uint64_t)kStableSec * 1000000ULL);
}

static void installTaskWatchdog() {
  esp_task_wdt_init(kTaskWdtSec, /*panic=*/true);
  esp_task_wdt_add(nullptr);   // subscribe loopTask (where the game loop runs)
}

void setup() {
  // FIRST: hold the power rail up. Until this runs the watch is alive only
  // for as long as the user keeps SW2 pressed.
  latchPower();

  // If the last few boots all crashed, power off instead of looping forever.
  esp_reset_reason_t resetReason = esp_reset_reason();
  checkPanicLoopAndMaybeShutdown(resetReason);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 300) delay(10);
  Serial.println("\n=== EWatch DOOM boot ===");

  i2cBegin();

  // The CST816S sleeps at cold boot; pulse RST so it ACKs the scan.
  touchReset();
  i2cScan();

  hapticBegin();
  hapticSetStrengthPct(100);

  if (!displayBegin()) {
    Serial.println("display init failed; halting");
    while (true) delay(1000);
  }

  touchBegin();

  // Arm the watchdog before the game loop starts feeding it.
  installTaskWatchdog();

  doomInit();

  // First frame is painted by doomInit(); bring the backlight up so the user
  // never sees the panel's power-on garbage.
  backlightSet(220);
  hapticBuzz(120, 60);

  armPanicCounterReset();
  Serial.println("Setup complete; DOOM running.");
}

void loop() {
  esp_task_wdt_reset();   // feed the watchdog every frame
  doomFrame();
}
