// stocks — live stock tracker. Swipe through tickers pushed from the PC
// companion (pc/stocks_feed.py) over USB serial. Each ticker shows its
// symbol, a big price, a colored day %, and a sparkline of recent closes.
//
// The watch does NOT network (WiFi is gated off). All quote data arrives as
// newline-terminated ASCII frames over the native-USB CDC serial link; see
// NOTES.md for the protocol. A dedicated FreeRTOS RX task parses frames into
// a mutex-guarded in-RAM store and nudges the renderer via model.revision.
#pragma once
#include "view.h"

class StocksView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override;

private:
  int      cur          = 0;   // currently-shown ticker index
  bool     firstFrame   = true;
  uint32_t lastDataRev  = 0xFFFFFFFF;
  int      lastCur      = -1;
  int      lastCount    = -1;
};
