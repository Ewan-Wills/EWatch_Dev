// ewsim — host-driven test hooks. See ewsim.h for the overview.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "ewsim.h"
#include "model.h"
#include "view.h"
#include "power_mgr.h"
#include "display.h"
#include <esp_timer.h>
#include <esp_log.h>

// ESP-IDF logs (ESP_LOGx, e.g. bluedroid "E (123) BT_...") are written straight
// to the USB-Serial-JTAG FIFO, NOT through the Arduino HWCDC tx lock, so they
// can land in the middle of a binary snap/rec chunk and corrupt it. Silence
// them while binary data is streaming (ewlog lines are safe: they use Serial).
static int sIdfQuiet = 0;
static esp_log_level_t sIdfSaved = ESP_LOG_INFO;
// Same window: HWCDC gives up on a write after tx_timeout (default 100 ms)
// of a full ring and sends a partial chunk, which desyncs the host. When the
// CPU is busy (BLE scanning) the ISR drains slowly, so allow 1 s per write.
static void idfQuiet(bool on) {
  if (on) {
    if (sIdfQuiet++ == 0) {
      sIdfSaved = esp_log_level_get("*"); esp_log_level_set("*", ESP_LOG_NONE);
      Serial.setTxTimeoutMs(1000);
    }
  } else if (sIdfQuiet > 0 && --sIdfQuiet == 0) {
    esp_log_level_set("*", sIdfSaved);
    Serial.setTxTimeoutMs(100);
  }
}

#if !EW_SIM

Arduino_DataBus *ewsimMakeBus(int8_t dc, int8_t cs, int8_t sck, int8_t mosi,
                              int8_t miso, uint8_t spiNum) {
  return new Arduino_ESP32SPI(dc, cs, sck, mosi, miso, spiNum);
}
bool ewsimHandleLine(char *) { return false; }
void ewsimEnsureAwake() {}

#else

// ---------------------------------------------------------------------------
// Display bus tap: mirror every pixel the panel receives into a shadow frame.
// ST7789 addressing: CASET/RASET set the window (panel coords, row offset
// 20), RAMWR starts a pixel stream that fills it row-major. Pixel data
// arrives as native uint16 (writePixels/writeRepeat/write16) or as wire-order
// bytes (writeBytes/write: MSB first).
// ---------------------------------------------------------------------------
static const int kW = 240, kH = 280, kRowOff = 20;
static uint16_t *sShadow = nullptr;      // kW*kH RGB565, PSRAM
static volatile uint32_t sPxCount = 0;   // pixels pushed to the panel
static volatile int64_t  sLastBusUs = 0; // last pixel write (recorder waits for a quiet bus)
static volatile bool     sTapPaused = false;   // overlay (cursor) pixels skip the shadow

class TapSPI : public Arduino_ESP32SPI {
public:
  using Arduino_ESP32SPI::Arduino_ESP32SPI;

  void writeCommand(uint8_t c) override {
    if (c == 0x2C) { cx = x0; cy = y0; streaming = true; half = false; sLastBusUs = esp_timer_get_time(); }
    else streaming = false;
    Arduino_ESP32SPI::writeCommand(c);
  }
  void writeCommand16(uint16_t c) override {
    streaming = false; Arduino_ESP32SPI::writeCommand16(c);
  }
  void writeCommandBytes(uint8_t *d, uint32_t n) override {
    streaming = false; Arduino_ESP32SPI::writeCommandBytes(d, n);
  }
  void writeC8D8(uint8_t c, uint8_t d) override {
    streaming = false; Arduino_ESP32SPI::writeC8D8(c, d);
  }
  void writeC8D16(uint8_t c, uint16_t d) override {
    streaming = false; Arduino_ESP32SPI::writeC8D16(c, d);
  }
  void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) override {
    if (c == 0x2A) { x0 = d1; x1 = d2; }
    else if (c == 0x2B) { y0 = d1; y1 = d2; }
    streaming = false;
    Arduino_ESP32SPI::writeC8D16D16(c, d1, d2);
  }
  void write(uint8_t b) override {
    if (streaming) {
      if (half) { push((uint16_t)((hi << 8) | b)); half = false; }
      else { hi = b; half = true; }
    }
    Arduino_ESP32SPI::write(b);
  }
  void write16(uint16_t d) override {
    if (streaming) push(d);
    Arduino_ESP32SPI::write16(d);
  }
  void writeRepeat(uint16_t p, uint32_t len) override {
    if (streaming) pushRepeat(p, len);
    Arduino_ESP32SPI::writeRepeat(p, len);
  }
  void writePixels(uint16_t *data, uint32_t len) override {
    if (streaming) pushPixels(data, len);   // before: base may byte-swap in place
    Arduino_ESP32SPI::writePixels(data, len);
  }
  void writeBytes(uint8_t *data, uint32_t len) override {
    if (streaming) for (uint32_t i = 0; i < len; i++) {
      if (half) { push((uint16_t)((hi << 8) | data[i])); half = false; }
      else { hi = data[i]; half = true; }
    }
    Arduino_ESP32SPI::writeBytes(data, len);
  }
  void writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len) override {
    if (streaming) for (uint32_t i = 0; i < len; i++) push(idx[data[i]]);
    Arduino_ESP32SPI::writeIndexedPixels(data, idx, len);
  }
  void writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len) override {
    if (streaming) for (uint32_t i = 0; i < len; i++) { push(idx[data[i]]); push(idx[data[i]]); }
    Arduino_ESP32SPI::writeIndexedPixelsDouble(data, idx, len);
  }

