// Xbox Wireless Controller over Bluetooth LE (Series X|S / One with BLE
// firmware 5.x), on NimBLE-Arduino 1.4.3.
//
// Pairing: open "Xbox Pad", turn the controller on, hold its pair button (top
// edge) until the logo flashes fast. The bond is kept in NVS, so afterwards
// just switching the controller on reconnects (while the service is on).
//
// Input report (HID report 1 over GATT 0x2A4D, 16 or 19 bytes, no ID byte):
//   0-1 LX  2-3 LY  4-5 RX  6-7 RY   (uint16, centre ~32768, LY 0 = up)
//   8-9 LT  10-11 RT                  (0..1023)
//   12 hat (0 none, 1 N .. 8 NW clockwise)
//   13 A 01 B 02 X 08 Y 10 LB 40 RB 80
//   14 View 04 Menu 08 Xbox 10 LS 20 RS 40     15 Share 01
#pragma once
#include "view.h"

enum PadButton : uint16_t {
  PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_X = 1 << 2, PAD_Y = 1 << 3,
  PAD_LB = 1 << 4, PAD_RB = 1 << 5, PAD_VIEW = 1 << 6, PAD_MENU = 1 << 7,
  PAD_XBOX = 1 << 8, PAD_LS = 1 << 9, PAD_RS = 1 << 10, PAD_SHARE = 1 << 11,
  PAD_UP = 1 << 12, PAD_DOWN = 1 << 13, PAD_LEFT = 1 << 14, PAD_RIGHT = 1 << 15,
};

struct PadState {
  bool     connected;
  float    lx, ly, rx, ry;   // -1..1 after dead zone; ly/ry: -1 = up
  float    lt, rt;           // 0..1
  uint16_t buttons;          // PadButton bits (d-pad folded in)
  uint32_t reports;          // increments on every input report
  uint8_t  battery;          // %, 0 = unknown
};

void        padBegin();                  // starts the service task (radio off until enabled)
void        padSetEnabled(bool on);      // radio + reconnect loop
bool        padEnabled();
PadState    padGet();
const char *padStatus();                 // human-readable state line
uint32_t    padGen();                    // bumps on state/connection change
void        padRumble(uint8_t strength, uint16_t ms);   // 0..100, best effort

class PadView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 50; }
private:
  bool firstDraw = true;
  uint32_t shownGen = 0xFFFFFFFF, shownReports = 0xFFFFFFFF;
  void drawStatic();
  void drawLive(const PadState &p);
};
