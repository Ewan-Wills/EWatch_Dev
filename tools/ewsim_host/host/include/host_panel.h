#pragma once
#include "Arduino_DataBus.h"
#ifndef SPI_MODE0
#define SPI_MODE0 0
#endif

// Host stand-in for GFX's ESP32 hardware-SPI bus. Every byte that would go
// out on the wire drives a model of the ST7789 (CASET/RASET/RAMWR window
// addressing, DISPON/DISPOFF, SLPIN/SLPOUT) holding 240x320 RGB565 panel RAM.
// The simulated clock advances by the time the transfer would take at the
// configured SPI speed (60 MHz on the EWatch), so panel pushes cost what they
// cost on the watch. Never calls its own virtuals (ewsim's TapSPI subclass
// overrides them and calls back down here).
class Arduino_ESP32SPI : public Arduino_DataBus {
public:
  Arduino_ESP32SPI(int8_t dc = GFX_NOT_DEFINED, int8_t cs = GFX_NOT_DEFINED, int8_t sck = GFX_NOT_DEFINED,
                   int8_t mosi = GFX_NOT_DEFINED, int8_t miso = GFX_NOT_DEFINED, uint8_t spi_num = 1,
                   bool is_shared_interface = true);
  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t dataMode = SPI_MODE0) override;
  void beginWrite() override;
  void endWrite() override;
  void writeCommand(uint8_t) override;
  void writeCommand16(uint16_t) override;
  void writeCommandBytes(uint8_t *data, uint32_t len) override;
  void write(uint8_t) override;
  void write16(uint16_t) override;
  void writeC8D8(uint8_t c, uint8_t d) override;
  void writeC8D16(uint8_t c, uint16_t d) override;
  void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) override;
  void writeRepeat(uint16_t p, uint32_t len) override;
  void writePixels(uint16_t *data, uint32_t len) override;
  void writeBytes(uint8_t *data, uint32_t len) override;
  void writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len) override;
  void writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len) override;
};

// ---- host side: the panel model --------------------------------------------
struct EwsimPanel {
  uint16_t ram[320][240];      // ST7789 GRAM (240x320); the watch shows rows 20..299
  bool     dispOn;             // DISPON (0x29) vs DISPOFF (0x28)
  bool     sleeping;           // SLPIN (0x10) vs SLPOUT (0x11)
  uint64_t pixels;             // pixels written since boot
  uint64_t bytes;              // bytes on the bus since boot
  uint32_t lastPixelMs;        // simulated time of the last pixel write
};
extern EwsimPanel gEwsimPanel;
// Visible 240x280 window (row offset 20) as RGB565, black when the panel is
// off or the backlight is 0 (what a camera would see).
void ewsimPanelFrame(uint16_t *out240x280, bool honourPower);
