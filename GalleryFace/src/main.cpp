// GalleryFace: EWatch v2 analog face over a crossfading slideshow of Claire
// Wills paintings, plus QR cards for the gallery and the website.
//
// Pages (swipe right = next, swipe left = back):
//   0  paintings: dial over the slideshow; tap or button toggles the dial
//      off/on (screensaver = paintings only)
//   1  QR: gallery location (Google Maps)
//   2  QR: clairewills.co.uk
// Button: short press = back to page 0 (on page 0: toggle the dial);
//         hold 2 s = power off (drops the LDO latch).
//
// Core 1 (loop) renders; core 0 runs the JPEG decoder task that prepares the
// next painting while the current one is on screen. Pins, RTC and touch
// follow AuroraOS (src/drivers/pins.h, core/controller.cpp, drivers/touch.cpp).
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <CST816S.h>
#include <driver/gpio.h>
#include "slides.h"
#include "face_draw.h"
#include "qr_draw.h"

// ---- EWatch v2 pins (AuroraOS pins.h) ----
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

// CST816S gesture IDs (AuroraOS event.h; 0x04 is a right swipe on this panel).
enum : uint8_t { G_SWIPE_LEFT = 0x03, G_SWIPE_RIGHT = 0x04, G_TAP = 0x05 };

struct QrCard { const char *title, *subtitle, *url; };
static const QrCard kCards[] = {
  { "Find the gallery", "Directions in Google Maps", "https://maps.app.goo.gl/LDtHCnFEXopYkxdy5" },
  { "clairewills.co.uk", "See all the paintings online", "https://clairewills.co.uk" },
};
static const int kPages = 1 + sizeof(kCards) / sizeof(kCards[0]);
static const uint32_t kFrameMs = 33;           // ~30 fps on the paintings page

// Arduino_Canvas mallocs its framebuffer, which lands in PSRAM on this board
// (every malloc > 4 KB does). Hand it a buffer in internal RAM instead: the
// blend, dial and SPI push all touch every pixel every frame.
class FastCanvas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  void useBuffer(uint16_t *b) { _framebuffer = b; }
};

static Arduino_DataBus *bus;
static Arduino_GFX     *gfx;
static FastCanvas      *canvas;
static CST816S touch(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);

static int  page = 0;
static bool dialOn = true;
static bool pageDirty = true;                  // QR pages draw once per visit

// ---- RTC (RV-3028) --------------------------------------------------------
static uint8_t bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
struct Clock { bool ok; uint8_t h, m, s, wd, day, mon; };
static Clock readRTC() {
  Clock c{};
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return c;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 7) != 7) return c;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  c.s = bcd(b[0] & 0x7F); c.m = bcd(b[1] & 0x7F); c.h = bcd(b[2] & 0x3F);
  c.wd = b[3] & 0x07; c.day = bcd(b[4] & 0x3F); c.mon = bcd(b[5] & 0x1F);
  c.ok = c.h < 24 && c.m < 60 && c.s < 60;
  return c;
}

static void decoderTask(void *) {
  for (;;) if (!slides::decodeStep()) vTaskDelay(pdMS_TO_TICKS(20));
}

static void powerOff() {
  gfx->fillScreen(0);
  digitalWrite(PIN_DISP_BL, LOW);
  gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
  digitalWrite(PIN_LDO_LATCH, LOW);
  for (;;) delay(1000);                  // rail drops once the button is released
}

static void goPage(int p) {
  if (p < 0) p = 0;
  if (p >= kPages) p = kPages - 1;
  if (p != page) { page = p; pageDirty = true; }
}

