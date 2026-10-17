// ewsim_host: Arduino core + ESP-IDF odds and ends (clock, GPIO, LEDC, ADC,
// heap, PRNG, esp_timer, sleep, Serial console).
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "host_sim.h"
#include <deque>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// ---------------------------------------------------------------- clock
extern "C" unsigned long millis(void) { sim_clock_tick(); return (unsigned long)(uint32_t)(sim_now_us() / 1000ULL); }
extern "C" unsigned long micros(void) { sim_clock_tick(); return (unsigned long)(uint32_t)sim_now_us(); }
extern "C" int64_t esp_timer_get_time(void) { sim_clock_tick(); return (int64_t)sim_now_us(); }
extern "C" void delay(uint32_t ms) { vTaskDelay(ms); }
extern "C" void delayMicroseconds(uint32_t us) { sim_advance_us(us); }
extern "C" void yield(void) { vTaskDelay(0); }

// ---------------------------------------------------------------- GPIO
// EWatch pins whose idle level matters (see src/drivers/pins.h): the side
// button (18) reads LOW = released (ewsim ORs its simulated button in), the
// CST816S INT (14) and RV-3028 INT (2) idle HIGH (active-low), MMA INT1/2 LOW.
static int sPinLevel[64];
static bool sPinWritten[64];
extern "C" void pinMode(uint8_t pin, uint8_t mode) {
  if (pin < 64 && !sPinWritten[pin]) sPinLevel[pin] = (mode == INPUT_PULLUP) ? 1 : sPinLevel[pin];
}
extern "C" void digitalWrite(uint8_t pin, uint8_t val) {
  if (pin < 64) { sPinLevel[pin] = val ? 1 : 0; sPinWritten[pin] = true; }
}
extern "C" int digitalRead(uint8_t pin) {
  switch (pin) {
    case 18: return 0;        // PIN_BTN released
    case 14: return 1;        // PIN_TOUCH_INT idle (active low)
    case 2:  return 1;        // PIN_RTC_INT idle (active low)
    case 3: case 4: return 0; // MMA INT1/INT2 idle
    default: return pin < 64 ? sPinLevel[pin] : 0;
  }
}
extern "C" uint16_t analogRead(uint8_t) { return 2048; }
// Battery divider tap (GPIO5): 1.95 V at the pin = 3.90 V cell on V2.1 (ratio 0.5).
extern "C" uint32_t analogReadMilliVolts(uint8_t pin) { return pin == 5 ? 1950 : 0; }
extern "C" void analogSetPinAttenuation(uint8_t, adc_attenuation_t) {}
extern "C" void analogReadResolution(uint8_t) {}
extern "C" void analogWrite(uint8_t, int) {}

