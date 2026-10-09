// USB Host scanner — turns the watch's USB-C port into a USB host for a timed
// scan window and lists every device that enumerates (VID:PID, class, strings,
// interfaces), as the first step towards a USB keyboard on the watch.
//
// The S3 has ONE internal USB PHY. Normally it belongs to USB-Serial/JTAG (the
// console + flashing). A scan hands it to the OTG controller in host mode, so
// the Mac loses the serial port for the window; when it ends (30 s, or a side
// button press) the PHY goes back to Serial/JTAG and the results stay on
// screen, so ewt can reconnect, snap and `log dump` the USBH lines.
//
// Hardware limits (see usbhost/NOTES.md):
//   * the watch cannot source 5 V on VBUS: the device needs its own power
//     (a powered hub, or an OTG adapter with a charging/power input);
//   * ESP-IDF 4.4 (arduino-esp32 2.0.17) has no hub driver: a hub enumerates
//     as one class-0x09 device and devices behind it stay invisible.
#pragma once
#include "view.h"

class UsbHostView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 200; }   // countdown

private:
  bool     firstDraw = true;
  uint32_t shownGen  = 0xFFFFFFFF;
  int      shownSecs = -1;
  int      shownState = -1;
  int      scroll = 0;
  uint32_t startAt = 0;      // first scan starts 1.5 s after entering (lets ewt finish its goto)

  void drawAll();
  void drawStatus();
  void drawList();
};
