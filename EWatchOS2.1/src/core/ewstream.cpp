// ewstream — see ewstream.h for the packet format and flow control.
//
// Two tasks touch this file:
//   transport RX task (core 0): ewsFeed() parses packets into frame slots
//   render task (core 1):       ewsService() decodes a slot and pushes it
// Slots move between them through two queues (free -> ready -> free), so a
// frame can arrive while the previous one is still on its way to the panel.
//
// Panel push: JPEGDEC hands over 16-row MCU blocks (big-endian RGB565, wire
// order). They are gathered into a full-width strip in DMA-capable RAM and
// sent by GDMA while the next strip decodes (same SPI2 takeover as the Hopper
// and Doom modules: Arduino_GFX sets the address window on the CPU path, GDMA
// feeds MOSI for the pixels). A little-endian copy of what went out is kept in
// PSRAM for snapshots and for the ewsim shadow.
#include "ewstream.h"
#if EW_STREAM

#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdarg.h>
#include "display.h"     // gfx, displayBus
#include "ewsim.h"       // ewsimTapRows
#include "power_mgr.h"   // powerPerfDemand

static constexpr int      PW = 240, PH = 280;
static constexpr int      STRIP_ROWS = 16;              // one 4:2:0 MCU row
static constexpr int      SLOTS = 3;
static constexpr uint32_t SLOT_CAP = 64 * 1024;         // largest JPEG accepted
static constexpr uint8_t  T_JPEG = 1, T_PING = 2, T_SNAP = 3;
static constexpr uint8_t  F_KEEP = 1;                    // JPEG: keep a copy for SNAP / ewsim

// ---------------------------------------------------------------------------
// GDMA pixel push (copy of modules/hopper/hopper_lcd.cpp, own symbols)
// ---------------------------------------------------------------------------
#if defined(EWSIM_HOST) || !defined(CONFIG_IDF_TARGET_ESP32S3)
static bool lcdBegin() { return false; }
static void lcdEnd() {}
static bool lcdSend(const void *, uint32_t) { return false; }
static bool lcdWait() { return true; }
#else
#include <esp_private/gdma.h>
#include <hal/dma_types.h>
#include <soc/spi_struct.h>

static gdma_channel_handle_t sChan = nullptr;
static dma_descriptor_t      sDesc[4];                 // 4 x 4092 B > one strip
static bool                  sBusy = false;
static spi_dev_t *const      sDev = &GPSPI2;           // display = FSPI = SPI2

static bool lcdBegin() {
  if (sChan) return true;
  gdma_channel_alloc_config_t cfg = {};
  cfg.direction = GDMA_CHANNEL_DIRECTION_TX;
  if (gdma_new_channel(&cfg, &sChan) != ESP_OK) { sChan = nullptr; return false; }
  if (gdma_connect(sChan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2)) != ESP_OK) {
    gdma_del_channel(sChan); sChan = nullptr; return false;
  }
  gdma_strategy_config_t st = {};
  st.owner_check = false; st.auto_update_desc = false;
  gdma_apply_strategy(sChan, &st);
  return true;
}

static bool lcdWait() {
  if (!sBusy) return true;
  int64_t t0 = esp_timer_get_time();
  bool ok = true;
  while (sDev->cmd.usr) {
    if (esp_timer_get_time() - t0 > 50000) { ok = false; break; }   // 7.7 KB takes ~1.5 ms
  }
  if (sChan) gdma_stop(sChan);
  sDev->dma_conf.dma_tx_ena = 0;                       // back to CPU-fed MOSI
  sDev->dma_conf.buf_afifo_rst = 1; sDev->dma_conf.buf_afifo_rst = 0;
  sBusy = false;
  return ok;
}

static void lcdEnd() {
  lcdWait();
  if (!sChan) return;
  gdma_disconnect(sChan);
  gdma_del_channel(sChan);
  sChan = nullptr;
}

