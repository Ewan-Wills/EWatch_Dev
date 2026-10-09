// =============================================================================
//  EWatch — STANDALONE DISPLAY + TOUCH BRING-UP TEST
// -----------------------------------------------------------------------------
//  This is a completely separate program from the normal firmware. It builds
//  in its own PlatformIO environment ([env:disptest]) and NONE of the regular
//  app sources are compiled, so you can hammer on the display bus and the
//  CST816S touch controller in isolation.
//
//  Build & run:
//      pio run -e disptest -t upload -t monitor
//      (~/.platformio/penv/bin/pio if `pio` isn't on your PATH)
//
//  Back to the real firmware:
//      pio run -e ewatch  -t upload -t monitor
//
//  Everything is driven from the serial monitor — press a key (then Enter is
//  not required) to run a test. Press 'h' for the menu at any time.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <CST816S.h>
#include "pins.h"

// ============================ TUNABLES =======================================
// Edit these, re-flash (pio run -e disptest -t upload), and re-test. The values
// below mirror the working firmware so the display comes up identically.

// ---- Display: Arduino_ESP32SPI(...) constructor -----------------------------
#define DISP_SPI_HOST     FSPI            // try HSPI if FSPI misbehaves
#define DISP_SPI_HZ       60000000UL      // start speed; switch live with 1-5

// ---- Display: Arduino_ST7789(...) geometry ----------------------------------
#define DISP_ROTATION     0
#define DISP_IPS          true
#define DISP_W            240
#define DISP_H            280
#define DISP_COL_OFFSET   0
#define DISP_ROW_OFFSET   20

// ---- Touch: default reset pulse (ms). The 'r' sweep ignores these and tries
//      a whole matrix; the single-reset 'R' command uses them. -----------------
#define TRST_PRE_HIGH_MS  50              // idle HIGH before the pulse
#define TRST_LOW_MS       5               // active-LOW pulse width
#define TRST_POST_HIGH_MS 50              // settle after release
// =============================================================================

// On-board I2C devices we already know about — used to flag "new" devices that
// appear after a touch reset (e.g. the touch chip strapping to a non-0x15 addr).
static const uint8_t KNOWN_ACCEL_A = I2C_ADDR_MMA8451_A;  // 0x1C
static const uint8_t KNOWN_ACCEL_B = I2C_ADDR_MMA8451_B;  // 0x1D
static const uint8_t KNOWN_RTC     = I2C_ADDR_RV3028;     // 0x52

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX     *gfx = nullptr;
static uint32_t         spiHz = DISP_SPI_HZ;

// CST816S library object (used by the 't' live-touch-via-library test).
static CST816S touch(PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);

