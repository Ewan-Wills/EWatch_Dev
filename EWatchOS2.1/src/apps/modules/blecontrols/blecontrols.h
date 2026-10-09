// BLE Controls — receives custom commands from an ewatch.cloud control panel
// (buttons, sliders, toggles, text and number inputs) over Bluetooth LE.
//
// The watch advertises "EWatch Controls" with one custom GATT service while
// this screen is open; the radio is fully off otherwise. The browser (Web
// Bluetooth) writes one command per GATT write to CMD and listens on EVT.
//
// Wire protocol v1 (UTF-8 text, one message per write/notify, <= 180 bytes):
//   web -> watch (CMD, write):   <id>=<value>        e.g. speed=42, go=1, msg=hi
//                                several may be joined with '\n'
//   watch -> web (EVT, notify):  hello app=blecontrols v=1   (on connect)
//                                ack <id>                   (each command)
//                                evt side=1 | evt tap=<row id>  (watch input)
// Reserved ids the watch acts on itself (everything else is just shown):
//   buzz=<ms>   vibrate      msg=<text>  full-screen message for 3 s
//   bl=<0-255>  backlight    clear=1     forget all received values
#pragma once
#include "view.h"

class BleControlsView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 50; }  // drain the RX queue

private:
  bool     firstDraw = true;
  int      shownConn = -1;
  uint32_t shownGen  = 0xFFFFFFFF;
  uint32_t msgUntil  = 0;          // millis() while a msg= overlay is up
  int      scroll    = 0;

  void drawAll();
  void drawStatus();
  void drawRows();
  void drawMsg();
};
