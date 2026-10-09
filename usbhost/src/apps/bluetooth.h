// Bluetooth app — media remote + BLE scanner.
// Remote (main page): the watch is a BLE HID media-key remote ("EWatch
// Remote"). Paired once from the phone/Mac, its play/pause, skip and volume
// buttons drive whatever is playing there, AirPods included. The S3 is BLE
// only (no Classic / A2DP), so the watch itself can't stream audio.
// USB keyboard (Kbd button): a keyboard on a powered hub on the watch's USB-C
// drives the remote: Space/Enter play-pause, Left/Right prev/next, Up/Down
// volume, M mute (usb_kbd_host; the Mac loses the serial port while it's on).
// Scan (top-right button): lists BLE advertisers and decodes Apple
// proximity-pairing adverts so AirPods/Beats show by model.
#pragma once
#include "view.h"

// Background remote service (also driven by the Control Centre).
// Key values match the HID report bits.
enum : uint8_t { BT_KEY_PLAYPAUSE = 1 << 0, BT_KEY_NEXT = 1 << 1, BT_KEY_PREV = 1 << 2,
                 BT_KEY_VOLUP = 1 << 3, BT_KEY_VOLDOWN = 1 << 4, BT_KEY_MUTE = 1 << 5 };
bool btRemoteOn();
bool btRemoteSetOn(bool on);           // blocking (~0.3 s on); false if the stack failed
bool btRemoteConnected();
bool btRemoteFailed();
bool btRemoteSend(uint8_t key);        // press+release; false if not connected
class Arduino_GFX;
void btDrawMediaGlyph(Arduino_GFX *g, uint8_t key, int16_t cx, int16_t cy, uint16_t c);

class BluetoothView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;   // 40 ms while the USB keyboard is on

private:
  enum class Mode : uint8_t { Remote, Scan };
  Mode     mode = Mode::Remote;
  bool     firstDraw = true;
  uint32_t shownGen = 0xFFFFFFFF;
  int      shownConn = -1;    // connection state last drawn on the remote
  uint32_t shownUsbGen = 0xFFFFFFFF;   // USB keyboard state last drawn
  int      scroll = 0;        // first visible scan row
  int      detail = -1;       // index into the scan snapshot, -1 = list

  void setMode(Mode m);
  void goBack();
  void drawRemote();
  void drawRemoteStatus();
  void drawKbdPill();
  void serviceUsbKeys();
  void drawList();
  void drawDetail();
};
