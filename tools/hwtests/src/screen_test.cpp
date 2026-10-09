// Screen test for EWatch v2 / v2.1 — standalone, serial + button driven.
//
// Cycles through test patterns on the ST7789 240x280 panel. Each pattern
// checks one thing you can judge by eye:
//   0 solid red / green / blue / white / black  dead or stuck pixels, tint
//   1 1-px border + 20 px grid + corner labels   offsets (row 20): all four
//                                                border lines must be visible
//   2 colour bars + 32-step grey ramps           colour order (RGB vs BGR),
//                                                inversion, banding
//   3 checkerboard 1 px / 2 px                   SPI data errors, flicker
//   4 text + orientation arrow                   rotation, font rendering
//   5 backlight ramp 0 -> 255 -> 0               PWM, BL transistor, flicker
//   6 speed: full-screen fills, frames/s         SPI clock / bus health
//   7 touch paint: draws where you touch         touch <-> panel alignment
//
// Controls:
//   button (SW2)     next pattern (auto-advance stops once you press it)
//   serial 0..7      jump to a pattern         n  next
//   serial a         toggle auto-advance (every 4 s)   h  help
//   serial b <0-255> backlight duty            c  clear touch canvas
//
//   pio run -e screen -t upload -t monitor
// Hold the watch button while flashing: the reset drops the LDO latch.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "pins.h"

static const int W = 240, H = 280;
static const uint8_t BL_CH = 0;

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;

static int  pattern = 0;
static bool autoAdv = true;
static uint32_t shownAt = 0;
static uint8_t blDuty = 255;
static bool touchOk = false;
static const int NPAT = 8;
static const char *NAMES[NPAT] = {"solid colours", "border + grid", "colour bars + grey ramps",
                                  "checkerboard", "text + orientation", "backlight ramp",
                                  "fill speed", "touch paint"};

static void bl(uint8_t d) { blDuty = d; ledcWrite(BL_CH, d); }

// ---- touch (CST816S, direct registers) ------------------------------
static bool touchRead(uint8_t &fingers, uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  fingers = b[0];
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
  return true;
}

static void touchInit() {
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(60);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  touchOk = Wire.endTransmission() == 0;
  if (touchOk) {                                   // keep it awake: DisAutoSleep = 1
    Wire.beginTransmission(I2C_ADDR_TOUCH); Wire.write(0xFE); Wire.write(0x01); Wire.endTransmission();
  }
  Serial.printf("touch  : CST816S %s\n", touchOk ? "answered at 0x15" : "SILENT at 0x15 (touch paint won't work)");
}

// ---- patterns --------------------------------------------------------
static void label(const char *s, uint16_t fg = WHITE, uint16_t bg = BLACK) {
  gfx->setTextSize(2);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(8, 8);
  gfx->print(s);
}

static void patSolid() {
  const uint16_t c[] = {RED, GREEN, BLUE, WHITE, BLACK};
  const char *n[] = {"red", "green", "blue", "white", "black"};
  for (int i = 0; i < 5; i++) {
    gfx->fillScreen(c[i]);
    Serial.printf("  solid %s\n", n[i]);
    uint32_t t = millis();
    while (millis() - t < 800) { delay(10); }
  }
  gfx->fillScreen(BLACK);
  label("solid: R G B W K");
}

static void patGrid() {
  gfx->fillScreen(BLACK);
  for (int x = 0; x < W; x += 20) gfx->drawFastVLine(x, 0, H, 0x4208);
  for (int y = 0; y < H; y += 20) gfx->drawFastHLine(0, y, W, 0x4208);
  gfx->drawRect(0, 0, W, H, WHITE);                // the 1-px border must show on all 4 sides
  gfx->drawRect(2, 2, W - 4, H - 4, RED);
  gfx->drawLine(0, 0, W - 1, H - 1, GREEN);
  gfx->drawLine(W - 1, 0, 0, H - 1, GREEN);
  gfx->drawCircle(W / 2, H / 2, 100, YELLOW);      // must look round, not oval
  gfx->setTextSize(1); gfx->setTextColor(CYAN);
  gfx->setCursor(5, 6);        gfx->print("TL");
  gfx->setCursor(W - 17, 6);   gfx->print("TR");
  gfx->setCursor(5, H - 14);   gfx->print("BL");
  gfx->setCursor(W - 17, H - 14); gfx->print("BR");
  gfx->setTextSize(2); gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(62, 130); gfx->print("240 x 280");
}

