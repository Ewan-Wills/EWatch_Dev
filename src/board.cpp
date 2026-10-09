// EWatch v2 board support. See board.h. Driver code copied (and trimmed)
// from AuroraOS src/drivers/{display,haptic,touch,i2c_bus}.cpp and the RTC /
// accel / battery helpers in src/core/controller.cpp.
#include <Arduino.h>
#include <Wire.h>
#include <CST816S.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "board.h"

// ===========================================================================
// Display + backlight
// ===========================================================================
static Arduino_DataBus *sBus = nullptr;
Arduino_GFX *gfx = nullptr;

static const uint32_t BL_FREQ_HZ  = 12000;
static const uint8_t  BL_RES_BITS = 8;
static const uint8_t  BL_LEDC_CH  = 0;
static bool     sBlAttached = false;
static uint8_t  sBlCur = 0;
static bool     sFading = false;
static uint8_t  sFadeFrom = 0, sFadeTo = 0;
static uint32_t sFadeStartMs = 0;
static uint16_t sFadeMs = 0;

bool displayBegin() {
  // Backlight OFF through panel init (the ST7789 shows random RAM for
  // ~200 ms); the caller lights it after the first real frame.
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);
  sBus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS, PIN_DISP_SCK, PIN_DISP_MOSI,
                              PIN_DISP_MISO, FSPI);
  // 240x280 IPS, col offset 0, row offset 20 (this FPC).
  gfx = new Arduino_ST7789(sBus, PIN_DISP_RST, /*rotation=*/0, /*IPS=*/true,
                           240, 280, 0, 20);
  if (!gfx->begin(60000000)) {
    // Leaked on purpose: Arduino_DataBus has no virtual destructor, and a
    // failed panel is not retried.
    gfx = nullptr;
    sBus = nullptr;
    return false;
  }
  gfx->fillScreen(0x0000);
  return true;
}

static Arduino_Canvas *sCanvas = nullptr;
Arduino_Canvas *frameCanvas() {
  if (sCanvas) return sCanvas;
  if (!gfx) return nullptr;
  sCanvas = new Arduino_Canvas(240, 280, gfx, 0, 0);
  // The ST7789 + SPI bus are already up: re-running output->begin() would
  // register a second APB-change callback for the bus.
  if (!sCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    delete sCanvas;
    sCanvas = nullptr;
  }
  return sCanvas;
}

static void blWrite(uint8_t duty) {
  if (!sBlAttached) {
    ledcSetup(BL_LEDC_CH, BL_FREQ_HZ, BL_RES_BITS);
    ledcAttachPin(PIN_DISP_BL, BL_LEDC_CH);
    sBlAttached = true;
  }
  ledcWrite(BL_LEDC_CH, duty);
  sBlCur = duty;
}

void backlightSet(uint8_t duty) {
  sFading = false;
  blWrite(duty);
}

uint8_t backlightGet() { return sFading ? sFadeTo : sBlCur; }

void backlightFadeTo(uint8_t target, uint16_t ms) {
  if (sFading && sFadeTo == target) return;       // already heading there
  if (ms == 0 || sBlCur == target) { backlightSet(target); return; }
  sFadeFrom = sBlCur;
  sFadeTo = target;
  sFadeStartMs = millis();
  sFadeMs = ms;
  sFading = true;
}

void backlightService() {
  if (!sFading) return;
  uint32_t t = millis() - sFadeStartMs;
  if (t >= sFadeMs) { backlightSet(sFadeTo); return; }
  int v = (int)sFadeFrom + ((int)sFadeTo - (int)sFadeFrom) * (int)t / (int)sFadeMs;
  blWrite((uint8_t)v);
}

// ===========================================================================
// Haptics (DRV2603). A small task owns the motor timing; EN stays LOW
// whenever the motor is idle.
// ===========================================================================
static const uint32_t HAPTIC_FREQ_HZ  = 10000;
static const uint8_t  HAPTIC_RES_BITS = 8;
static const uint8_t  HAPTIC_LEDC_CH  = 1;
struct BuzzReq { uint8_t intensity; uint16_t durationMs; };
static QueueHandle_t sBuzzQ = nullptr;

static void hapticTask(void *) {
  esp_task_wdt_add(nullptr);
  BuzzReq r;
  for (;;) {
    esp_task_wdt_reset();
    if (xQueueReceive(sBuzzQ, &r, pdMS_TO_TICKS(1000)) != pdPASS) continue;
    digitalWrite(PIN_MOTOR_EN, HIGH);
    ledcWrite(HAPTIC_LEDC_CH, r.intensity);
    vTaskDelay(pdMS_TO_TICKS(r.durationMs));
    ledcWrite(HAPTIC_LEDC_CH, 0);
    digitalWrite(PIN_MOTOR_EN, LOW);
  }
}

