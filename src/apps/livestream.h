// StreamView — live frame receiver. A Mac script (tools/stream_to_watch.py)
// resolves `ewatch.local`, opens a TCP connection to port 7878, and streams
// 240×280 RGB565 frames wrapped in an 8-byte header. Frames may be raw or
// LZ4-compressed; the watch decompresses and blits straight to the panel.
//
// Wire protocol (little-endian):
//
//   FrameHeader {
//     uint8_t  magic    = 0xE1;        // sanity / re-sync marker
//     uint8_t  flags;                  // bit 0 = payload is LZ4-compressed
//     uint16_t reserved = 0;           // future use
//     uint32_t len;                    // payload length in bytes (≤ ~140 KB)
//   }
//   uint8_t payload[len];              // raw RGB565 (134400 B) or LZ4 block
//
// render() blocks and drains its own events, same shape as MediaView. The
// watch stays awake for the entire session — there is no auto-sleep while
// the app is open. Exit: physical button OR tap on the top-left back chip.
//
// Whole file gated on EWATCH_ENABLE_STREAM AND EWATCH_ENABLE_WIFI: without
// either, the class and its TCP / mDNS dependencies drop from the build.
#pragma once
#if defined(EWATCH_ENABLE_STREAM) && EWATCH_ENABLE_STREAM && \
    defined(EWATCH_ENABLE_WIFI)   && EWATCH_ENABLE_WIFI
#include "view.h"

class StreamView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &) override {}   // render() drains its own events
};

#endif