// ----------------------------------------------------------------------------
// Low-level I2C helpers (raw — no library, so a broken INT line can't hide a
// working I2C path).
// ----------------------------------------------------------------------------
static bool pingAddr(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static bool isKnown(uint8_t a) {
  return a == KNOWN_ACCEL_A || a == KNOWN_ACCEL_B || a == KNOWN_RTC;
}

// Full-bus scan grid; returns the count and (optionally) the first address that
// is NOT one of our known on-board parts — i.e. a candidate touch controller.
static uint8_t scanBus(uint8_t *firstUnknown = nullptr) {
  if (firstUnknown) *firstUnknown = 0;
  Serial.println("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
  uint8_t found = 0;
  for (uint8_t row = 0; row < 0x80; row += 0x10) {
    Serial.printf("%02x:", row);
    for (uint8_t col = 0; col < 0x10; col++) {
      uint8_t a = row + col;
      if (a < 0x08 || a >= 0x78)      Serial.print("   ");
      else if (pingAddr(a)) {
        Serial.print(" ##");
        found++;
        if (firstUnknown && !*firstUnknown && !isKnown(a)) *firstUnknown = a;
      } else                          Serial.print(" --");
    }
    Serial.println();
  }
  Serial.printf("  %u device(s) found\n", found);
  return found;
}

// One configurable reset pulse. intLevel: -1 = leave INT as input (hi-Z),
// 0 = hold INT low across the reset, 1 = hold INT high. Some touch parts sample
// the IRQ pin at the RST rising edge to pick their I2C address, so this is worth
// sweeping.
static void touchResetPulse(uint16_t preHigh, uint16_t low, uint16_t postHigh,
                            int intLevel, bool invert = false) {
  const uint8_t idle = invert ? LOW  : HIGH;    // RST resting level
  const uint8_t act  = invert ? HIGH : LOW;     // RST active (reset) level
  pinMode(PIN_TOUCH_RST, OUTPUT);
  if (intLevel == 0)      { pinMode(PIN_TOUCH_INT, OUTPUT); digitalWrite(PIN_TOUCH_INT, LOW);  }
  else if (intLevel == 1) { pinMode(PIN_TOUCH_INT, OUTPUT); digitalWrite(PIN_TOUCH_INT, HIGH); }
  else                    { pinMode(PIN_TOUCH_INT, INPUT); }

  digitalWrite(PIN_TOUCH_RST, idle); delay(preHigh);
  digitalWrite(PIN_TOUCH_RST, act);  delay(low);
  digitalWrite(PIN_TOUCH_RST, idle);            // strap (if any) latched here
  delay(2);
  if (intLevel >= 0) pinMode(PIN_TOUCH_INT, INPUT);   // release before chip drives it
  delay(postHigh);
}

// ---- Pin-integrity probe: find a hardware short/open from firmware ----------
// Drives the ESP's internal pulldown then pullup on a pin and reads it back. A
// healthy pin that's only connected to a high-impedance chip input "follows"
// the internal resistor (0 then 1). A pin shorted to GND reads 0 even with the
// pullup; shorted to VDD (or a strong external pull-up) reads 1 even with the
// pulldown. RUN THIS ON A GOOD BOARD AND A DEAD BOARD AND DIFF THE RESULTS —
// any pin that reads differently is your fault.
static void pinIntegrity(int pin, const char *name) {
  pinMode(pin, INPUT_PULLDOWN); delay(3); int lo = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);   delay(3); int hi = digitalRead(pin);
  pinMode(pin, INPUT);
  const char *verdict;
  if      (lo == 0 && hi == 1) verdict = "follows pulls (normal hi-Z node)";
  else if (lo == 1 && hi == 1) verdict = "STUCK HIGH (short to VDD or strong ext pull-up)";
  else if (lo == 0 && hi == 0) verdict = "STUCK LOW  (short to GND or line held low)";
  else                         verdict = "noisy/unknown";
  Serial.printf("  %-4s GPIO%-2d : pulldown=%d pullup=%d -> %s\n", name, pin, lo, hi, verdict);
}

static void pinIntegrityTest() {
  Serial.println("=== Pin integrity (FPC signals) ===");
  Serial.println("  Compare these readings against a WORKING board:");
  Serial.println("  -- touch --");
  pinIntegrity(PIN_TOUCH_RST, "TRST");  // GPIO13
  pinIntegrity(PIN_TOUCH_INT, "TINT");  // GPIO14
  Serial.println("  -- display (DRST often has a module pull-up too) --");
  pinIntegrity(PIN_DISP_RST, "DRST");   // GPIO12
  Serial.println("  (SDA/SCL read STUCK HIGH normally — that's the bus pull-ups)");
  pinIntegrity(PIN_I2C_SDA, "SDA");     // GPIO8  — shared bus
  pinIntegrity(PIN_I2C_SCL, "SCL");     // GPIO9  — shared bus
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);   // restore the bus
}