static void pollInput(uint32_t now) {
  // Touch gestures (deduplicated: the chip can repeat one swipe).
  static uint8_t lastG = 0; static uint32_t lastGMs = 0;
  if (touch.available()) {
    uint8_t g = touch.data.gestureID;
    if (g && (g != lastG || now - lastGMs > 400)) {
      if (g == G_SWIPE_RIGHT) goPage(page + 1);
      else if (g == G_SWIPE_LEFT) goPage(page - 1);
      else if (g == G_TAP && page == 0) dialOn = !dialOn;
      Serial.printf("gesture 0x%02x -> page %d dial %d\n", g, page, dialOn);
    }
    if (g) { lastG = g; lastGMs = now; }
  }
  // Button: short press = back / toggle dial, hold 2 s = off.
  static uint32_t downMs = 0;
  if (digitalRead(PIN_BTN) == HIGH) {
    if (!downMs) downMs = now;
    else if (now - downMs > 2000) powerOff();
  } else if (downMs) {
    Serial.printf("button press %lums\n", (unsigned long)(now - downMs));
    if (now - downMs < 1000) { if (page) goPage(0); else dialOn = !dialOn; }
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
  gfx->begin(80000000);                      // 80 MHz (a "60 MHz" request really runs at 80/2 = 40 MHz)
  gfx->fillScreen(0);
  canvas = new FastCanvas(240, 280, gfx, 0, 0);
  uint16_t *fbInt = (uint16_t *)heap_caps_malloc(240 * 280 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (fbInt) canvas->useBuffer(fbInt);
  else Serial.println("canvas: no internal RAM, using PSRAM");
  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);

  slides::allocPixels = [](size_t n) -> void * { return ps_malloc(n); };
  if (!slides::begin(esp_random() % slides::count(), millis()))
    Serial.println("slides: PSRAM alloc failed");
  xTaskCreatePinnedToCore(decoderTask, "jpeg", 8192, nullptr, 1, nullptr, 0);

  Serial.printf("GalleryFace: %d paintings, psram free %u\n", slides::count(),
                (unsigned)ESP.getFreePsram());
}

void loop() {
  static uint32_t lastRtcMs = 0, secAnchorMs = 0;
  static Clock clk{};
  static uint8_t secSeen = 255;
  static uint32_t frames = 0, statMs = 0, busyMs = 0;
  static uint32_t usRender = 0, usFace = 0, usFlush = 0;
  static bool lit = false;

  uint32_t now = millis();
  pollInput(now);

  if (now - lastRtcMs >= 200 || !clk.ok) {
    lastRtcMs = now;
    Clock c = readRTC();
    if (c.ok) clk = c;
  }
  if (clk.s != secSeen) { secSeen = clk.s; secAnchorMs = now; }

  if (page == 0) {
    uint32_t u0 = micros();
    int shown = slides::render(now, canvas->getFramebuffer());
    uint32_t u1 = micros();
    if (dialOn) {
      float frac = (now - secAnchorMs) / 1000.0f;
      if (frac > 0.999f) frac = 0.999f;
      FaceInput in{clk.ok, clk.h, clk.m, clk.s, frac, clk.wd, clk.day, clk.mon,
                   slides::title(shown)};
      drawFace(canvas, in);
    }
    uint32_t u2 = micros();
    canvas->flush();
    usRender += u1 - u0; usFace += u2 - u1; usFlush += micros() - u2;
    pageDirty = true;                        // a QR page must redraw on entry
  } else if (pageDirty) {
    const QrCard &c = kCards[page - 1];
    drawQR(canvas, c.title, c.subtitle, c.url, page, kPages);
    canvas->flush();
    pageDirty = false;
  }
  if (!lit) { digitalWrite(PIN_DISP_BL, HIGH); lit = true; }

  frames++;
  busyMs += millis() - now;
  if (now - statMs >= 5000) {
    uint32_t f = frames ? frames : 1;
    Serial.printf("page=%d dial=%d fps=%.1f busy=%lums/frame (render %lu face %lu flush %lu us) decodeFail=%lu psram=%u\n",
                  page, dialOn, frames * 1000.0f / (now - statMs + 1),
                  (unsigned long)(busyMs / f), (unsigned long)(usRender / f),
                  (unsigned long)(usFace / f), (unsigned long)(usFlush / f),
                  (unsigned long)slides::decodeFailures(), (unsigned)ESP.getFreePsram());
    statMs = now; frames = busyMs = usRender = usFace = usFlush = 0;
  }
  uint32_t spent = millis() - now;
  if (spent < kFrameMs) delay(kFrameMs - spent);
}