// ---------------------------------------------------------------- LEDC (backlight / haptic PWM)
static uint32_t sLedcDuty[16];
static int sLedcPin[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static uint32_t sLedcFreq[16];
extern "C" uint32_t ledcSetup(uint8_t ch, uint32_t freq, uint8_t) { if (ch < 16) sLedcFreq[ch] = freq; return freq; }
extern "C" uint32_t ledcReadFreq(uint8_t ch) { return ch < 16 ? sLedcFreq[ch] : 0; }
extern "C" void ledcWrite(uint8_t ch, uint32_t duty) {
  if (ch >= 16) return;
  sLedcDuty[ch] = duty;
  // EWSIM_MOTOR_TRACE=1: log every write to the haptic motor PWM (GPIO37) so
  // effect timing can be checked without a watch.
  static const bool trace = getenv("EWSIM_MOTOR_TRACE") != nullptr;
  if (trace && sLedcPin[ch] == 37) fprintf(stderr, "MOTOR %u %u\n", (unsigned)millis(), (unsigned)duty);
}
extern "C" uint32_t ledcRead(uint8_t ch) { return ch < 16 ? sLedcDuty[ch] : 0; }
extern "C" uint32_t ledcWriteTone(uint8_t, uint32_t f) { return f; }
extern "C" void ledcAttachPin(uint8_t pin, uint8_t ch) { if (ch < 16) sLedcPin[ch] = pin; }
extern "C" void ledcDetachPin(uint8_t pin) { for (int &p : sLedcPin) if (p == pin) p = -1; }
uint8_t host_backlight() {
  for (int ch = 0; ch < 16; ch++) if (sLedcPin[ch] == 21) return (uint8_t)sLedcDuty[ch];   // PIN_DISP_BL
  return 0;
}

// ---------------------------------------------------------------- CPU clock
static uint32_t sCpuMhz = 240;
extern "C" bool setCpuFrequencyMhz(uint32_t m) { sCpuMhz = m; return true; }
extern "C" uint32_t getCpuFrequencyMhz(void) { return sCpuMhz; }
extern "C" uint32_t getApbFrequency(void) { return 80000000; }
extern "C" float temperatureRead(void) { return 38.5f; }
uint32_t host_cpu_mhz() { return sCpuMhz; }

// Interrupts never fire on the host: touch/gesture input arrives through ewsim.
extern "C" void attachInterruptArg(uint8_t, void (*)(void *), void *, int) {}
extern "C" void attachInterrupt(uint8_t, void (*)(void), int) {}
extern "C" void detachInterrupt(uint8_t) {}

// ---------------------------------------------------------------- memory
static const uint32_t kHeap = 327680, kPsram = 2097152;
extern "C" void *ps_malloc(size_t n) { return calloc(1, n ? n : 1); }
extern "C" void *ps_calloc(size_t n, size_t s) { return calloc(n ? n : 1, s ? s : 1); }
extern "C" void *ps_realloc(void *p, size_t n) { return realloc(p, n); }
extern "C" bool psramFound(void) { return true; }
extern "C" void *heap_caps_malloc(size_t n, uint32_t) { return calloc(1, n ? n : 1); }
extern "C" void *heap_caps_calloc(size_t n, size_t s, uint32_t) { return calloc(n ? n : 1, s ? s : 1); }
extern "C" void *heap_caps_realloc(void *p, size_t n, uint32_t) { return realloc(p, n); }
extern "C" void *heap_caps_aligned_alloc(size_t a, size_t n, uint32_t) {
  void *p = nullptr;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n ? n : 1) != 0) return nullptr;
  memset(p, 0, n);
  return p;
}
extern "C" void heap_caps_free(void *p) { free(p); }
extern "C" size_t heap_caps_get_free_size(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? 1800000 : 120000; }
extern "C" size_t heap_caps_get_largest_free_block(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? 1700000 : 90000; }
extern "C" size_t heap_caps_get_total_size(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? kPsram : kHeap; }
extern "C" size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps) - 20000; }
extern "C" uint32_t esp_get_free_heap_size(void) { return 120000; }
extern "C" uint32_t esp_get_minimum_free_heap_size(void) { return 100000; }

EspClass ESP;
uint32_t EspClass::getHeapSize() { return kHeap; }
uint32_t EspClass::getFreeHeap() { return 120000; }
uint32_t EspClass::getMinFreeHeap() { return 100000; }
uint32_t EspClass::getMaxAllocHeap() { return 90000; }
uint32_t EspClass::getPsramSize() { return kPsram; }
uint32_t EspClass::getFreePsram() { return 1800000; }
uint32_t EspClass::getMinFreePsram() { return 1700000; }
uint32_t EspClass::getMaxAllocPsram() { return 1700000; }
uint32_t EspClass::getCpuFreqMHz() { return sCpuMhz; }
uint32_t EspClass::getCycleCount() { return (uint32_t)(sim_now_us() * sCpuMhz); }
void EspClass::restart() { esp_restart(); }

