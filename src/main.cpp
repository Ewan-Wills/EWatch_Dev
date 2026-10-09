// ClaudeFace: stand-alone EWatch v2 firmware — the Claude Code status face,
// drawn landscape (watch on its side, physical top edge pointing RIGHT), fed
// over USB serial by the host bridge (ClaudeFace/bridge). Nothing else: no
// launcher, no other faces, no WiFi, and no sleep of any kind.
//
// Power: the LDO latch is driven HIGH as the very first thing and pad-held
// (gpio_hold_en), so no reset can drop the rail. The ONLY way off is holding
// the side button 3 s (powerOffNow). The screen never turns off; it dims to
// the ambient level (cf set ambient) 10 s after the last trigger.
//
// Tasks:
//   loopTask (core 1)  the UI loop below: owns Wire (touch / RTC / accel),
//                      gfx + canvas + backlight, the side button, haptic
//                      policy and rendering (claude_face.cpp)
//   cflink   (core 0)  USB serial reader: #CF frames, console (serial_io.cpp)
//   haptic   (core 1)  DRV2603 pulse timing (board.cpp)
// All three are on the task watchdog (20 s, panic -> reboot; the latch hold
// keeps the watch powered through it).
//
// Hardware code is the minimum taken from AuroraOS (src/drivers, and the
// RTC / accel / battery helpers of src/core/controller.cpp); the structure
// follows HeadFace / GalleryFace.
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "pins.h"
#include "board.h"
#include "serial_io.h"
#include "settings.h"
#include "claude_face.h"
#include "cf_policy.h"

static const uint32_t kTaskWdtSec = 20;
RTC_DATA_ATTR static uint32_t gCrashBoots = 0;   // survives soft resets only

static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:                return "other";
  }
}

// ---------------------------------------------------------------------------
// Power-off: the ONLY path that drives the LDO latch LOW (3 s button hold).
// On battery the rail drops here and the watch is off; SW2 powers it back
// up. On USB the board stays powered: stay dark and idle (latch LOW) until
// SW2 is held 1 s, then restart. Same behaviour as the AuroraOS no-sleep
// build's powerOffNow().
// ---------------------------------------------------------------------------
static void powerOffNow() __attribute__((noreturn));
static void powerOffNow() {
  logf("SYS", "power_off (button hold)");
  if (gfx) gfx->fillScreen(0x0000);
  hapticBuzz(180, 200);
  delay(80);
  backlightSet(0);
  gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);   // setup() holds the pad HIGH
  unlatchPower();                             // battery: power dies here
  delay(300);
  // Still running => USB powers the board. This task stops feeding the
  // task WDT, so leave it first.
  esp_task_wdt_delete(nullptr);
  while (buttonPressed()) delay(20);          // release of the off-hold
  for (;;) {
    if (buttonPressed()) {
      const uint32_t t0 = millis();
      while (buttonPressed() && millis() - t0 < 1000) delay(10);
      if (millis() - t0 >= 1000) {
        latchPower();
        while (buttonPressed()) delay(20);    // don't carry into a 3 s off
        esp_restart();
      }
    }
    delay(50);
  }
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup() {
  // FIRST: hold our own power (on a cold boot the rail is only up while SW2
  // is pressed), then pad-hold it so no reset (flash, USB chip reset,
  // panic, WDT) can let it float LOW and drop the rail.
  latchPower();
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);

  // A panic reset does not reset the GPIO matrix: an interrupt left armed
  // (e.g. a level wake on BTN from AuroraOS' sleep path) would storm the
  // moment the ISR service installs -> IWDT -> reset loop. Disarm every
  // interrupt-capable pin of ours before anything installs ISRs.
  {
    const gpio_num_t kIntPins[] = {
      (gpio_num_t)PIN_BTN, (gpio_num_t)PIN_TOUCH_INT,
      (gpio_num_t)PIN_MMA_INT1, (gpio_num_t)PIN_MMA_INT2,
      (gpio_num_t)PIN_RTC_INT,
    };
    for (size_t i = 0; i < sizeof kIntPins / sizeof kIntPins[0]; i++) {
      gpio_set_intr_type(kIntPins[i], GPIO_INTR_DISABLE);
      gpio_intr_disable(kIntPins[i]);
      gpio_wakeup_disable(kIntPins[i]);
    }
  }
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_RTC_INT, INPUT_PULLUP);
  pinMode(PIN_MMA_INT1, INPUT);              // strapping pin: input, never driven
  pinMode(PIN_MMA_INT2, INPUT);

  esp_reset_reason_t rr = esp_reset_reason();
  bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
               rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
  if (crash) gCrashBoots++;
  else if (rr == ESP_RST_POWERON) gCrashBoots = 0;

  serialBegin();
  logf("SYS", "ClaudeFace boot reset=%s crash_boots=%lu heap=%lu psram=%lu",
       resetReasonStr(rr), (unsigned long)gCrashBoots,
       (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

  settingsLoad();
  hapticBegin();

  if (!displayBegin()) logf("SYS", "display init FAILED");
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) logf("SYS", "canvas alloc failed: drawing straight to the panel");

  i2cBegin();
  touchReset();                              // wake the CST816S before probing
  bool touchOk = touchBegin();               // restores 400 kHz after the lib
  i2cRestoreClock();
  bool accelOk = accelBegin();
  RtcTime t;
  bool rtcOk = rtcRead(t);
  logf("SYS", "touch=%d accel=%d rtc=%d", touchOk ? 1 : 0, accelOk ? 1 : 0, rtcOk ? 1 : 0);

  claudeFaceBegin(cv);

  // Arm the task watchdog before the tasks start (each subscribes itself).
  esp_task_wdt_init(kTaskWdtSec, /*panic=*/true);
  esp_task_wdt_add(nullptr);                 // this loopTask
  serialStartLinkTask();
}

