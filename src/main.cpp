// TankFace: EWatch v2 watch face - a rectangular Tank-style dial with blued
// sword hands (see tank_draw.cpp and tools/build_dial.py).
//
// Touch: tap = seconds hand on/off (off by default, as on the watch it copies).
// Button: short press = seconds hand on/off; hold 2 s = power off (drops the
// LDO latch).
//
// Pins, RTC and touch follow EWatchOS (src/drivers/pins.h) and HeadFace.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <CST816S.h>
#include <driver/gpio.h>
#include "tank_draw.h"

// ---- EWatch v2 pins (EWatchOS pins.h; same on V2.0 and V2.1) ----
#define PIN_LDO_LATCH 17
#define PIN_BTN       18
#define PIN_I2C_SDA    8
#define PIN_I2C_SCL    9
#define PIN_DISP_SCK  10
#define PIN_DISP_MOSI 11
#define PIN_DISP_DC    6
#define PIN_DISP_CS    7
#define PIN_DISP_RST  12
#define PIN_DISP_BL   21
#define PIN_TOUCH_RST 13
#define PIN_TOUCH_INT 14
#define I2C_ADDR_RV3028 0x52

// CST816S gesture IDs (EWatchOS event.h).
enum : uint8_t { G_TAP = 0x05 };

static const uint32_t kSweepMs = 40;           // seconds hand: 25 steps a second

// Arduino_Canvas mallocs its framebuffer, which lands in PSRAM on this board
// (every malloc > 4 KB does). Hand it a buffer in internal RAM instead: the
// dial copy, hands and SPI push all touch every pixel every frame.
class FastCanvas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  void useBuffer(uint16_t *b) { _framebuffer = b; }
};

static Arduino_DataBus *bus;
static Arduino_GFX     *gfx;
static FastCanvas      *canvas;
static CST816S touch(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);
static bool secondsOn = false;

// ---- RTC (RV-3028) --------------------------------------------------------
static uint8_t bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
struct Clock { bool ok; uint8_t h, m, s; };
static Clock readRTC() {
  Clock c{};
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return c;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 3) != 3) return c;
  uint8_t b[3];
  for (int i = 0; i < 3; i++) b[i] = Wire.read();
  c.s = bcd(b[0] & 0x7F); c.m = bcd(b[1] & 0x7F); c.h = bcd(b[2] & 0x3F);
  c.ok = c.h < 24 && c.m < 60 && c.s < 60;
  return c;
}

static void powerOff() {
  gfx->fillScreen(0);
  digitalWrite(PIN_DISP_BL, LOW);
  gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
  digitalWrite(PIN_LDO_LATCH, LOW);
  for (;;) delay(1000);                  // rail drops once the button is released
}

static void pollInput(uint32_t now) {
  // Touch gestures (deduplicated: the chip can repeat one gesture).
  static uint8_t lastG = 0; static uint32_t lastGMs = 0;
  if (touch.available()) {
    uint8_t g = touch.data.gestureID;
    if (g && (g != lastG || now - lastGMs > 400)) {
      if (g == G_TAP) secondsOn = !secondsOn;
      Serial.printf("gesture 0x%02x seconds %d\n", g, secondsOn);
    }
    if (g) { lastG = g; lastGMs = now; }
  }
  // Button: short press = seconds hand on/off, hold 2 s = off.
  static uint32_t downMs = 0;
  if (digitalRead(PIN_BTN) == HIGH) {
    if (!downMs) downMs = now;
    else if (now - downMs > 2000) powerOff();
  } else if (downMs) {
    Serial.printf("button press %lums\n", (unsigned long)(now - downMs));
    if (now - downMs < 1000) secondsOn = !secondsOn;
    downMs = 0;
  }
}

void setup() {
  // FIRST: hold our own power (the rail is only up while SW2 is pressed).
  pinMode(PIN_LDO_LATCH, OUTPUT);
  digitalWrite(PIN_LDO_LATCH, HIGH);
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);   // survive resets without dropping
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);            // dark until the first real frame

  Serial.begin(115200);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  touch.begin(Wire);
  Wire.setClock(400000);                     // the library re-init drops the clock
  touch.disable_auto_sleep();

  bus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS, PIN_DISP_SCK, PIN_DISP_MOSI,
                             GFX_NOT_DEFINED, FSPI);
  gfx = new Arduino_ST7789(bus, PIN_DISP_RST, 0, true, 240, 280, 0, 20);
  gfx->begin(80000000);                      // 80 MHz, as GalleryFace (measured 17.7 ms/frame push)
  gfx->fillScreen(0);
  canvas = new FastCanvas(240, 280, gfx, 0, 0);
  uint16_t *fbInt = (uint16_t *)heap_caps_malloc(240 * 280 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (fbInt) canvas->useBuffer(fbInt);
  else Serial.println("canvas: no internal RAM, using PSRAM");
  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);

  Serial.printf("TankFace: rtc(0x%02X) %s\n", I2C_ADDR_RV3028, readRTC().ok ? "ok" : "MISSING");
}

void loop() {
  static uint32_t lastRtcMs = 0, secAnchorMs = 0, lastDrawMs = 0;
  static Clock clk{};
  static uint8_t secSeen = 255;
  static uint32_t frames = 0, statMs = 0, usDraw = 0, usFlush = 0;
  static int drawnKey = -1; static bool drawnSeconds = false, lit = false;

  uint32_t now = millis();
  pollInput(now);

  if (now - lastRtcMs >= 100 || !clk.ok) {
    lastRtcMs = now;
    Clock c = readRTC();
    if (c.ok) clk = c;
  }
  if (clk.s != secSeen) { secSeen = clk.s; secAnchorMs = now; }

  // Redraw once a second (the minute hand creeps), or every kSweepMs while
  // the seconds hand is on, or when it was toggled.
  int key = clk.ok ? (clk.h * 60 + clk.m) * 60 + clk.s : -2;
  bool due = key != drawnKey || secondsOn != drawnSeconds ||
             (secondsOn && clk.ok && now - lastDrawMs >= kSweepMs);
  if (due) {
    float frac = (now - secAnchorMs) / 1000.0f;
    if (frac > 0.999f) frac = 0.999f;
    uint32_t u0 = micros();
    tank::draw(canvas->getFramebuffer(), tank::Input{clk.ok, clk.h, clk.m, clk.s, frac, secondsOn});
    uint32_t u1 = micros();
    canvas->flush();
    usDraw += u1 - u0; usFlush += micros() - u1;
    if (!lit) { digitalWrite(PIN_DISP_BL, HIGH); lit = true; }
    frames++;
    drawnKey = key; drawnSeconds = secondsOn; lastDrawMs = now;
  }

  if (now - statMs >= 5000) {
    uint32_t f = frames ? frames : 1;
    Serial.printf("time=%02u:%02u:%02u rtc=%d seconds=%d fps=%.1f draw=%luus flush=%luus\n",
                  clk.h, clk.m, clk.s, clk.ok, secondsOn, frames * 1000.0f / (now - statMs + 1),
                  (unsigned long)(usDraw / f), (unsigned long)(usFlush / f));
    statMs = now; frames = usDraw = usFlush = 0;
  }
  delay(5);
}
