// Doom (copied from modules/hopper/hopper_lcd.cpp by agreement with that
// module; no symbols shared across modules): stream pixel rows to the ST7789 by GDMA instead of CPU polling.
//
// Arduino_GFX's ESP32SPI bus sends pixels through the SPI peripheral's 64-byte
// data buffer and busy-waits on every chunk: measured on the watch, a full
// 240x280 frame took ~40 ms that way (2100 chunks at ~19 us). Here one GDMA
// descriptor chain sends a whole 16-row strip while the CPU draws the next.
//
// The bus stays Arduino_GFX's: the caller does startWrite + writeAddrWindow
// (CPU path, as usual), then doomLcdDmaSend() switches SPI2 to DMA-fed MOSI for
// the pixel phase, and doomLcdDmaWait() switches it back before endWrite.
// The data must be big-endian RGB565 (wire order) in DMA-capable internal RAM.
//
// Host simulator builds (EWSIM_HOST) and any setup failure report "not ready";
// the caller then uses the CPU path (bus->writePixels).

#include <Arduino.h>
#include "doom_lcd.h"

#if defined(EWSIM_HOST) || !defined(CONFIG_IDF_TARGET_ESP32S3)

bool doomLcdDmaBegin() { return false; }
void doomLcdDmaEnd() {}
bool doomLcdDmaSend(const void *, uint32_t) { return false; }
bool doomLcdDmaWait() { return true; }

#else

#include <esp_private/gdma.h>
#include <hal/dma_types.h>
#include <soc/spi_struct.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "ewlog.h"

static gdma_channel_handle_t sChan = nullptr;
static dma_descriptor_t      sDesc[4];          // 4 x 4092 B > one strip (7680 B)
static bool                  sBusy = false;
static spi_dev_t *const      sDev = &GPSPI2;    // display bus = FSPI = SPI2 (drivers/pins.h)

bool doomLcdDmaBegin() {
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
  EWLOGI("DOOM", "lcd_dma=on");
  return true;
}

void doomLcdDmaEnd() {
  doomLcdDmaWait();
  if (!sChan) return;
  gdma_disconnect(sChan);
  gdma_del_channel(sChan);
  sChan = nullptr;
}

bool doomLcdDmaSend(const void *buf, uint32_t bytes) {
  if (!sChan || bytes == 0 || bytes > sizeof(sDesc) / sizeof(sDesc[0]) * 4092) return false;
  // descriptor chain
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
  // SPI2: DMA-fed MOSI for this transaction (same steps as IDF's spi_hal)
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

bool doomLcdDmaWait() {
  if (!sBusy) return true;
  int64_t t0 = esp_timer_get_time();
  bool ok = true;
  while (sDev->cmd.usr) {
    if (esp_timer_get_time() - t0 > 50000) { ok = false; break; }   // 7.7 KB takes ~1 ms
    taskYIELD();                       // Doom: let the console (same priority) run
  }
  if (sChan) gdma_stop(sChan);
  sDev->dma_conf.dma_tx_ena = 0;     // back to CPU-fed MOSI for Arduino_GFX
  sDev->dma_conf.buf_afifo_rst = 1; sDev->dma_conf.buf_afifo_rst = 0;
  sBusy = false;
  if (!ok) EWLOGW("DOOM", "lcd_dma=timeout");
  return ok;
}

#endif