// Blink ONLY the backlight — isolates that one FPC pin + the LED string from
// the display data path. If the panel visibly glows on/off, the backlight
// circuit and its FPC contact are good and the fault is in the display DATA
// signals. If nothing happens, the backlight pin/contact is dead too.
static void backlightTest() {
  pinMode(PIN_DISP_BL, OUTPUT);
  Serial.println("=== Backlight blink x5 — watch the panel ===");
  Serial.println("  glows on/off  -> BL pin + FPC + LEDs OK (fault is in display DATA)");
  Serial.println("  stays dark    -> backlight pin/contact also open (broad FPC fault)");
  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_DISP_BL, HIGH); Serial.println("  BL ON");  delay(700);
    digitalWrite(PIN_DISP_BL, LOW);  Serial.println("  BL OFF"); delay(700);
  }
  digitalWrite(PIN_DISP_BL, HIGH);
}

// Scan the bus at a non-default clock — a marginal/long touch stub sometimes
// only works slow.
static void scanAtClock(uint32_t hz) {
  Serial.printf("=== I2C scan @ %lu Hz ===\n", hz);
  Wire.setClock(hz);
  scanBus();
  Wire.setClock(I2C_FREQ_HZ);
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

// Bring the panel up at the current spiHz. Safe to call repeatedly — recreates
// the bus + driver so live SPI-speed changes take effect.
static bool displayInit() {
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);             // backlight off during init

  delete gfx; gfx = nullptr;
  delete bus; bus = nullptr;

  bus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS,
                             PIN_DISP_SCK, PIN_DISP_MOSI,
                             PIN_DISP_MISO, DISP_SPI_HOST);
  gfx = new Arduino_ST7789(bus, PIN_DISP_RST, DISP_ROTATION, DISP_IPS,
                           DISP_W, DISP_H, DISP_COL_OFFSET, DISP_ROW_OFFSET);

  Serial.printf("Display: begin() @ %lu Hz ... ", spiHz);
  if (!gfx->begin(spiHz)) {
    Serial.println("FAILED");
    return false;
  }
  gfx->fillScreen(BLACK);
  digitalWrite(PIN_DISP_BL, HIGH);            // full brightness for the test
  Serial.println("OK (backlight ON)");
  return true;
}

static void displayTest() {
  if (!gfx) { Serial.println("display not initialised"); return; }
  struct { uint16_t c; const char *n; } steps[] = {
    {RED, "RED"}, {GREEN, "GREEN"}, {BLUE, "BLUE"},
    {WHITE, "WHITE"}, {BLACK, "BLACK"},
  };
  for (auto &s : steps) {
    Serial.printf("  fill %s\n", s.n);
    gfx->fillScreen(s.c);
    delay(400);
  }
  // Geometry / text check: border, crosshair, RGB bars, label.
  gfx->fillScreen(BLACK);
  gfx->drawRect(0, 0, DISP_W, DISP_H, WHITE);
  gfx->drawLine(0, 0, DISP_W - 1, DISP_H - 1, CYAN);
  gfx->drawLine(DISP_W - 1, 0, 0, DISP_H - 1, CYAN);
  for (int i = 0; i < DISP_W; i++) {
    gfx->drawFastVLine(i, 20, 20, gfx->color565(i, 0, 0));
    gfx->drawFastVLine(i, 40, 20, gfx->color565(0, i, 0));
    gfx->drawFastVLine(i, 60, 20, gfx->color565(0, 0, i));
  }
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 120);
  gfx->print("DISPLAY OK");
  gfx->setTextSize(1);
  gfx->setCursor(10, 150);
  gfx->printf("%dx%d off(%d,%d)", DISP_W, DISP_H, DISP_COL_OFFSET, DISP_ROW_OFFSET);
  gfx->setCursor(10, 165);
  gfx->printf("SPI %lu Hz", spiHz);
  Serial.println("  pattern drawn — check the panel for border/crosshair/bars/text");
}

