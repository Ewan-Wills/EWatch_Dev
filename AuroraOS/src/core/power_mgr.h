// Tiered power management — single source of truth for power states.
//
//   ACTIVE     screen on, user brightness. CPU 240 MHz only while an app
//              demands it (animated view), else 80 MHz.
//   IDLE       after T_DIM s of no input: backlight fades to 20%.
//   SCREEN_OFF after T_OFF s: backlight 0, panel DISPOFF (RAM retained),
//              touch in monitor mode, then light sleep. Wake sources: touch
//              INT, button, accel INT2 (raise-to-wake), RTC INT (minute
//              tick), plus a timer for the deep-sleep deadline / WiFi sync
//              windows. RAM + peripheral state retained; wake is the fast
//              path (target touch -> visible < 150 ms).
//   DEEP_SLEEP after T_DEEP min with no activity: full peripheral shutdown
//              and EXT1 deep sleep (wake = reboot; accepted).
//
// The state machine runs on the render task: call powerOnActivity() for
// every user input and powerTick() once per loop. powerTick() may block for
// the whole screen-off phase and may not return at all (deep sleep).
//
// Dev-mode guard: entering real light sleep kills USB CDC. When USB is
// connected (or -DEW_DEV_MODE=1), screen-off runs a delay()-based fake sleep
// that keeps serial alive and logs sleep=simulated.
#pragma once
#include <Arduino.h>

enum class PowerState : uint8_t { Active, Idle, ScreenOff, Deep };

struct PowerConfig {
  uint16_t dimSec;      // ACTIVE -> IDLE (default 8)
  uint16_t offSec;      // -> SCREEN_OFF; = model.sleepTimeoutSec (default 15)
  uint16_t deepMin;     // -> DEEP_SLEEP after this many minutes (default 45)
  uint16_t syncSec;     // WiFi sync-window period (default 300)
};

void        powerInit();               // load config, drop CPU to 80 MHz

// SAFE MODE (crash-loop give-up in main.cpp): disables the whole idle
// progression — no dim, no screensaver, no light/deep sleep. The screen
// stays on so the watch remains debuggable and flashable.
void        powerSetSafeMode(bool on);
void        powerOnActivity();         // any user input (render task only)
void        powerTick();               // render loop hook; may sleep / not return
PowerState  powerState();
const PowerConfig &powerConfig();

// CPU clock demand from the render loop: true while the active view runs an
// animation loop. De-bounced internally; logs SYS cpu_mhz on change.
void powerPerfDemand(bool need240);

// wifi_svc accounting: total associated time feeds the duty-cycle stat.
void powerNoteWifiAssoc(uint32_t assocMs);

// `pwr stats` / `pwr selftest` / `pwr set <key> <n>` (ewlog console).
void powerStatsPrint(Stream &out);
void powerRequestSelfTest();
bool powerSetConfig(const char *key, uint16_t val);   // dim|off|deep|sync

bool powerDevMode();                   // USB connected or EW_DEV_MODE build

// Awake hold (Claude Face, apps/claude): while held, the idle progression is
// suspended exactly like a screenBlocksSleep() screen — no dim, screensaver
// or screen-off — and the holder owns the backlight level. Render task only.
// Releasing restarts the normal timeouts from "now".
void powerHoldAwake(bool hold);

// Ask a dark screen to wake as if the user did (any task; e.g. the Claude
// link when an agent becomes BLOCKED). Serviced by the screen-off wait loop;
// a no-op while the screen is on.
void powerRequestWake();

// ---- Hold-to-power, no sleep (EW_NO_SLEEP=1, set in platformio.ini) ----
// Ewan, 2026-09-23: "remove sleep mode on the watch; just have it power on
// and off with long hold". With the flag set the idle progression stops at
// the IDLE dim: no screensaver, no screen-off, no light or deep sleep.
#ifndef EW_NO_SLEEP
#define EW_NO_SLEEP 0
#endif
// The ONLY path that drives the LDO latch LOW: the 3 s side-button hold.
// On battery the watch is then truly off; pressing SW2 powers it back up and
// setup() latches HIGH again. On USB the board stays powered: the watch stays
// dark and idle until SW2 is held 1 s, then restarts.
void powerOffNow() __attribute__((noreturn));