static void patBars() {
  const uint16_t c[] = {WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK};
  int bw = W / 8;
  for (int i = 0; i < 8; i++) gfx->fillRect(i * bw, 0, bw, 140, c[i]);
  for (int i = 0; i < 32; i++) {                   // grey, red, green, blue ramps
    int x = i * W / 32, w = W / 32 + 1;
    uint8_t v = i * 255 / 31;
    gfx->fillRect(x, 140, w, 35, gfx->color565(v, v, v));
    gfx->fillRect(x, 175, w, 35, gfx->color565(v, 0, 0));
    gfx->fillRect(x, 210, w, 35, gfx->color565(0, v, 0));
    gfx->fillRect(x, 245, w, 35, gfx->color565(0, 0, v));
  }
  gfx->setTextSize(1); gfx->setTextColor(BLACK);
  gfx->setCursor(4, 4); gfx->print("W Y C G M R B K");
}

static void patChecker() {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      bool top = y < H / 2;
      int s = top ? 1 : 2;
      gfx->writePixel(x, y, ((x / s + y / s) & 1) ? WHITE : BLACK);
    }
  label("1px / 2px", RED, BLACK);
}

static void patText() {
  gfx->fillScreen(NAVY);
  gfx->fillTriangle(W / 2, 30, W / 2 - 25, 70, W / 2 + 25, 70, YELLOW);   // arrow points UP
  gfx->setTextColor(WHITE, NAVY);
  gfx->setTextSize(2); gfx->setCursor(96, 78); gfx->print("TOP");
  gfx->setTextSize(1); gfx->setCursor(10, 110); gfx->print("size 1: The quick brown fox 0123456789");
  gfx->setTextSize(2); gfx->setCursor(10, 130); gfx->print("size 2: EWatch");
  gfx->setTextSize(3); gfx->setCursor(10, 160); gfx->print("size 3");
  gfx->setTextSize(4); gfx->setCursor(10, 200); gfx->print("12:34");
}

static void patBacklight() {
  gfx->fillScreen(WHITE);
  label("backlight ramp", BLACK, WHITE);
  for (int d = 0; d <= 255; d += 5) { bl(d); delay(20); }
  for (int d = 255; d >= 0; d -= 5) { bl(d); delay(20); }
  for (int d = 0; d <= 255; d += 5) { bl(d); delay(20); }
  bl(255);
  Serial.println("  backlight ramped 0 -> 255 -> 0 -> 255 (should be smooth, no flicker)");
}

static void patSpeed() {
  const uint16_t c[] = {RED, GREEN, BLUE, BLACK};
  uint32_t t0 = millis();
  const int N = 40;
  for (int i = 0; i < N; i++) gfx->fillScreen(c[i & 3]);
  uint32_t ms = millis() - t0;
  float fps = N * 1000.0f / ms;
  float mbit = fps * W * H * 16 / 1e6f;
  Serial.printf("  %d full-screen fills in %lu ms -> %.1f frames/s (%.1f Mbit/s of pixels; SPI 60 MHz max 60)\n",
                N, (unsigned long)ms, fps, mbit);
  gfx->fillScreen(BLACK);
  char s[40];
  snprintf(s, sizeof s, "%.1f fps", fps);
  label(s);
}

static void patTouch() {
  gfx->fillScreen(BLACK);
  label(touchOk ? "touch to draw" : "touch: no chip", touchOk ? WHITE : RED);
  gfx->drawRect(0, 0, W, H, DARKGREY);
}

static void show(int p) {
  pattern = (p % NPAT + NPAT) % NPAT;
  Serial.printf("screen : pattern %d - %s\n", pattern, NAMES[pattern]);
  switch (pattern) {
    case 0: patSolid(); break;
    case 1: patGrid(); break;
    case 2: patBars(); break;
    case 3: patChecker(); break;
    case 4: patText(); break;
    case 5: patBacklight(); break;
    case 6: patSpeed(); break;
    case 7: patTouch(); break;
  }
  shownAt = millis();
}

