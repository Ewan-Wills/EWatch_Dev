// Bluetooth app — media remote + BLE scanner.
// Remote (main page): the watch is a BLE HID media-key remote ("EWatch
// Remote"). Paired once from the phone/Mac, its play/pause, skip and volume
// buttons drive whatever is playing there, AirPods included. The S3 is BLE
// only (no Classic / A2DP), so the watch itself can't stream audio.
// Scan (top-right button): lists BLE advertisers and decodes Apple
// proximity-pairing adverts so AirPods/Beats show by model.
#pragma once
#include "view.h"

class BluetoothView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 200; }

private:
  enum class Mode : uint8_t { Remote, Scan };
  Mode     mode = Mode::Remote;
  bool     firstDraw = true;
  uint32_t shownGen = 0xFFFFFFFF;
  int      shownConn = -1;    // connection state last drawn on the remote
  int      scroll = 0;        // first visible scan row
  int      detail = -1;       // index into the scan snapshot, -1 = list

  void setMode(Mode m);
  void goBack();
  void drawRemote();
  void drawRemoteStatus();
  void drawList();
  void drawDetail();
};