// ---------------------------------------------------------------- PRNG (deterministic)
static uint64_t sRng = 0x9E3779B97F4A7C15ULL;
void host_seed_random(uint64_t seed) { sRng = seed ? seed : 0x9E3779B97F4A7C15ULL; }
extern "C" uint32_t esp_random(void) {                // splitmix64
  uint64_t z = (sRng += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return (uint32_t)(z ^ (z >> 31));
}
extern "C" void esp_fill_random(void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  for (size_t i = 0; i < len; i++) p[i] = (uint8_t)esp_random();
}
long random(long howbig) { return howbig <= 0 ? 0 : (long)(esp_random() % (uint32_t)howbig); }
long random(long lo, long hi) { return lo >= hi ? lo : lo + random(hi - lo); }
void randomSeed(unsigned long s) { if (s) host_seed_random(s); }
long map(long x, long in_min, long in_max, long out_min, long out_max) {
  const long dividend = out_max - out_min, divisor = in_max - in_min, delta = x - in_min;
  if (divisor == 0) return -1;
  return (delta * dividend + (divisor / 2)) / divisor + out_min;
}
uint16_t makeWord(uint16_t w) { return w; }
uint16_t makeWord(uint8_t h, uint8_t l) { return (h << 8) | l; }

// ---------------------------------------------------------------- misc ESP-IDF
extern "C" esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
extern "C" void esp_restart(void) { ewsimHostPanic("firmware called esp_restart() (the watch would reboot)", __FILE__, __LINE__); }
static esp_log_level_t sLogLvl = ESP_LOG_INFO;
extern "C" void esp_log_level_set(const char *, esp_log_level_t l) { sLogLvl = l; }
extern "C" esp_log_level_t esp_log_level_get(const char *) { return sLogLvl; }
extern "C" const char *esp_err_to_name(esp_err_t c) { return c == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
extern "C" bool getLocalTime(struct tm *, uint32_t) { return false; }   // no SNTP (WiFi not simulated)
extern "C" void configTime(long, int, const char *, const char *, const char *) {}

extern "C" esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return ESP_SLEEP_WAKEUP_UNDEFINED; }
extern "C" esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return ESP_OK; }
extern "C" esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t, int) { return ESP_OK; }
extern "C" esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, esp_sleep_ext1_wakeup_mode_t) { return ESP_OK; }
extern "C" esp_err_t esp_sleep_enable_gpio_wakeup(void) { return ESP_OK; }
extern "C" esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t) { return ESP_OK; }
// The host is "on USB" (HWCDC::isPlugged() == true), so the power manager uses
// its simulated sleep; real light/deep sleep would end the simulation.
extern "C" esp_err_t esp_light_sleep_start(void) {
  ewsimHostPanic("esp_light_sleep_start() reached (not simulated; the host is always 'on USB')", __FILE__, __LINE__);
}
extern "C" void esp_deep_sleep_start(void) {
  ewsimHostPanic("watch entered deep sleep / power-off (esp_deep_sleep_start)", __FILE__, __LINE__);
}

// ---------------------------------------------------------------- esp_timer (dispatch task)
struct HEspTimer {
  esp_timer_create_args_t args;
  bool active = false;
  uint64_t due = 0, period = 0;
};
static std::vector<HEspTimer *> sTimers;
static SemaphoreHandle_t sTimerSig = nullptr;
static void espTimerTask(void *) {
  for (;;) {
    HEspTimer *next = nullptr;
    for (HEspTimer *t : sTimers)
      if (t->active && (!next || t->due < next->due)) next = t;
    if (!next) { xSemaphoreTake(sTimerSig, portMAX_DELAY); continue; }
    uint64_t now = sim_now_us();
    if (next->due > now) {
      uint64_t waitMs = (next->due - now + 999) / 1000;
      xSemaphoreTake(sTimerSig, (TickType_t)waitMs);
      continue;
    }
    if (next->period) next->due += next->period; else next->active = false;
    next->args.callback(next->args.arg);
  }
}
extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) {
  if (!sTimerSig) {
    sTimerSig = xSemaphoreCreateBinary();
    sim_create_task(espTimerTask, "esp_timer", nullptr, 22, 0);
  }
  HEspTimer *t = new HEspTimer();
  t->args = *a;
  sTimers.push_back(t);
  *out = t;
  return ESP_OK;
}
extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
  t->due = sim_now_us() + us; t->period = 0; t->active = true; xSemaphoreGive(sTimerSig); return ESP_OK;
}
extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) {
  t->due = sim_now_us() + us; t->period = us ? us : 1; t->active = true; xSemaphoreGive(sTimerSig); return ESP_OK;
}
extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t t) { t->active = false; return ESP_OK; }
extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t t) { t->active = false; return ESP_OK; }
extern "C" bool esp_timer_is_active(esp_timer_handle_t t) { return t->active; }

