// ewsim_host: the "wire" end of the display bus: a model of the ST7789.
#include <Arduino_GFX_Library.h>
#include "host_sim.h"

EwsimPanel gEwsimPanel = {};

namespace {
struct Wire7789 {
  uint16_t xs = 0, xe = 239, ys = 0, ye = 319;
  uint16_t cx = 0, cy = 0;
  bool ramwr = false;          // inside a RAMWR pixel stream
  uint8_t cmd = 0;             // last command (for parameter bytes)
  uint8_t params[8];
  int nparam = 0;
  bool haveHi = false;         // half a pixel received (byte stream)
  uint8_t hi = 0;
  double usPerByte = 8.0 / 60.0;
  double owed = 0;             // fractional microseconds not yet charged

  void charge(uint64_t nbytes) {
    gEwsimPanel.bytes += nbytes;
    owed += nbytes * usPerByte;
    if (owed >= 1.0) { uint64_t w = (uint64_t)owed; owed -= (double)w; sim_advance_us(w); }
  }
  void command(uint8_t c) {
    finishParams();
    cmd = c; nparam = 0; ramwr = false; haveHi = false;
    charge(1);
    switch (c) {
      case 0x28: gEwsimPanel.dispOn = false; break;
      case 0x29: gEwsimPanel.dispOn = true; break;
      case 0x10: gEwsimPanel.sleeping = true; break;
      case 0x11: gEwsimPanel.sleeping = false; break;
      case 0x01: gEwsimPanel.dispOn = false; gEwsimPanel.sleeping = true; break;   // SWRESET
      case 0x2C: ramwr = true; cx = xs; cy = ys; break;
      default: break;
    }
  }
  void finishParams() {
    if (cmd == 0x2A && nparam >= 4) { xs = (params[0] << 8) | params[1]; xe = (params[2] << 8) | params[3]; }
    if (cmd == 0x2B && nparam >= 4) { ys = (params[0] << 8) | params[1]; ye = (params[2] << 8) | params[3]; }
    nparam = 0;
  }
  inline void pixel(uint16_t p) {
    if (cx < 240 && cy < 320) gEwsimPanel.ram[cy][cx] = p;
    gEwsimPanel.pixels++;
    if (++cx > xe) { cx = xs; if (++cy > ye) cy = ys; }
  }
  void dataByte(uint8_t b) {
    if (ramwr) {
      if (haveHi) { pixel((uint16_t)((hi << 8) | b)); haveHi = false; }
      else { hi = b; haveHi = true; }
    } else if (nparam < 8) {
      params[nparam++] = b;
      if ((cmd == 0x2A || cmd == 0x2B) && nparam == 4) finishParams(), nparam = 4;
    }
  }
  void data16(uint16_t d) {
    if (ramwr && !haveHi) pixel(d);
    else { dataByte(d >> 8); dataByte(d & 0xFF); }
  }
  void pixelsDone() { gEwsimPanel.lastPixelMs = (uint32_t)(sim_now_us() / 1000); }
};
Wire7789 W;
}  // namespace

Arduino_ESP32SPI::Arduino_ESP32SPI(int8_t, int8_t, int8_t, int8_t, int8_t, uint8_t, bool) {}

bool Arduino_ESP32SPI::begin(int32_t speed, int8_t) {
  if (speed > 0) W.usPerByte = 8.0 * 1e6 / (double)speed;
  return true;
}
void Arduino_ESP32SPI::beginWrite() {}
void Arduino_ESP32SPI::endWrite() { W.finishParams(); }
void Arduino_ESP32SPI::writeCommand(uint8_t c) { W.command(c); }
void Arduino_ESP32SPI::writeCommand16(uint16_t c) { W.command((uint8_t)c); }
void Arduino_ESP32SPI::writeCommandBytes(uint8_t *d, uint32_t n) { for (uint32_t i = 0; i < n; i++) W.command(d[i]); }
void Arduino_ESP32SPI::write(uint8_t b) { W.charge(1); W.dataByte(b); W.pixelsDone(); }
void Arduino_ESP32SPI::write16(uint16_t d) { W.charge(2); W.data16(d); W.pixelsDone(); }
void Arduino_ESP32SPI::writeC8D8(uint8_t c, uint8_t d) { W.command(c); W.charge(1); W.dataByte(d); }
void Arduino_ESP32SPI::writeC8D16(uint8_t c, uint16_t d) { W.command(c); W.charge(2); W.dataByte(d >> 8); W.dataByte(d & 0xFF); }
void Arduino_ESP32SPI::writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) {
  W.command(c); W.charge(4);
  W.dataByte(d1 >> 8); W.dataByte(d1 & 0xFF); W.dataByte(d2 >> 8); W.dataByte(d2 & 0xFF);
  W.finishParams();
}
void Arduino_ESP32SPI::writeRepeat(uint16_t p, uint32_t len) {
  W.charge(2ULL * len);
  for (uint32_t i = 0; i < len; i++) W.data16(p);
  W.pixelsDone();
}
void Arduino_ESP32SPI::writePixels(uint16_t *d, uint32_t len) {
  W.charge(2ULL * len);
  for (uint32_t i = 0; i < len; i++) W.data16(d[i]);
  W.pixelsDone();
}
void Arduino_ESP32SPI::writeBytes(uint8_t *d, uint32_t len) {
  W.charge(len);
  for (uint32_t i = 0; i < len; i++) W.dataByte(d[i]);
  W.pixelsDone();
}
void Arduino_ESP32SPI::writeIndexedPixels(uint8_t *d, uint16_t *idx, uint32_t len) {
  W.charge(2ULL * len);
  for (uint32_t i = 0; i < len; i++) W.data16(idx[d[i]]);
  W.pixelsDone();
}
void Arduino_ESP32SPI::writeIndexedPixelsDouble(uint8_t *d, uint16_t *idx, uint32_t len) {
  W.charge(4ULL * len);
  for (uint32_t i = 0; i < len; i++) { W.data16(idx[d[i]]); W.data16(idx[d[i]]); }
  W.pixelsDone();
}

void ewsimPanelFrame(uint16_t *out, bool honourPower) {
  bool dark = honourPower && (!gEwsimPanel.dispOn || gEwsimPanel.sleeping || host_backlight() == 0);
  for (int y = 0; y < 280; y++)
    for (int x = 0; x < 240; x++) out[y * 240 + x] = dark ? 0 : gEwsimPanel.ram[y + 20][x];
}