void hapticBegin() {
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, LOW);
  ledcSetup(HAPTIC_LEDC_CH, HAPTIC_FREQ_HZ, HAPTIC_RES_BITS);
  ledcAttachPin(PIN_MOTOR_PWM, HAPTIC_LEDC_CH);
  ledcWrite(HAPTIC_LEDC_CH, 0);
  sBuzzQ = xQueueCreate(4, sizeof(BuzzReq));
  xTaskCreatePinnedToCore(hapticTask, "haptic", 2048, nullptr, 2, nullptr, 1);
}

void hapticBuzz(uint8_t intensity, uint16_t durationMs) {
  if (!sBuzzQ || intensity == 0) return;
  BuzzReq r = { intensity, durationMs };
  xQueueSend(sBuzzQ, &r, 0);              // full queue: drop (UI feedback only)
}

// ===========================================================================
// I2C
// ===========================================================================
static QueueHandle_t sRtcQ = nullptr;   // cross-task RTC writes (see rtcRequestSet)

void i2cBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  if (!sRtcQ) sRtcQ = xQueueCreate(2, sizeof(RtcTime));
}
void i2cRestoreClock() { Wire.setClock(I2C_FREQ_HZ); }
bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ===========================================================================
// RTC (RV-3028) — same register convention as AuroraOS controller.cpp.
// ===========================================================================
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

bool rtcRead(RtcTime &t) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 7) != 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = (uint8_t)Wire.read();
  t.second  = bcd2dec(b[0] & 0x7F);
  t.minute  = bcd2dec(b[1] & 0x7F);
  t.hour    = bcd2dec(b[2] & 0x3F);
  t.weekday = b[3] & 0x07;
  t.day     = bcd2dec(b[4] & 0x3F);
  t.month   = bcd2dec(b[5] & 0x1F);
  t.year    = (uint16_t)(2000 + bcd2dec(b[6]));
  return t.hour < 24 && t.minute < 60 && t.second < 60 &&
         t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31;
}

bool rtcWrite(const RtcTime &t) {
  uint8_t yy = (t.year >= 2000) ? (uint8_t)(t.year - 2000) : 0;
  if (yy > 99) yy = 99;
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  Wire.write(dec2bcd(t.second) & 0x7F);
  Wire.write(dec2bcd(t.minute) & 0x7F);
  Wire.write(dec2bcd(t.hour)   & 0x3F);
  Wire.write(t.weekday         & 0x07);
  Wire.write(dec2bcd(t.day)    & 0x3F);
  Wire.write(dec2bcd(t.month)  & 0x1F);
  Wire.write(dec2bcd(yy));
  return Wire.endTransmission() == 0;
}

void rtcRequestSet(const RtcTime &t) {
  if (sRtcQ) xQueueSend(sRtcQ, &t, 0);    // created by i2cBegin() in setup()
}

bool rtcTakeRequest(RtcTime &t) {
  return sRtcQ && xQueueReceive(sRtcQ, &t, 0) == pdPASS;
}

// ===========================================================================
// Accelerometer (MMA8451)
// ===========================================================================
static uint8_t sMmaAddr = 0;

static void mmaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(sMmaAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void mmaDrain(uint8_t reg) {
  Wire.beginTransmission(sMmaAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)sMmaAddr, 1);
    while (Wire.available()) (void)Wire.read();
  }
}

bool accelBegin() {
  if      (i2cPing(I2C_ADDR_MMA8451_A)) sMmaAddr = I2C_ADDR_MMA8451_A;
  else if (i2cPing(I2C_ADDR_MMA8451_B)) sMmaAddr = I2C_ADDR_MMA8451_B;
  if (!sMmaAddr) return false;
  // The chip keeps its registers through an ESP reset (it is only reset by a
  // power cycle), so whatever AuroraOS / an older build left (e.g. the
  // deep-sleep jolt wake on INT1) is overwritten here.
  mmaWrite(0x2A, 0x00);   // CTRL_REG1: standby to edit config
  mmaWrite(0x2B, 0x03);   // CTRL_REG2: MODS=11 low-power scheme
  mmaWrite(0x2D, 0x00);   // CTRL_REG4: NO interrupt sources at all
  mmaWrite(0x2E, 0x00);   // CTRL_REG5: (nothing routed to INT1)
  mmaWrite(0x2C, 0x02);   // CTRL_REG3: IPOL active-high -> idle LOW
  mmaDrain(0x1E);         // TRANSIENT_SRC: clear any latch
  mmaDrain(0x16);         // FF_MT_SRC
  mmaDrain(0x0C);         // INT_SOURCE
  mmaWrite(0x2A, 0x21);   // CTRL_REG1: DR=50 Hz + ACTIVE
  return true;
}