// ---------------------------------------------------------------- Serial console
HWCDC Serial;
HWCDC Serial0;
static std::deque<uint8_t> sRx;
static std::string sTx;
static bool sEcho = false;
void host_serial_inject(const char *line) { while (*line) sRx.push_back((uint8_t)*line++); }
size_t host_serial_tx_size() { return sTx.size(); }
const char *host_serial_tx_data() { return sTx.c_str(); }
void host_serial_set_echo(bool on) { sEcho = on; }
int HWCDC::available(void) { return this == &Serial ? (int)sRx.size() : 0; }
int HWCDC::peek(void) { return (this == &Serial && !sRx.empty()) ? sRx.front() : -1; }
int HWCDC::read(void) {
  if (this != &Serial || sRx.empty()) return -1;
  int c = sRx.front(); sRx.pop_front(); return c;
}
size_t HWCDC::read(uint8_t *b, size_t n) {
  size_t i = 0;
  while (i < n) { int c = read(); if (c < 0) break; b[i++] = (uint8_t)c; }
  return i;
}
size_t HWCDC::write(uint8_t c) { return write(&c, 1); }
size_t HWCDC::write(const uint8_t *b, size_t n) {
  sTx.append((const char *)b, n);
  if (sEcho) fwrite(b, 1, n, stderr);
  return n;
}

// ---------------------------------------------------------------- notes / unsupported hardware
static std::vector<std::string> sUnsupported, sNotes;
static std::string sUnsList, sNoteList;
void ewsimHostUnsupported(const char *what) {
  for (auto &s : sUnsupported) if (s == what) return;
  sUnsupported.push_back(what);
  sUnsList += sUnsList.empty() ? "" : "; ";
  sUnsList += what;
  fprintf(stderr, "ewsim: UNSUPPORTED on the host: %s (t=%.2fs)\n", what, sim_now_us() / 1e6);
}
void ewsimHostNote(const char *what) {
  for (auto &s : sNotes) if (s == what) return;
  sNotes.push_back(what);
  sNoteList += sNoteList.empty() ? "" : "; ";
  sNoteList += what;
}
int host_unsupported_count() { return (int)sUnsupported.size(); }
const char *host_unsupported_list() { return sUnsList.c_str(); }
const char *host_notes() { return sNoteList.c_str(); }

extern "C" void ewsimHostPanic(const char *what, const char *file, int line) {
  fflush(stdout);
  fprintf(stderr, "\newsim: FIRMWARE PANIC in task '%s' at t=%.3fs: %s (%s:%d)\n",
          sim_current_task_name(), sim_now_us() / 1e6, what, file, line);
  // last console lines, like ewt's crash report
  const std::string &tx = sTx;
  size_t pos = tx.size();
  for (int n = 0; n < 12 && pos > 0; n++) {
    size_t p = tx.rfind('\n', pos - 1);
    if (p == std::string::npos) { pos = 0; break; }
    pos = p;
  }
  fprintf(stderr, "console tail:\n%s\n", tx.substr(pos).c_str());
  fflush(stderr);
  _exit(70);
}

// FunctionalInterrupt (CST816S library): never fires on the host.
#include <FunctionalInterrupt.h>
void attachInterrupt(uint8_t, std::function<void(void)>, int) {}