// ---------------------------------------------------------------------------
// UI loop (loopTask, core 1). Sole owner of Wire.
// ---------------------------------------------------------------------------
void loop() {
  static bool     init = false;
  static CFButton btn;
  static bool     lastPressed = false;
  static CFEnv    env;
  static uint32_t lastRtcMs = 0, lastBatMs = 0, lastAccMs = 0;
  static bool     fingerDown = false;
  static uint16_t lastTx = 0, lastTy = 0;
  static uint32_t lastHoldMs = 0;
  static int16_t  pax = 0, pay = 0, paz = 0;
  static uint32_t motionAcc = 0, motionWinMs = 0, lastMotionMs = 0;

  esp_task_wdt_reset();
  uint32_t now = millis();

  if (!init) {
    init = true;
    memset(&env, 0, sizeof env);
    cfButtonInit(btn, buttonPressed());      // still holding SW2 from power-on?
    lastPressed = buttonPressed();
  }

  // ---- side button ----
  bool pressed = buttonPressed();
  if (pressed && !lastPressed) claudeFaceWake();
  lastPressed = pressed;
  switch (cfButtonStep(btn, pressed, now)) {
    case CF_BTN_SHORT:     claudeFaceButtonShort(); break;
    case CF_BTN_POWER_OFF: powerOffNow();
    default: break;
  }

  // ---- RTC writes asked for by the link (TIME frames) ----
  RtcTime w;
  while (rtcTakeRequest(w)) {
    bool ok = rtcWrite(w);
    logf("RTC", "write %s", ok ? "ok" : "FAILED");
    lastRtcMs = 0;                           // re-read now
  }

  // ---- touch: poll is the truth for down / hold / up; ISR for gestures ----
  touchSelfHeal();
  uint16_t tx = 0, ty = 0;
  bool live = touchReadHeld(tx, ty);
  if (live && !fingerDown) {
    fingerDown = true;
    lastTx = tx; lastTy = ty;
    lastHoldMs = now;
    claudeFaceTouchDown(tx, ty);
  } else if (live) {
    lastTx = tx; lastTy = ty;
    if (now - lastHoldMs >= 30) { lastHoldMs = now; claudeFaceTouchHold(tx, ty); }
  } else if (fingerDown) {
    fingerDown = false;
    claudeFaceTouchUp(lastTx, lastTy);
  }
  uint16_t gx, gy;
  uint8_t g = touchTakeGesture(gx, gy);
  if (g) claudeFaceGesture(g);

  // ---- accelerometer: a deliberate jolt / wrist raise = 100 % for 10 s
  //      (AuroraOS' ImuMotion detector: summed |delta| over 300 ms at 50 Hz) ----
  if (now - lastAccMs >= 20) {
    lastAccMs = now;
    int16_t ax, ay, az;
    if (accelRead(ax, ay, az)) {
      uint32_t d = (uint32_t)abs(ax - pax) + (uint32_t)abs(ay - pay) + (uint32_t)abs(az - paz);
      pax = ax; pay = ay; paz = az;
      if (now - motionWinMs > 300) { motionWinMs = now; motionAcc = 0; }
      motionAcc += d;
      if (motionAcc > 18000 && now - lastMotionMs > 800) {
        lastMotionMs = now;
        motionAcc = 0;
        claudeFaceWake();
      }
    }
  }

  // ---- RTC (4 Hz) + battery (1 Hz) ----
  if (lastRtcMs == 0 || now - lastRtcMs >= 250) {
    lastRtcMs = now ? now : 1;
    RtcTime t;
    env.rtcOk = rtcRead(t);
    if (env.rtcOk) {
      env.year = t.year; env.month = t.month; env.day = t.day;
      env.hour = t.hour; env.minute = t.minute; env.second = t.second;
    }
  }
  if (lastBatMs == 0 || now - lastBatMs >= 1000) {
    lastBatMs = now ? now : 1;
    float v;
    uint8_t pct = 0;
    env.batOk = batteryRead(v, pct);
    env.batPct = pct;
  }

  claudeFaceTick(env);
  delay(10);
}