private:
  uint16_t x0 = 0, x1 = kW - 1, y0 = kRowOff, y1 = kRowOff + kH - 1;
  uint16_t cx = 0, cy = 0;
  bool streaming = false, half = false;
  uint8_t hi = 0;

  inline void advance(uint32_t n) {
    cx += n;
    if (cx > x1) { cx = x0; cy++; }
  }
  inline void push(uint16_t p) {
    if (sTapPaused) { advance(1); return; }
    sPxCount++;
    int sy = (int)cy - kRowOff;
    if (sShadow && cx < kW && sy >= 0 && sy < kH) sShadow[sy * kW + cx] = p;
    advance(1);
  }
  // Row-run fast paths: fill/copy up to the end of the current window row.
  void pushRepeat(uint16_t p, uint32_t len) {
    if (sTapPaused) { while (len) { uint32_t run = (uint32_t)(x1 - cx + 1); if (run > len) run = len; len -= run; advance(run); } return; }
    sPxCount += len;
    sLastBusUs = esp_timer_get_time();
    while (len) {
      uint32_t run = (uint32_t)(x1 - cx + 1);
      if (run > len) run = len;
      int sy = (int)cy - kRowOff;
      if (sShadow && sy >= 0 && sy < kH) {
        for (uint32_t i = 0; i < run; i++) {
          uint32_t x = cx + i;
          if (x < (uint32_t)kW) sShadow[sy * kW + x] = p;
        }
      }
      len -= run;
      advance(run);
    }
  }
  void pushPixels(const uint16_t *d, uint32_t len) {
    if (sTapPaused) { while (len) { uint32_t run = (uint32_t)(x1 - cx + 1); if (run > len) run = len; len -= run; advance(run); } return; }
    sPxCount += len;
    sLastBusUs = esp_timer_get_time();
    while (len) {
      uint32_t run = (uint32_t)(x1 - cx + 1);
      if (run > len) run = len;
      int sy = (int)cy - kRowOff;
      if (sShadow && sy >= 0 && sy < kH && cx < kW) {
        uint32_t n = run;
        if (cx + n > (uint32_t)kW) n = kW - cx;
        memcpy(&sShadow[sy * kW + cx], d, n * 2);
      }
      d += run; len -= run;
      advance(run);
    }
  }
};

const uint16_t *ewsimShadow() { return sShadow; }
uint32_t ewsimPxCount() { return sPxCount; }
void ewsimTapPause(bool paused) { sTapPaused = paused; }
void ewsimTapRows(int y0, int rows, const uint16_t *px) {
  if (y0 < 0 || rows <= 0 || y0 + rows > kH) return;
  if (sShadow) memcpy(sShadow + y0 * kW, px, (size_t)rows * kW * 2);
  sPxCount += (uint32_t)rows * kW;
  sLastBusUs = esp_timer_get_time();
}