void setup() {
  pinMode(PIN_LDO_LATCH, OUTPUT);                  // keep the watch powered
  digitalWrite(PIN_LDO_LATCH, HIGH);
  pinMode(PIN_BTN, INPUT);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\n==== EWatch screen test ====");

  ledcSetup(BL_CH, 12000, 8);
  ledcAttachPin(PIN_DISP_BL, BL_CH);
  bl(0);                                           // hide power-on garbage

  bus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS, PIN_DISP_SCK, PIN_DISP_MOSI, PIN_DISP_MISO, FSPI);
  gfx = new Arduino_ST7789(bus, PIN_DISP_RST, 0, true, W, H, 0, 20);
  if (!gfx->begin(60000000)) {
    Serial.println("display: gfx->begin() FAILED");
    while (true) delay(1000);
  }
  gfx->fillScreen(BLACK);
  bl(255);
  Serial.println("display: ST7789 240x280 up, SPI 60 MHz, backlight 255");
  touchInit();
  Serial.println("keys   : 0-7 pattern, n next, a auto on/off, b <0-255> backlight, c clear; button = next");
  show(0);
}

void loop() {
  // serial commands. At most 64 bytes per pass so a noisy line can't starve
  // the screen; unknown lines are shown as hex (rate-limited) instead of
  // answered with help text, which could feed back through an echoing host.
  static char buf[16]; static uint8_t len = 0; static uint8_t raw[16]; static uint8_t rawN = 0;
  static uint32_t junkLines = 0, junkShown = 0, junkT = 0;
  for (int k = 0; k < 64 && Serial.available(); k++) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (rawN < sizeof raw) raw[rawN++] = (uint8_t)ci;
    if (c != '\n' && c != '\r') { if (len < sizeof buf - 1) buf[len++] = c; continue; }
    buf[len] = 0; uint8_t n = rawN; rawN = 0;
    if (!len) continue; len = 0;
    if (buf[0] >= '0' && buf[0] <= '7' && !buf[1]) { autoAdv = false; show(buf[0] - '0'); }
    else if (!strcmp(buf, "n")) show(pattern + 1);
    else if (!strcmp(buf, "a")) { autoAdv = !autoAdv; Serial.printf("screen : auto-advance %s\n", autoAdv ? "on" : "off"); }
    else if (!strcmp(buf, "c")) patTouch();
    else if (buf[0] == 'b' && buf[1] == ' ') { bl(constrain(atoi(buf + 2), 0, 255)); Serial.printf("screen : backlight %u\n", blDuty); }
    else if (!strcmp(buf, "h") || !strcmp(buf, "?")) Serial.println("keys   : 0-7 pattern, n next, a auto on/off, b <0-255> backlight, c clear");
    else {
      junkLines++;
      if (millis() - junkT > 1000) {
        junkT = millis();
        Serial.printf("serial : %lu unknown line(s) since last report; latest %u byte(s):", (unsigned long)(junkLines - junkShown), n);
        for (int i = 0; i < n; i++) Serial.printf(" %02X", raw[i]);
        Serial.println();
        junkShown = junkLines;
      }
    }
  }

  // button: next pattern, stops auto-advance
  static bool lastBtn = false; static uint32_t btnT = 0;
  bool b = digitalRead(PIN_BTN);
  if (b && !lastBtn && millis() - btnT > 200) { btnT = millis(); autoAdv = false; show(pattern + 1); }
  lastBtn = b;

  if (pattern == 7 && touchOk) {
    uint8_t f; uint16_t x, y;
    static uint32_t lastPrint = 0;
    if (touchRead(f, x, y) && f) {
      gfx->fillCircle(x, y, 4, gfx->color565((x * 255) / W, (y * 255) / H, 200));
      if (millis() - lastPrint > 200) { lastPrint = millis(); Serial.printf("  touch x=%u y=%u\n", x, y); }
      shownAt = millis();                          // don't auto-advance while drawing
    }
  }

  if (autoAdv && millis() - shownAt > 4000) show(pattern + 1);
  delay(5);
}