bool accelRead(int16_t &x, int16_t &y, int16_t &z) {
  if (!sMmaAddr) return false;
  Wire.beginTransmission(sMmaAddr);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)sMmaAddr, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = (uint8_t)Wire.read();
  x = (int16_t)((int16_t)((b[0] << 8) | b[1]) >> 2);
  y = (int16_t)((int16_t)((b[2] << 8) | b[3]) >> 2);
  z = (int16_t)((int16_t)((b[4] << 8) | b[5]) >> 2);
  return true;
}

// ===========================================================================
// Battery (divider tap on GPIO5, enabled only while sampling)
// ===========================================================================
bool batteryRead(float &volts, uint8_t &pct) {
#if defined(BAT_MON_SUPPORTED)
  static bool init = false;
  if (!init) {
    pinMode(PIN_BAT_MON_EN, OUTPUT);
    digitalWrite(PIN_BAT_MON_EN, LOW);
    analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);
    init = true;
  }
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  delay(2);
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_BAT_MON_ADC);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  uint32_t mv = acc / 8;
  if (mv == 0) return false;
  volts = (mv / 1000.0f) / 0.769f;
  float p = (volts - 3.30f) * (100.0f / 0.90f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  pct = (uint8_t)(p + 0.5f);
  return true;
#else
  (void)volts; (void)pct;
  return false;
#endif
}

// ===========================================================================
// Touch (CST816S). The FingerNum poll is the truth for down / hold / up;
// the ISR only flags that a gesture frame is waiting.
// ===========================================================================
static CST816S sTouch(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);
static volatile bool sTouchPending = false;
static bool     sTouchPresent = false;
static uint32_t sTouchFailStreak = 0;

static void IRAM_ATTR touchISR() { sTouchPending = true; }

static bool touchWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void touchReset() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(5);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);
}

static void touchDrainEvent() {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
    while (Wire.available()) (void)Wire.read();
  }
}

bool touchBegin() {
  // Pull INT up before any interrupt is attached: a floating pad with a
  // FALLING interrupt can storm and trip the interrupt watchdog.
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  sTouch.begin(Wire);                  // resets the chip, re-inits Wire
  i2cRestoreClock();                   // the library drops the bus to 100 kHz
  sTouchPresent = i2cPing(I2C_ADDR_TOUCH);
  if (!sTouchPresent) return false;
  sTouch.disable_auto_sleep();
  touchWriteReg(0xFA, 0x70);           // IrqCtl: EnTouch | EnChange | EnMotion
  // Drain touches latched during power-up (and any gesture that survived a
  // reboot — it would replay as a phantom swipe).
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {
    if (sTouch.available()) (void)sTouch.data.gestureID;
    delay(5);
  }
  touchDrainEvent();
  sTouch.attachUserInterrupt(touchISR);
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);  // the library reconfigures it as INPUT
  sTouchPending = false;
  return true;
}

bool touchReadHeld(uint16_t &x, uint16_t &y) {
  if (!sTouchPresent) return false;
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);
  bool ok = Wire.endTransmission(false) == 0 &&
            Wire.requestFrom((int)I2C_ADDR_TOUCH, 5) == 5;
  if (!ok) { if (sTouchFailStreak < 0xFFFFFFFFUL) sTouchFailStreak++; return false; }
  sTouchFailStreak = 0;
  uint8_t pts = (uint8_t)Wire.read();
  uint8_t xh = (uint8_t)Wire.read(), xl = (uint8_t)Wire.read();
  uint8_t yh = (uint8_t)Wire.read(), yl = (uint8_t)Wire.read();
  if (pts == 0) return false;
  x = (uint16_t)(((xh & 0x0F) << 8) | xl);
  y = (uint16_t)(((yh & 0x0F) << 8) | yl);
  return true;
}

uint8_t touchTakeGesture(uint16_t &x, uint16_t &y) {
  if (!sTouchPresent || !sTouchPending) return 0;
  sTouchPending = false;
  if (!sTouch.available()) return 0;
  uint8_t g = sTouch.data.gestureID;
  if (!g) return 0;
  // In continuous-report mode one physical swipe can raise several IRQs:
  // repeats of the same gesture within 400 ms are one gesture.
  static uint8_t  lastG = 0;
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  bool fresh = (g != lastG) || (now - lastMs > 400);
  lastG = g;
  lastMs = now;
  if (!fresh) return 0;
  x = (uint16_t)sTouch.data.x;
  y = (uint16_t)sTouch.data.y;
  return g;
}

void touchSelfHeal() {
  // A CST816S that slipped into its sleep state stops ACKing and never comes
  // back by itself: ~2 s of failed polls -> RST pulse + re-apply registers.
  if (!sTouchPresent || sTouchFailStreak <= 100) return;
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(5);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(60);
  touchWriteReg(0xFA, 0x70);
  touchWriteReg(0xFE, 0x01);          // DisAutoSleep
  touchDrainEvent();
  sTouchPending = false;
  sTouchFailStreak = 0;
}