Arduino_DataBus *ewsimMakeBus(int8_t dc, int8_t cs, int8_t sck, int8_t mosi,
                              int8_t miso, uint8_t spiNum) {
  if (!sShadow) {
    sShadow = (uint16_t *)ps_malloc(kW * kH * 2);
    if (sShadow) memset(sShadow, 0, kW * kH * 2);
  }
  return new TapSPI(dc, cs, sck, mosi, miso, spiNum);
}

// ---------------------------------------------------------------------------
// Simulated inputs (written by the console task, read by the I/O task)
// ---------------------------------------------------------------------------
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static bool     sFinger = false;
static uint16_t sFx = 0, sFy = 0;
static Gesture  sGest = Gesture::None;
static uint16_t sGx = 0, sGy = 0;
static volatile bool sBtn = false;
static bool     sAccOn = false;
static int16_t  sAx = 0, sAy = 0, sAz = 0;
static volatile int  sGoto = -1;
static volatile bool sWake = false;

void ewsimTouch(bool &live, uint16_t &x, uint16_t &y) {
  portENTER_CRITICAL(&sMux);
  if (sFinger) { live = true; x = sFx; y = sFy; }
  portEXIT_CRITICAL(&sMux);
}

Gesture ewsimTakeGesture(uint16_t &x, uint16_t &y) {
  portENTER_CRITICAL(&sMux);
  Gesture g = sGest;
  if (g != Gesture::None) { x = sGx; y = sGy; sGest = Gesture::None; }
  portEXIT_CRITICAL(&sMux);
  return g;
}

bool ewsimButton() { return sBtn; }

void ewsimAccel(bool &ok, int16_t &ax, int16_t &ay, int16_t &az) {
  portENTER_CRITICAL(&sMux);
  if (sAccOn) { ok = true; ax = sAx; ay = sAy; az = sAz; }
  portEXIT_CRITICAL(&sMux);
}

int ewsimTakeGoto() { int g = sGoto; if (g >= 0) sGoto = -1; return g; }

bool ewsimTakeWake() { bool w = sWake; sWake = false; return w; }

// Recorder input track: what the simulated finger / button did, with device
// timestamps, so the host can draw touch markers on the footage.
struct RecInput { uint32_t t; uint8_t kind, down; uint16_t x, y; };
static RecInput sInRing[32];
static volatile uint8_t sInHead = 0, sInTail = 0;
static volatile bool sRecOn = false;

static void recInput(uint8_t kind, bool down, uint16_t x, uint16_t y) {
  if (!sRecOn) return;
  portENTER_CRITICAL(&sMux);
  uint8_t n = (sInHead + 1) & 31;
  if (n != sInTail) { sInRing[sInHead] = { (uint32_t)millis(), kind, (uint8_t)down, x, y }; sInHead = n; }
  portEXIT_CRITICAL(&sMux);
}

static void finger(bool down, uint16_t x = 0, uint16_t y = 0) {
  portENTER_CRITICAL(&sMux);
  sFinger = down;
  if (down) { sFx = x; sFy = y; }
  portEXIT_CRITICAL(&sMux);
  recInput(0, down, x, y);
}

static void setBtn(bool down) { sBtn = down; recInput(1, down, 0, 0); }

static void gesture(Gesture g, uint16_t x, uint16_t y) {
  portENTER_CRITICAL(&sMux);
  sGest = g; sGx = x; sGy = y;
  portEXIT_CRITICAL(&sMux);
}

static void accel(bool on, int16_t x = 0, int16_t y = 0, int16_t z = 0) {
  portENTER_CRITICAL(&sMux);
  sAccOn = on; sAx = x; sAy = y; sAz = z;
  portEXIT_CRITICAL(&sMux);
}

static inline void ms(uint32_t n) { vTaskDelay(pdMS_TO_TICKS(n)); }

// The I/O task polls every 20 ms: give it 3 cycles to see a change and post.
static const uint32_t kSettle = 60;

// Bring the screen back before input, like a real user would (screen-off
// light sleep only polls wake pins). Returns the state it found.
static char ensureAwake() {
  PowerState ps = powerState();
  if (ps == PowerState::ScreenOff) {
    sWake = true;
    uint32_t t0 = millis();
    while (powerState() == PowerState::ScreenOff && millis() - t0 < 1500) ms(10);
    ms(150);
    return 'O';
  }
  return ps == PowerState::Idle ? 'I' : 'A';
}
void ewsimEnsureAwake() { (void)ensureAwake(); }