// Brute-force the reset-timing + INT-strap space, scanning the bus after each
// combination. Prints any combo where a non-known device answers.
static void touchResetSweep() {
  const uint16_t lows[]  = {1, 5, 10, 50, 100};
  const uint16_t highs[] = {10, 50, 200};
  const int      ints[]  = {-1, 0, 1};
  const char*    intName[] = {"hi-Z", "low ", "high"};
  const bool     invs[]  = {false, true};

  Serial.println("=== Touch RST sweep — probing the bus after each reset ===");
  uint8_t hits = 0;
  for (bool inv : invs) {
    for (int ii = 0; ii < 3; ii++) {
      for (uint16_t hi : highs) {
        for (uint16_t lo : lows) {
          touchResetPulse(hi, lo, 60, ints[ii], inv);
          bool ack15 = pingAddr(I2C_ADDR_TOUCH);
          // Also look for a touch chip that strapped to a different address.
          uint8_t newAddr = 0;
          for (uint8_t a = 0x08; a < 0x78 && !newAddr; a++)
            if (a != I2C_ADDR_TOUCH && !isKnown(a) && pingAddr(a)) newAddr = a;

          if (ack15 || newAddr) {
            hits++;
            Serial.printf("  *** RST=%s INT=%s preHigh=%3u low=%3u -> ",
                          inv ? "inverted" : "normal", intName[ii], hi, lo);
            if (ack15)  Serial.print("0x15 ACK ");
            if (newAddr) Serial.printf("NEW @0x%02X ", newAddr);
            Serial.println();
          }
        }
      }
    }
  }
  if (!hits)
    Serial.println("  no combination produced an ACK — touch chip is not on the bus");
  else
    Serial.printf("  %u combination(s) responded (see above)\n", hits);
}

// Raw register read of the touch frame (regs 0x01..0x06). Does NOT use the INT
// line, so it works even if the IRQ trace is dead — as long as I2C reaches the
// chip.
static bool rawTouchRead(uint8_t &gest, uint8_t &pts, uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 6) != 6) return false;
  gest = Wire.read();
  pts  = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

// Live touch via raw polling. Resets the chip first, then reads coordinates and
// draws them on the panel. Press any key to stop.
static void liveTouchRaw() {
  touchResetPulse(TRST_PRE_HIGH_MS, TRST_LOW_MS, TRST_POST_HIGH_MS, -1);
  if (!pingAddr(I2C_ADDR_TOUCH)) {
    Serial.println("0x15 not responding after reset — cannot read touch");
    return;
  }
  if (gfx) { gfx->fillScreen(BLACK); gfx->setCursor(6, 6); gfx->setTextColor(WHITE);
             gfx->setTextSize(1); gfx->print("RAW TOUCH - press to draw"); }
  Serial.println("Live RAW touch — touch the panel; press any serial key to stop.");
  while (!Serial.available()) {
    uint8_t g, p; uint16_t x, y;
    if (rawTouchRead(g, p, x, y) && p > 0) {
      Serial.printf("  pts=%u gesture=0x%02X x=%4u y=%4u\n", p, g, x, y);
      if (gfx && x < DISP_W && y < DISP_H) gfx->fillCircle(x, y, 4, GREEN);
    }
    delay(20);
  }
  while (Serial.available()) Serial.read();   // flush the stop key
}

// Live touch via the CST816S library (exercises begin()/available()/gesture(),
// which DO depend on the INT line firing).
static void liveTouchLib() {
  Serial.println("CST816S library begin() ...");
  touch.begin(Wire);
  Wire.setClock(I2C_FREQ_HZ);                 // library may drop the bus to 100k
  if (!pingAddr(I2C_ADDR_TOUCH)) {
    Serial.println("0x15 still silent after library begin() — INT/lib path can't help");
    return;
  }
  touch.disable_auto_sleep();
  Serial.println("Library live touch — needs the INT line. Press any key to stop.");
  while (!Serial.available()) {
    if (touch.available()) {
      Serial.printf("  gesture=%s pts=%u x=%d y=%d\n",
                    touch.gesture().c_str(), touch.data.points,
                    touch.data.x, touch.data.y);
      if (gfx) gfx->fillCircle(touch.data.x, touch.data.y, 4, MAGENTA);
    }
    delay(10);
  }
  while (Serial.available()) Serial.read();
}

