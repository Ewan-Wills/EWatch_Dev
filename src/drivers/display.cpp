#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pins.h"
#include "display.h"
#include "ewlog.h"

static Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

static const uint32_t BL_FREQ_HZ  = 12000;
static const uint8_t  BL_RES_BITS = 8;
static const uint8_t  BL_LEDC_CH  = 0;     // LEDC channel 0
static bool    bl_attached = false;
static uint8_t bl_current  = 0;            // last duty written

bool displayBegin() {
  // Keep backlight OFF throughout panel init. The ST7789 power-up sequence
  // takes ~200 ms and shows random panel RAM during that window — that's the
  // distorted black/white pattern users saw on every cold boot and wake. The
  // caller must call backlightOn() (or backlightSet) only after the first
  // frame has been drawn, so the user only ever sees committed content.
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);

  bus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS,
                             PIN_DISP_SCK, PIN_DISP_MOSI,
                             PIN_DISP_MISO, FSPI);

  // 240x280 IPS, col offset 0, row offset 20 (matches the FPC panel).
  gfx = new Arduino_ST7789(bus, PIN_DISP_RST, /*rotation=*/0, /*IPS=*/true,
                           240, 280, 0, 20);

  if (!gfx->begin(60000000)) {
    Serial.println("Display: gfx->begin() FAILED");
    delete gfx; gfx = nullptr;
    delete bus; bus = nullptr;
    return false;  
}
  gfx->fillScreen(BLACK);
  Serial.println("Display: ST7789 ready");
  return true;
}

void backlightSet(uint8_t duty) {
  if (!bl_attached) {
    ledcSetup(BL_LEDC_CH, BL_FREQ_HZ, BL_RES_BITS);
    ledcAttachPin(PIN_DISP_BL, BL_LEDC_CH);
    bl_attached = true;
  }
  ledcWrite(BL_LEDC_CH, duty);
  bl_current = duty;
}

uint8_t backlightGet() { return bl_current; }

// Stepped fade so brightness changes read as intentional, not glitchy.
// Blocking (max ~250 ms); called only from task context. All fades in the
// firmware go through here so the log timeline shows them.
void backlightFadeTo(uint8_t target, uint16_t ms) {
  uint8_t from = bl_current;
  if (from == target || ms == 0) { backlightSet(target); return; }
  EWLOGD("PWR", "bl_fade from=%u to=%u ms=%u", from, target, ms);
  const int steps = 12;
  for (int i = 1; i <= steps; i++) {
    int v = (int)from + ((int)target - (int)from) * i / steps;
    backlightSet((uint8_t)v);
    delay(ms / steps);
  }
  backlightSet(target);
}

void backlightOn()  { backlightSet(255); }
void backlightOff() { backlightSet(0); }

// ---- panel power controls --------------------------------------------------
// Fast path: DISPOFF (0x28) / DISPON (0x29) only. Panel RAM is retained, and
// neither command carries a mandatory delay — unlike SLPIN/SLPOUT (0x10/0x11,
// 120 ms each), which Arduino_GFX's displayOff()/displayOn() send. SLPIN is
// reserved for the deep-sleep path where the wake is a full reboot anyway.
void panelDispOff() {
  if (bus) bus->sendCommand(0x28);
}
void panelDispOn() {
  if (bus) bus->sendCommand(0x29);
}
void panelSleepIn() {
  if (!bus) return;
  bus->sendCommand(0x28);       // DISPOFF first per datasheet ordering
  bus->sendCommand(0x10);       // SLPIN — deep-sleep path only
  delay(120);                   // mandatory tSLPIN before rail changes
}

static Arduino_Canvas *gFrameCanvas = nullptr;
Arduino_Canvas *frameCanvas() {
  if (gFrameCanvas) return gFrameCanvas;
  if (!gfx) return nullptr;
  gFrameCanvas = new Arduino_Canvas(240, 280, gfx, 0, 0);
  // GFX_SKIP_OUTPUT_BEGIN: the underlying ST7789 + SPI bus were already
  // started in displayBegin(). Letting Canvas re-run output->begin() registers
  // a second APB-change callback for the SPI bus and logs a "duplicate" warning
  // from esp32-hal-cpu.c the moment any view first allocates the canvas.
  if (!gFrameCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("frameCanvas: alloc failed");
    delete gFrameCanvas;
    gFrameCanvas = nullptr;
  }
  return gFrameCanvas;
}