static bool lcdSend(const void *buf, uint32_t bytes) {
  if (!sChan || bytes == 0 || bytes > sizeof(sDesc) / sizeof(sDesc[0]) * 4092) return false;
  const uint8_t *p = (const uint8_t *)buf;
  uint32_t left = bytes;
  int n = 0;
  while (left) {
    uint32_t len = left > 4092 ? 4092 : left;
    sDesc[n].dw0.size = len;
    sDesc[n].dw0.length = len;
    sDesc[n].dw0.err_eof = 0;
    sDesc[n].dw0.suc_eof = (left == len);
    sDesc[n].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    sDesc[n].buffer = (void *)p;
    sDesc[n].next = (left == len) ? nullptr : &sDesc[n + 1];
    p += len; left -= len; n++;
  }
  sDev->dma_conf.dma_rx_ena = 0;
  sDev->dma_conf.dma_tx_ena = 1;
  sDev->dma_conf.buf_afifo_rst = 1; sDev->dma_conf.buf_afifo_rst = 0;
  sDev->dma_conf.dma_afifo_rst = 1; sDev->dma_conf.dma_afifo_rst = 0;
  gdma_reset(sChan);
  gdma_start(sChan, (intptr_t)&sDesc[0]);
  sDev->ms_dlen.ms_data_bitlen = bytes * 8 - 1;
  sDev->user.usr_mosi = 1;
  sDev->user.usr_miso = 0;
  sDev->cmd.update = 1;
  while (sDev->cmd.update) {}
  sDev->cmd.usr = 1;
  sBusy = true;
  return true;
}
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Slot {
  uint8_t  type, flags;
  uint16_t seq, x, y, w, h;
  uint32_t len;
  uint8_t *buf;
};
static Slot          sSlot[SLOTS];
static QueueHandle_t sFreeQ = nullptr, sReadyQ = nullptr;
static EwsWriteFn    sWrite = nullptr;
static uint16_t     *sFrame = nullptr;         // copy of the panel, wire order (BE), PSRAM
static uint16_t     *sStrip[2] = { nullptr, nullptr };   // BE, DMA-capable
static bool          sDma = false;
static volatile bool sEnding = false;
static bool          sShadowDirty = false;
static EwsStats      sStats;

// parser (RX task only)
enum class Ps : uint8_t { M0, M1, Hdr, Payload, Csum };
static Ps       sPs = Ps::M0;
static uint8_t  sHdr[16];
static uint8_t  sHdrGot = 0;
static int      sCur = -1;                     // slot being filled
static uint32_t sGot = 0;
static uint8_t  sCsum = 0;
static char     sLine[128];
static uint8_t  sLineLen = 0;

void ewsReply(const char *fmt, ...) {
  if (!sWrite) return;
  char b[96];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
  b[n++] = '\n';
  sWrite((const uint8_t *)b, n);
}

bool ewsBegin(EwsWriteFn write) {
  sWrite = write;
  sEnding = false;
  memset(&sStats, 0, sizeof(sStats));
  sStats.lastFrameMs = sStats.lastPktMs = millis();
  if (!sFreeQ)  sFreeQ  = xQueueCreate(SLOTS, sizeof(int));
  if (!sReadyQ) sReadyQ = xQueueCreate(SLOTS, sizeof(int));
  xQueueReset(sFreeQ); xQueueReset(sReadyQ);
  for (int i = 0; i < SLOTS; i++) {
    if (!sSlot[i].buf) sSlot[i].buf = (uint8_t *)ps_malloc(SLOT_CAP);
    if (!sSlot[i].buf) { ewsEnd(); return false; }
    xQueueSend(sFreeQ, &i, 0);
  }
  if (!sFrame) sFrame = (uint16_t *)ps_malloc(PW * PH * 2);
  for (int k = 0; k < 2; k++)
    if (!sStrip[k]) sStrip[k] = (uint16_t *)heap_caps_malloc(PW * STRIP_ROWS * 2,
                                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!sFrame || !sStrip[0] || !sStrip[1]) { ewsEnd(); return false; }
  memset(sFrame, 0, PW * PH * 2);
  sDma = lcdBegin();
  sCur = -1;
  ewsResetParser();
  powerPerfDemand(true);                       // 240 MHz: decode is CPU-bound
  return true;
}

static void finishPush();                      // render section below