// True if an external pull-up wins over the ESP's internal pulldown — i.e. the
// module's pull-up on this line is electrically reachable (FPC contact good).
static bool externallyHigh(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(60);
  bool h = digitalRead(pin);
  pinMode(pin, INPUT);
  return h;
}

// Live connection monitor — press / wiggle the FPC connector and watch whether
// the module's pull-ups come and go. Great for finding an intermittent or
// not-fully-seated connector, and for confirming a reflow worked.
static void connectionMonitor() {
  Serial.println("=== Connection monitor — press/wiggle the FPC, press a key to stop ===");
  while (!Serial.available()) {
    bool tr = externallyHigh(PIN_TOUCH_RST);
    bool ti = externallyHigh(PIN_TOUCH_INT);
    bool dr = externallyHigh(PIN_DISP_RST);
    bool ack = pingAddr(I2C_ADDR_TOUCH);
    Serial.printf("  TRST:%-4s TINT:%-4s DRST:%-4s  touch@0x15:%s\n",
                  tr ? "CONN" : "open", ti ? "CONN" : "open",
                  dr ? "CONN" : "open", ack ? "ACK" : "--");
    delay(300);
  }
  while (Serial.available()) Serial.read();
}

// Solder-bridge detector for the FPC signal pins. Drives each pin high with the
// rest held low by internal pulldowns; any other pin that reads high is bridged
// to it. Run this AFTER reflow to catch shorts you may have introduced. (SDA/SCL
// are excluded — their bus pull-ups would false-positive.)
static void shortTest() {
  const int  pins[]  = {PIN_DISP_DC, PIN_DISP_CS, PIN_DISP_SCK, PIN_DISP_MOSI,
                        PIN_DISP_RST, PIN_DISP_BL, PIN_TOUCH_RST, PIN_TOUCH_INT};
  const char *names[] = {"DC", "CS", "SCK", "MOSI", "DRST", "BL", "TRST", "TINT"};
  const int  N = sizeof(pins) / sizeof(pins[0]);
  Serial.println("=== Short/bridge test on FPC signal pins ===");

  // A pin held HIGH by its own internal pulldown is being overridden by an
  // EXTERNAL pull-up (TRST/TINT have module pull-ups). Such a pin would read
  // high against every aggressor and false-positive, so we skip it as a
  // "victim". A real bridge to it is still caught the other way round — when
  // IT is the one driven and a non-pulled pin lights up.
  bool pulled[8] = {false};
  for (int i = 0; i < N; i++) pinMode(pins[i], INPUT_PULLDOWN);
  delayMicroseconds(100);
  for (int i = 0; i < N; i++) {
    pulled[i] = digitalRead(pins[i]);
    if (pulled[i])
      Serial.printf("  (%s has an external pull-up — not used as a victim)\n", names[i]);
  }
  for (int i = 0; i < N; i++) pinMode(pins[i], INPUT);

  bool any = false;
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) if (j != i) pinMode(pins[j], INPUT_PULLDOWN);
    pinMode(pins[i], OUTPUT); digitalWrite(pins[i], HIGH);
    delayMicroseconds(60);
    for (int j = 0; j < N; j++) {
      if (j != i && !pulled[j] && digitalRead(pins[j])) {
        Serial.printf("  BRIDGE: %s <-> %s\n", names[i], names[j]);
        any = true;
      }
    }
    pinMode(pins[i], INPUT);
  }
  if (!any) Serial.println("  no real bridges detected");
  for (int i = 0; i < N; i++) pinMode(pins[i], INPUT);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);   // restore the bus
}

// Continuously re-init the panel and flash full-screen RED/GREEN. Press and
// wiggle the FPC while watching the screen: any flash of colour means a display
// SPI line just made contact — i.e. that joint is marginal/open, not the panel.
static void displayWiggle() {
  Serial.println("=== Display wiggle — press/wiggle the FPC, watch for RED/GREEN ===");
  Serial.println("  any colour flash = a display data line is intermittently connecting");
  bool red = true;
  while (!Serial.available()) {
    displayInit();                       // re-runs panel reset+init each cycle
    if (gfx) gfx->fillScreen(red ? RED : GREEN);
    red = !red;
    delay(400);
  }
  while (Serial.available()) Serial.read();
}

