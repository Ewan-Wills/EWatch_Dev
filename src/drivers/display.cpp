#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pins.h"
#include "display.h"

static Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

static const uint32_t BL_FREQ_HZ  = 12000;
static const uint8_t  BL_RES_BITS = 8;
static const uint8_t  BL_LEDC_CH  = 0;     // LEDC channel 0
static bool bl_attached = false;

bool displayBegin() {
  // Hold backlight off so the user doesn't see uninitialised panel garbage.
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, HIGH);

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
}

void backlightOn()  { backlightSet(255); }
void backlightOff() { backlightSet(0); }