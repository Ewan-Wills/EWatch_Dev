// EWatch on-device LLM feasibility spike (STANDALONE sketch).
//
// Runs Karpathy's llama2.c `stories260K` model (~1 MB, embedded in flash)
// entirely on the EWatch's ESP32-S3FH4R2 (4 MB flash / 2 MB PSRAM). On boot it
// generates a short "story" and streams it to Serial AND the ST7789 display.
// No PC, no WiFi, no network of any kind. Toy-quality output is expected.
//
// Display wiring + init mirror EWatchOS2.1/src/drivers/display.cpp exactly
// (ST7789 240x280 IPS over FSPI, backlight on GPIO21 via S8050 NPN).

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "driver/gpio.h"
#include "pins.h"     // canonical EWatch pin map (display, power latch, button)
#include "power.h"    // latchPower() / unlatchPower() / buttonPressed()
#include "llm.h"

// Display pins come from pins.h (shared with the OS). PIN_DISP_MISO == -1.

static Arduino_DataBus* bus =
    new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS, PIN_DISP_SCK,
                         PIN_DISP_MOSI, PIN_DISP_MISO, FSPI);
// 240x280 IPS, col offset 0, row offset 20 (matches the FPC panel).
static Arduino_GFX* gfx =
    new Arduino_ST7789(bus, PIN_DISP_RST, /*rotation=*/0, /*IPS=*/true,
                       240, 280, 0, 20);

static const int SCREEN_W = 240;
static const int SCREEN_H = 280;
static const int BODY_TOP = 24;   // y below the header

static void resetBody() {
    gfx->fillRect(0, BODY_TOP, SCREEN_W, SCREEN_H - BODY_TOP, BLACK);
    gfx->setCursor(0, BODY_TOP);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    gfx->setTextWrap(true);
}

// Token callback: stream each decoded piece to Serial and the display.
static void emit(const char* piece) {
    Serial.print(piece);
    // wrap to top of the body region when we run out of vertical space
    if (gfx->getCursorY() > SCREEN_H - 10) resetBody();
    gfx->print(piece);
}

static void header(const char* msg) {
    gfx->fillRect(0, 0, SCREEN_W, BODY_TOP, BLACK);
    gfx->setCursor(2, 4);
    gfx->setTextColor(CYAN);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->print(msg);
    gfx->drawFastHLine(0, BODY_TOP - 2, SCREEN_W, DARKGREY);
}

void setup() {
    // FIRST, always: take over the LDO enable so the watch stays on after the
    // user releases SW2. Without this the rail drops the instant the button is
    // let go (the button only momentarily forces the LDO on). Pre-load HIGH on
    // the output register, then enable the driver (no LOW glitch).
    latchPower();
    // A pad hold left by prior firmware (or the flash reset) can pin the latch;
    // release it so our drive actually rules the pin.
    gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
    holdLatch();   // our own hold: rail survives esptool / CDC resets

    Serial.begin(115200);
    delay(300);
    Serial.println("\n== EWatch on-device LLM (llama2.c stories260K) ==");

    // --- bring up the panel (backlight off during init, like the OS driver) ---
    pinMode(PIN_DISP_BL, OUTPUT);
    digitalWrite(PIN_DISP_BL, LOW);
    if (!gfx->begin(60000000)) {
        Serial.println("Display: gfx->begin() FAILED - continuing on Serial only");
    } else {
        gfx->fillScreen(BLACK);
        header("EWatch LLM  loading...");
        resetBody();
        gfx->print("Mapping model from flash...");
        digitalWrite(PIN_DISP_BL, HIGH);  // backlight on after first paint
    }

    // --- build the model (weights mmap'd from flash, state in PSRAM) ---
    const char* err = NULL;
    // temperature 0.9, top-p 0.9 - lively but not unhinged for a toy model.
    unsigned long long seed = (unsigned long long)esp_random() ^ (unsigned long long)micros();
    bool ok = llm_setup(0.9f, 0.9f, seed, &err);

    Serial.printf("heap free: %u, psram free: %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    if (!ok) {
        Serial.printf("llm_setup FAILED: %s\n", err ? err : "unknown");
        header("LLM init FAILED");
        resetBody();
        gfx->setTextColor(RED);
        gfx->printf("init failed:\n%s", err ? err : "unknown");
        return;
    }

    Serial.printf("model: dim=%d layers=%d vocab=%d seq_len=%d params=%ld\n",
                  llm_dim(), llm_n_layers(), llm_vocab_size(), llm_seq_len(),
                  llm_param_count());

    header("EWatch LLM  generating");
    resetBody();

    // --- generate a short story and stream it out ---
    const char* prompt = "Once upon a time";
    gfx->setTextColor(YELLOW);
    gfx->print(prompt);
    gfx->setTextColor(WHITE);
    Serial.print(prompt);

    float tps = 0.0f;
    int n = llm_generate(prompt, 220, emit, &tps);

    Serial.printf("\n-- done: %d tokens, %.2f tok/s --\n", n, tps);

    char foot[48];
    snprintf(foot, sizeof(foot), "EWatch LLM  %d tok  %.1f tok/s", n, tps);
    header(foot);
}

void loop() {
    // Basic power management: hold SW2 for ~1.5 s to cut the rail (the watch
    // has no hard switch; dropping the latch powers it off). Otherwise idle
    // and keep the generated story on screen.
    static uint32_t pressStart = 0;
    if (buttonPressed()) {
        if (pressStart == 0) pressStart = millis();
        else if (millis() - pressStart > 1500) {
            Serial.println("-- power off --");
            unlatchPower();
            for (;;) delay(1000);   // rail is gone; this never returns
        }
    } else {
        pressStart = 0;
    }
    delay(50);
}