static void printMenu() {
  Serial.println();
  Serial.println("===== DISPLAY + TOUCH TEST MENU =====");
  Serial.println("  d : display test pattern (colours, crosshair, bars, text)");
  Serial.println("  b : backlight-only blink (isolate BL pin from display data)");
  Serial.println("  w : display WIGGLE — re-init + flash RED/GREEN while you press FPC");
  Serial.println("  D : re-init display at current SPI speed");
  Serial.println("  1..5 : set SPI speed 10/27/40/60/80 MHz and re-init display");
  Serial.println("  s : I2C bus scan (full grid)");
  Serial.println("  0 : I2C scan at SLOW clock (100k then 50k then 10k)");
  Serial.println("  g : PIN INTEGRITY test — find a short/open on RST/INT/SDA/SCL");
  Serial.println("  m : live CONNECTION monitor — wiggle the FPC to find bad contact");
  Serial.println("  x : SHORT/BRIDGE test across FPC signal pins (run after reflow)");
  Serial.println("  r : touch RST + INT-strap + polarity SWEEP (brute force)");
  Serial.println("  R : single touch reset using TRST_* defines, then ping 0x15");
  Serial.println("  p : ping touch (0x15) once");
  Serial.println("  t : live touch via RAW register polling (ignores INT line)");
  Serial.println("  T : live touch via CST816S library (needs INT line)");
  Serial.println("  h : this menu");
  Serial.println("=====================================");
}

void setup() {
  // Latch the LDO ON first thing. The normal firmware does this in its power
  // bring-up; this standalone test must do it too or any rail gated by the
  // latch (possibly the display/touch supply) stays off.
  pinMode(PIN_LDO_LATCH, OUTPUT);
  digitalWrite(PIN_LDO_LATCH, HIGH);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\n=== EWatch DISPLAY + TOUCH test ===");
  Serial.println("LDO latch (GPIO17) held HIGH");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Serial.printf("I2C: SDA=GPIO%d SCL=GPIO%d %d Hz\n",
                PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

  displayInit();
  displayTest();
  scanBus();
  printMenu();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  char c = Serial.read();
  switch (c) {
    case 'd': displayTest(); break;
    case 'b': backlightTest(); break;
    case 'w': displayWiggle(); break;
    case 'D': displayInit(); displayTest(); break;
    case '1': spiHz = 10000000UL; displayInit(); displayTest(); break;
    case '2': spiHz = 27000000UL; displayInit(); displayTest(); break;
    case '3': spiHz = 40000000UL; displayInit(); displayTest(); break;
    case '4': spiHz = 60000000UL; displayInit(); displayTest(); break;
    case '5': spiHz = 80000000UL; displayInit(); displayTest(); break;
    case 's': scanBus(); break;
    case '0': scanAtClock(100000); scanAtClock(50000); scanAtClock(10000); break;
    case 'g': pinIntegrityTest(); break;
    case 'm': connectionMonitor(); break;
    case 'x': shortTest(); break;
    case 'r': touchResetSweep(); break;
    case 'R':
      touchResetPulse(TRST_PRE_HIGH_MS, TRST_LOW_MS, TRST_POST_HIGH_MS, -1);
      Serial.printf("reset done; 0x15 %s\n",
                    pingAddr(I2C_ADDR_TOUCH) ? "ACK" : "no response");
      break;
    case 'p':
      Serial.printf("0x15 %s\n", pingAddr(I2C_ADDR_TOUCH) ? "ACK" : "no response");
      break;
    case 't': liveTouchRaw(); break;
    case 'T': liveTouchLib(); break;
    case 'h': printMenu(); break;
    case '\n': case '\r': break;
    default: Serial.printf("? '%c' — press h for menu\n", c); break;
  }
}