void ewsStop() { sEnding = true; }

void ewsEnd() {
  sEnding = true;
  if (gfx) finishPush();
  if (sDma) lcdEnd();
  sDma = false;
  if (sShadowDirty) ewsSyncShadow();
  for (int i = 0; i < SLOTS; i++) { free(sSlot[i].buf); sSlot[i].buf = nullptr; }
  for (int k = 0; k < 2; k++) { heap_caps_free(sStrip[k]); sStrip[k] = nullptr; }
  free(sFrame); sFrame = nullptr;
  sWrite = nullptr;
  powerPerfDemand(false);
}

const EwsStats &ewsStats() { return sStats; }

// sFrame rows -> little endian, STRIP_ROWS at a time through sStrip[0].
static void frameLE(void (*out)(int y, int rows, const uint16_t *px)) {
  for (int y = 0; y < PH; y += STRIP_ROWS) {
    int rows = PH - y < STRIP_ROWS ? PH - y : STRIP_ROWS;
    const uint16_t *in = sFrame + (size_t)y * PW;
    for (int i = 0; i < rows * PW; i++) sStrip[0][i] = __builtin_bswap16(in[i]);
    out(y, rows, sStrip[0]);
  }
}

void ewsSyncShadow() {
  if (!sFrame || !sStrip[0]) return;
  frameLE([](int y, int rows, const uint16_t *px) { ewsimTapRows(y, rows, px); });
  sShadowDirty = false;
}

// ---------------------------------------------------------------------------
// RX: parser
// ---------------------------------------------------------------------------
void ewsResetParser() {
  if (sCur >= 0) { int i = sCur; xQueueSend(sFreeQ, &i, 0); sCur = -1; }
  sPs = Ps::M0; sHdrGot = 0; sGot = 0; sCsum = 0; sLineLen = 0;
}

bool ewsInPacket() { return sPs != Ps::M0; }

static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// Header complete: validate, grab a slot (blocking = back-pressure).
static bool startPacket() {
  uint8_t type = sHdr[0];
  uint16_t seq = rd16(sHdr + 2);
  uint16_t x = rd16(sHdr + 4), y = rd16(sHdr + 6), w = rd16(sHdr + 8), h = rd16(sHdr + 10);
  uint32_t len = sHdr[12] | (sHdr[13] << 8) | (sHdr[14] << 16) | ((uint32_t)sHdr[15] << 24);
  if (type == T_PING) {
    if (len) { sStats.bad++; ewsReply(">>BAD ping %lu", (unsigned long)sStats.bad); return false; }
    ewsReply(">>P %u %llu", seq, (unsigned long long)esp_timer_get_time());
    sStats.lastPktMs = millis();
    sPs = Ps::Csum; sCsum = 0; sCur = -1;
    return true;
  }
  bool okRect = w && h && x + w <= PW && y + h <= PH;
  if ((type == T_JPEG && (!okRect || !len || len > SLOT_CAP)) ||
      (type == T_SNAP && len) || (type != T_JPEG && type != T_SNAP)) {
    sStats.bad++;
    ewsReply(">>BAD hdr %lu", (unsigned long)sStats.bad);
    return false;
  }
  int idx = -1;
  while (xQueueReceive(sFreeQ, &idx, pdMS_TO_TICKS(50)) != pdTRUE) {
    if (sEnding) return false;
  }
  Slot &s = sSlot[idx];
  s.type = type; s.flags = sHdr[1]; s.seq = seq; s.x = x; s.y = y; s.w = w; s.h = h; s.len = len;
  sCur = idx; sGot = 0; sCsum = 0;
  sPs = len ? Ps::Payload : Ps::Csum;
  return true;
}