// Finger path from (x1,y1) to (x2,y2) over durMs, one step per I/O cycle.
static void path(int x1, int y1, int x2, int y2, uint32_t durMs) {
  int steps = (int)(durMs / 20); if (steps < 2) steps = 2;
  finger(true, x1, y1);
  ms(40);
  for (int i = 1; i <= steps; i++) {
    finger(true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
    ms(20);
  }
  ms(20);
  finger(false);
}

static void pressBtn(uint32_t holdMs) {
  setBtn(true); ms(holdMs); setBtn(false); ms(kSettle);
}

static const char *pwrName() {
  switch (powerState()) {
    case PowerState::Active:    return "A";
    case PowerState::Idle:      return "I";
    case PowerState::ScreenOff: return "O";
    default:                    return "D";
  }
}

// ---- snapshot: binary chunks, each one Serial.write (atomic vs log lines) --
//   A5 5A <len u16 LE> <len bytes> <sum16 LE of the bytes>
static void cmdSnap(const char *seq) {
  if (!sShadow) { Serial.printf("S:%s ERR no shadow (psram)\n", seq); return; }
  const size_t total = kW * kH * 2;
  uint8_t *copy = (uint8_t *)ps_malloc(total);   // consistent frame
  if (!copy) { Serial.printf("S:%s ERR no mem\n", seq); return; }
  memcpy(copy, sShadow, total);
  idfQuiet(true);
  Serial.printf("S:%s SNAP %d %d %u\n", seq, kW, kH, (unsigned)total);
  Serial.flush();
  static uint8_t buf[4 + 2048 + 2];
  for (size_t off = 0; off < total; off += 2048) {
    uint16_t n = (uint16_t)((total - off) < 2048 ? (total - off) : 2048);
    buf[0] = 0xA5; buf[1] = 0x5A; buf[2] = n & 0xFF; buf[3] = n >> 8;
    memcpy(buf + 4, copy + off, n);
    uint16_t sum = 0;
    for (uint16_t i = 0; i < n; i++) sum += copy[off + i];
    buf[4 + n] = sum & 0xFF; buf[5 + n] = sum >> 8;
    if (Serial.write(buf, 6 + n) != 6 + n) {   // partial chunk: host must retry
      Serial.flush(); idfQuiet(false); free(copy);
      Serial.printf("\nS:%s ERR snap short write at %u\n", seq, (unsigned)off);
      return;
    }
  }
  Serial.flush();
  idfQuiet(false);
  free(copy);
  Serial.printf("S:%s OK snap\n", seq);
  Serial.flush();
}

static void cmdState(const char *seq) {
  uint8_t scr; uint32_t rev;
  { ModelLock lk; scr = (uint8_t)model.screen; rev = model.revision; }
  Serial.printf("S:%s OK scr=%u pwr=%s bl=%u heap=%u psram=%u up=%lu "
                "rev=%lu px=%lu\n", seq, scr, pwrName(), backlightGet(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                (unsigned long)millis(), (unsigned long)rev,
                (unsigned long)sPxCount);
}


// ---------------------------------------------------------------------------
// Recorder: streams the shadow frame as row deltas while `rec on`.
//   packet = A5 5B <len u16> <payload> <sum16 of payload>, one Serial.write
//   payload 'F' <fid u16> <t_ms u32> <last u8> rows...
//           row = <y u16> <x0 u16> <n u16> <rlelen u16> <PackBits16 data>
//           (header h < 0x80: h+1 literal pixels; h >= 0x80: next pixel x (h-126))
//   payload 'I' <t_ms u32> <kind u8: 0 finger 1 button> <down u8> <x u16> <y u16>
// A frame is captured once the bus has been quiet for 6 ms (so it isn't torn
// mid-flush), or after 200 ms of continuous drawing. Keyframe (all rows) at
// start, every 5 s and on `rec key`, so a lost packet only lasts until then.
// ---------------------------------------------------------------------------
static volatile bool sRecRunning = false, sRecKeyReq = false;
static volatile uint32_t sRecFrames = 0, sRecBytes = 0, sRecDrops = 0;
static uint32_t sRecMinUs = 0;
static uint8_t  sPkt[4 + 4096 + 2];
static size_t   sPktLen = 0;          // payload bytes in sPkt (after the 4-byte header)

static void pktSend() {
  if (!sPktLen) return;
  sPkt[0] = 0xA5; sPkt[1] = 0x5B; sPkt[2] = sPktLen & 0xFF; sPkt[3] = sPktLen >> 8;
  uint16_t sum = 0;
  for (size_t i = 0; i < sPktLen; i++) sum += sPkt[4 + i];
  sPkt[4 + sPktLen] = sum & 0xFF; sPkt[5 + sPktLen] = sum >> 8;
  size_t n = 6 + sPktLen;
  if (Serial.write(sPkt, n) != n) sRecDrops++;
  sRecBytes += n;
  sPktLen = 0;
}
static inline void put8(uint8_t v)   { sPkt[4 + sPktLen++] = v; }
static inline void put16(uint16_t v) { put8(v & 0xFF); put8(v >> 8); }
static inline void put32(uint32_t v) { put16(v & 0xFFFF); put16(v >> 16); }

static size_t rle16(const uint16_t *p, int n, uint8_t *out) {
  size_t o = 0; int i = 0;
  while (i < n) {
    int r = 1;
    while (i + r < n && r < 129 && p[i + r] == p[i]) r++;
    if (r >= 2) { out[o++] = 0x80 | (r - 2); memcpy(out + o, &p[i], 2); o += 2; i += r; continue; }
    int s = i, l = 0;
    while (i < n && l < 128 && !(i + 1 < n && p[i + 1] == p[i])) { i++; l++; }
    out[o++] = (uint8_t)(l - 1); memcpy(out + o, &p[s], l * 2); o += l * 2;
  }
  return o;
}

static void recFlushInputs() {
  while (sInTail != sInHead) {
    RecInput e = sInRing[sInTail];
    sInTail = (sInTail + 1) & 31;
    put8('I'); put32(e.t); put8(e.kind); put8(e.down); put16(e.x); put16(e.y);
    pktSend();
  }
}

static void recTask(void *) {
  uint16_t *prev  = (uint16_t *)ps_malloc(kW * kH * 2);
  uint16_t *stage = (uint16_t *)ps_malloc(kW * kH * 2);
  static uint8_t rowbuf[2 + 240 * 2 + 4];
  if (prev && stage) {
    uint16_t fid = 0;
    uint32_t lastPx = sPxCount - 1;     // force a first frame
    int64_t pendingSince = 0, lastFrameUs = 0, lastKeyUs = 0;
    bool key = true;
    while (sRecOn) {
      recFlushInputs();
      int64_t now = esp_timer_get_time();
      uint32_t px = sPxCount;
      if (now - lastKeyUs > 5000000) key = true;
      if (sRecKeyReq) { sRecKeyReq = false; key = true; }
      if (px == lastPx && !key) { pendingSince = 0; ms(4); continue; }
      if (!pendingSince) pendingSince = now;
      bool quiet = now - sLastBusUs > 6000;
      if ((!quiet && now - pendingSince < 200000) || now - lastFrameUs < (int64_t)sRecMinUs) { ms(2); continue; }

      lastPx = sPxCount;
      memcpy(stage, sShadow, kW * kH * 2);
      pendingSince = 0; lastFrameUs = now;
      if (key) lastKeyUs = now;
      uint32_t t = millis();
      put8('F'); put16(fid); put32(t); put8(0);
      size_t lastFlag = 7;              // offset of <last> in the payload
      for (int y = 0; y < kH; y++) {
        const uint16_t *a = stage + y * kW, *b = prev + y * kW;
        int x0 = 0, x1 = kW - 1;
        if (!key) {
          while (x0 < kW && a[x0] == b[x0]) x0++;
          if (x0 == kW) continue;
          while (x1 > x0 && a[x1] == b[x1]) x1--;
        }
        int n = x1 - x0 + 1;
        size_t rl = rle16(a + x0, n, rowbuf);
        if (sPktLen + 8 + rl > 4096) {
          pktSend();
          put8('F'); put16(fid); put32(t); put8(0);
        }
        put16(y); put16(x0); put16(n); put16(rl);
        memcpy(&sPkt[4 + sPktLen], rowbuf, rl); sPktLen += rl;
      }
      sPkt[4 + lastFlag] = 1;           // mark the frame's final packet
      pktSend();
      memcpy(prev, stage, kW * kH * 2);
      fid++; sRecFrames++; key = false;
    }
    recFlushInputs();
  }
  free(prev); free(stage);
  idfQuiet(false);
  sRecRunning = false;
  vTaskDelete(nullptr);
}

static void cmdRec(const char *seq, const char *a1, int nv, const int *v) {
  if (!strcmp(a1, "on")) {
    if (!sShadow) { Serial.printf("S:%s ERR no shadow\n", seq); return; }
    int fps = nv >= 1 && v[0] > 0 ? v[0] : 30;
    sRecMinUs = 1000000 / fps;
    if (!sRecRunning) {
      sRecFrames = sRecBytes = sRecDrops = 0;
      sInHead = sInTail = 0;
      sRecOn = true; sRecRunning = true;
      idfQuiet(true);                   // released when the task exits
      xTaskCreatePinnedToCore(recTask, "ewrec", 4096, nullptr, 1, nullptr, 0);
    }
    Serial.printf("S:%s OK t=%lu\n", seq, (unsigned long)millis());
  } else if (!strcmp(a1, "off")) {
    sRecOn = false;
    uint32_t t0 = millis();
    while (sRecRunning && millis() - t0 < 2000) ms(5);
    Serial.printf("S:%s OK frames=%lu bytes=%lu drops=%lu\n", seq, (unsigned long)sRecFrames,
                  (unsigned long)sRecBytes, (unsigned long)sRecDrops);
  } else if (!strcmp(a1, "key")) {
    sRecKeyReq = true;
    Serial.printf("S:%s OK\n", seq);
  } else {
    Serial.printf("S:%s ERR usage: rec on [fps] | rec off | rec key\n", seq);
  }
}

static Gesture swipeCode(char d) {
  switch (d) {
    case 'u': return Gesture::SwipeUp;
    case 'd': return Gesture::SwipeDown;
    case 'l': return Gesture::SwipeLeft;
    default:  return Gesture::SwipeRight;
  }
}

bool ewsimHandleLine(char *line) {
  if (strncmp(line, "sim ", 4) != 0) return false;
  // Tokens: <seq> <cmd> [word] [ints...] — the optional word is a1
  // (btn short, swipe l, touch up); the ints follow it.
  char *tok[9] = { nullptr };
  int nt = 0;
  char *save = nullptr;
  for (char *t = strtok_r(line + 4, " ", &save); t && nt < 9; t = strtok_r(nullptr, " ", &save)) {
    tok[nt++] = t;
  }
  if (nt < 2) { Serial.println("S:? ERR usage: sim <seq> <cmd> [args]"); return true; }
  const char *seq = tok[0], *cmd = tok[1];
  int k = 2;
  const char *a1 = "";
  if (k < nt && (isalpha((unsigned char)tok[k][0]))) a1 = tok[k++];
  int v[5] = { -1, -1, -1, -1, -1 };
  int nv = 0;
  while (k < nt && nv < 5) v[nv++] = atoi(tok[k++]);

  if (!strcmp(cmd, "state")) { cmdState(seq); return true; }
  if (!strcmp(cmd, "snap"))  { cmdSnap(seq);  return true; }
  if (!strcmp(cmd, "rec"))   { cmdRec(seq, a1, nv, v); return true; }
  if (!strcmp(cmd, "goto")) {
    if (nv < 1 || !viewFor((Screen)v[0])) { Serial.printf("S:%s ERR bad screen\n", seq); return true; }
    ensureAwake();
    sGoto = v[0];
    uint32_t t0 = millis();
    while (sGoto >= 0 && millis() - t0 < 1000) ms(10);
    ms(kSettle);
    Serial.printf("S:%s OK\n", seq); return true;
  }
  if (!strcmp(cmd, "wake")) {
    Serial.printf("S:%s OK was=%c\n", seq, ensureAwake()); return true;
  }
  if (!strcmp(cmd, "tilt")) {          // tilt <ax> <ay> <az> (raw, 4096 = 1 g) | tilt off
    if (nv >= 3) accel(true, v[0], v[1], v[2]); else accel(false);
    ms(kSettle);
    Serial.printf("S:%s OK\n", seq); return true;
  }
  if (!strcmp(cmd, "shake")) {         // shake [ms]
    uint32_t dur = nv >= 1 ? v[0] : 600;
    for (uint32_t t = 0; t < dur; t += 20) {
      accel(true, ((t / 20) & 1) ? 7000 : -7000, 0, 4096);
      ms(20);
    }
    accel(false); ms(kSettle);
    Serial.printf("S:%s OK\n", seq); return true;
  }

  // Everything below is user input: wake the screen first, like a person.
  ensureAwake();

  if (!strcmp(cmd, "tap") || !strcmp(cmd, "dtap")) {   // tap x y
    if (nv < 2) { Serial.printf("S:%s ERR usage: %s x y\n", seq, cmd); return true; }
    int reps = cmd[0] == 'd' ? 2 : 1;
    for (int r = 0; r < reps; r++) {
      finger(true, v[0], v[1]); ms(80); finger(false); ms(r + 1 < reps ? 100 : 0);
    }
    gesture(reps == 2 ? Gesture::DoubleTap : Gesture::SingleTap, v[0], v[1]);
    ms(kSettle);
  } else if (!strcmp(cmd, "long")) {   // long x y [ms]
    if (nv < 2) { Serial.printf("S:%s ERR usage: long x y [ms]\n", seq); return true; }
    uint32_t hold = nv >= 3 ? v[2] : 1000;
    finger(true, v[0], v[1]);
    ms(hold > 800 ? 800 : hold);
    gesture(Gesture::LongPress, v[0], v[1]);
    if (hold > 800) ms(hold - 800);
    finger(false); ms(kSettle);
  } else if (!strcmp(cmd, "drag")) {   // drag x1 y1 x2 y2 [ms]
    if (nv < 4) { Serial.printf("S:%s ERR usage: drag x1 y1 x2 y2 [ms]\n", seq); return true; }
    path(v[0], v[1], v[2], v[3], nv >= 5 ? v[4] : 300);
    ms(kSettle);
  } else if (!strcmp(cmd, "swipe")) {  // swipe u|d|l|r  (finger path + chip gesture)
    char d = a1[0];
    if (!strchr("udlr", d) || !d) { Serial.printf("S:%s ERR usage: swipe u|d|l|r\n", seq); return true; }
    int x1 = 120, y1 = 140, x2 = 120, y2 = 140;
    if (d == 'l') { x1 = 190; x2 = 50; }
    if (d == 'r') { x1 = 50;  x2 = 190; }
    if (d == 'u') { y1 = 220; y2 = 60; }
    if (d == 'd') { y1 = 60;  y2 = 220; }
    path(x1, y1, x2, y2, 160);
    gesture(swipeCode(d), x2, y2);
    ms(kSettle);
  } else if (!strcmp(cmd, "gest")) {   // gest <code> [x y]  raw CST816S gesture id
    if (nv < 1) { Serial.printf("S:%s ERR usage: gest code [x y]\n", seq); return true; }
    gesture((Gesture)v[0], nv >= 3 ? v[1] : 120, nv >= 3 ? v[2] : 140);
    ms(kSettle);
  } else if (!strcmp(cmd, "touch")) {  // touch down x y | touch up  (raw finger)
    if (!strcmp(a1, "up")) finger(false);
    else if (nv >= 2) finger(true, v[0], v[1]);   // "down"/"move" + x y
    else { Serial.printf("S:%s ERR usage: touch down x y | touch up\n", seq); return true; }
    ms(kSettle);
  } else if (!strcmp(cmd, "btn")) {    // btn short|long|vlong|down|up|hold <ms>
    if      (!strcmp(a1, "short")) pressBtn(150);
    else if (!strcmp(a1, "long"))  pressBtn(1500);
    else if (!strcmp(a1, "vlong")) pressBtn(3500);   // global power-off!
    else if (!strcmp(a1, "down"))  { setBtn(true);  ms(kSettle); }
    else if (!strcmp(a1, "up"))    { setBtn(false); ms(kSettle); }
    else if (!strcmp(a1, "hold") && nv >= 1) pressBtn(v[0]);
    else { Serial.printf("S:%s ERR usage: btn short|long|vlong|down|up|hold ms\n", seq); return true; }
  } else {
    Serial.printf("S:%s ERR unknown sim cmd '%s'\n", seq, cmd);
    return true;
  }
  Serial.printf("S:%s OK\n", seq);
  return true;
}

#endif  // EW_SIM