void ewsFeed(const uint8_t *p, size_t n, void (*line)(char *)) {
  while (n) {
    switch (sPs) {
      case Ps::M0: {
        uint8_t b = *p++; n--;
        if (b == 'E') { sPs = Ps::M1; break; }
        // Text between packets (ewt "sim ..." lines while no host streams).
        if (!line) break;
        if (b == '\n' || b == '\r') {
          if (sLineLen) { sLine[sLineLen] = 0; line(sLine); sLineLen = 0; }
        } else if (sLineLen < sizeof(sLine) - 1 && b >= 0x20 && b < 0x7F) {
          sLine[sLineLen++] = (char)b;
        } else sLineLen = 0;
        break;
      }
      case Ps::M1: {
        uint8_t b = *p++; n--;
        if (b == 'X') { sPs = Ps::Hdr; sHdrGot = 0; sLineLen = 0; }
        else if (b != 'E') {
          // "E" + something else: it was text (e.g. a line starting with E).
          if (line && sLineLen < sizeof(sLine) - 2 && b >= 0x20 && b < 0x7F) {
            sLine[sLineLen++] = 'E'; sLine[sLineLen++] = (char)b;
          }
          sPs = Ps::M0;
        }
        break;
      }
      case Ps::Hdr: {
        sHdr[sHdrGot++] = *p++; n--;
        if (sHdrGot == sizeof(sHdr) && !startPacket()) sPs = Ps::M0;
        break;
      }
      case Ps::Payload: {
        Slot &s = sSlot[sCur];
        uint32_t run = s.len - sGot;
        if (run > n) run = n;
        uint8_t *dst = s.buf + sGot;
        uint8_t c = sCsum;
        for (uint32_t i = 0; i < run; i++) { uint8_t b = p[i]; dst[i] = b; c ^= b; }
        sCsum = c;
        sGot += run; p += run; n -= run;
        if (sGot == s.len) sPs = Ps::Csum;
        break;
      }
      case Ps::Csum: {
        uint8_t b = *p++; n--;
        int idx = sCur;
        sCur = -1;
        sPs = Ps::M0;
        if (idx < 0) break;                              // ping: already answered
        if (b != sCsum) {
          sStats.bad++;
          ewsReply(">>BAD csum %lu", (unsigned long)sStats.bad);
          xQueueSend(sFreeQ, &idx, 0);
          break;
        }
        ewsReply(">>R %u", sSlot[idx].seq);
        sStats.lastPktMs = millis();
        xQueueSend(sReadyQ, &idx, 0);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Render: JPEG -> strips -> GDMA
// ---------------------------------------------------------------------------
static JPEGDEC  sJpeg;
static int      sRx, sRy, sRw, sRh;           // destination rect
static int      sStripY = -1, sStripRows = 0, sK = 0;
static bool     sInFlight = false;
static uint32_t sWaitUs = 0, sCopyUs = 0;
static bool     sKeep = false;               // this rect also goes to sFrame

// A frame's last strip is left on the wire while the next frame decodes;
// its ">>D" goes out once that DMA is done (here, or when the stream idles).
static bool     sPendD = false;
static char     sPendLine[96];

static void finishPush() {
  if (sInFlight) {
    uint32_t t0 = micros();
    if (!lcdWait()) sDma = false;
    static_cast<Arduino_TFT *>(gfx)->endWrite();
    sInFlight = false;
    sWaitUs += micros() - t0;
  }
  if (sPendD) { sPendD = false; ewsReply("%s", sPendLine); }
}

static void pushStrip() {
  if (sStripY < 0) return;
  Arduino_TFT *tft = static_cast<Arduino_TFT *>(gfx);
  int rows = sStripRows, w = sRw, py = sRy + sStripY;
  uint16_t *src = sStrip[sK];
  finishPush();
  tft->startWrite();
  tft->writeAddrWindow(sRx, py, w, rows);
  if (sDma && lcdSend(src, (uint32_t)w * rows * 2)) {
    sInFlight = true;
  } else {
    displayBus()->writeBytes((uint8_t *)src, (uint32_t)w * rows * 2);   // already wire order
    tft->endWrite();
  }
  // KEEP rects: while DMA sends, copy into sFrame (wire order; swapped only
  // when a snapshot or ewsim needs it). PSRAM writes run at ~15 MB/s, so
  // this is 9 ms for a full frame: only KEEP frames pay it.
  uint32_t tc = micros();
  if (sKeep) {
    if (w == PW) memcpy(sFrame + (size_t)py * PW, src, (size_t)w * rows * 2);
    else for (int r = 0; r < rows; r++)
      memcpy(sFrame + (size_t)(py + r) * PW + sRx, src + r * w, (size_t)w * 2);
  }
  sCopyUs += micros() - tc;
  sK ^= 1;
  sStripY = -1;
}

static int jpegDraw(JPEGDRAW *d) {
  if (d->y != sStripY) {
    pushStrip();
    sStripY = d->y;
    sStripRows = d->iHeight;
    if (sStripY + sStripRows > sRh) sStripRows = sRh - sStripY;
    if (sStripRows > STRIP_ROWS) sStripRows = STRIP_ROWS;
    if (sStripRows <= 0) { sStripY = -1; return 1; }
  }
  int bw = d->iWidthUsed > 0 ? d->iWidthUsed : d->iWidth;
  if (d->x + bw > sRw) bw = sRw - d->x;
  if (bw <= 0) return 1;
  uint16_t *dst = sStrip[sK] + d->x;
  for (int r = 0; r < sStripRows; r++)
    memcpy(dst + r * sRw, d->pPixels + r * d->iWidth, (size_t)bw * 2);
  return 1;
}

static bool showJpeg(const Slot &s) {
  sRx = s.x; sRy = s.y; sRw = s.w; sRh = s.h;
  sKeep = s.flags & F_KEEP;
  sStripY = -1; sWaitUs = 0; sCopyUs = 0;      // sInFlight: previous frame's tail
  uint32_t t0 = micros();
  bool ok = sJpeg.openRAM(s.buf, (int)s.len, jpegDraw);
  if (ok) {
    if (sJpeg.getWidth() != s.w || sJpeg.getHeight() != s.h) ok = false;
    else {
      sJpeg.setPixelType(RGB565_BIG_ENDIAN);
      ok = sJpeg.decode(0, 0, 0) == 1;
    }
    sJpeg.close();
  }
  if (ok) pushStrip();                         // last strip: left in flight
  else finishPush();
  uint32_t total = micros() - t0;
  sStats.pushUs = sWaitUs;
  sStats.decUs = total - sWaitUs;
  return ok;
}

static uint32_t sLastDoneUs = 0, sGapUs = 0;

bool ewsService(uint32_t waitMs) {
  int idx;
  uint32_t tq = micros();
  if (xQueueReceive(sReadyQ, &idx, 0) != pdTRUE) {
    finishPush();                              // nothing queued: complete + report now
    if (xQueueReceive(sReadyQ, &idx, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
      // Idle: let `ewt snap` see the last streamed frame.
      if (sShadowDirty && millis() - sStats.lastFrameMs > 150) ewsSyncShadow();
      return false;
    }
  }
  Slot &s = sSlot[idx];
  bool drew = false;
  if (s.type == T_SNAP) {
    finishPush();
    // Little-endian RGB565, 240x280, straight after the header line.
    ewsReply(">>SNAP %u", (unsigned)(PW * PH * 2));
    frameLE([](int, int rows, const uint16_t *px) { sWrite((const uint8_t *)px, (size_t)rows * PW * 2); });
  } else if (gfx && displayBus()) {
    uint32_t tIn = micros();
    sGapUs = sLastDoneUs ? tIn - sLastDoneUs : 0;   // previous frame done -> this one starts
    uint32_t waitedUs = tIn - tq;
    if (showJpeg(s)) {
      sStats.frames++;
      sStats.lastFrameMs = millis();
      if (sKeep) sShadowDirty = true;
      drew = true;
      snprintf(sPendLine, sizeof(sPendLine), ">>D %u %lu %lu %lu %lu %lu", s.seq,
               (unsigned long)sStats.decUs, (unsigned long)sStats.pushUs,
               (unsigned long)sCopyUs, (unsigned long)sGapUs, (unsigned long)waitedUs);
      sPendD = true;
      sLastDoneUs = micros();
    } else {
      sStats.bad++;
      ewsReply(">>BAD jpeg %lu", (unsigned long)sStats.bad);
    }
  }
  xQueueSend(sFreeQ, &idx, 0);
  return drew;
}

#endif  // EW_STREAM
