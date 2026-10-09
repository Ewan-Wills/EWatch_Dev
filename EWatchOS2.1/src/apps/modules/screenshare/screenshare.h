// src/apps/screenshare.h
//
// Screenshare — mirror a Mac window/region to the watch face over USB CDC.
//
// The Mac companion (pc/screenshare.py) captures a window, downscales it to the
// 240x280 panel, JPEG-encodes it and streams framed JPEGs down the USB serial
// link. This view parses those frames, decodes each one with JPEGDEC straight
// into the shared frame canvas and blits it. Until the first good frame lands
// it shows a "waiting for PC…" state.
//
// Wire protocol (little-endian), one frame:
//     'E' 'W'           2 magic bytes (0x45 0x57)
//     uint32_t len      JPEG payload length in bytes (LE)
//     len bytes         baseline JPEG, already scaled to <= 240x280
//     uint8_t  csum     XOR of all `len` payload bytes
// Watch -> PC telemetry lines are plain ASCII prefixed with ">>" so the PC
// parser can ignore them (and boot-log noise) while hunting for the magic.
//
// Flow control (USB-Serial/JTAG drops RX bytes once its 4 KiB queue is full):
// the PC sends the header plus at most 2048 payload bytes, then waits for
// ">>ACK <bytes so far>" before each further 2048-byte block. After the last
// block + csum it waits for ">>FRAME OK|BAD <n>" before starting a new frame.
#pragma once
#include "view.h"

class ScreenshareView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;

  // OS2.1 frame pacing: the view must redraw continuously while it is up so the
  // serial parser keeps draining USB CDC and decoded frames reach the panel
  // (frames arrive with no touch/event to drive a redraw). ~30 fps ceiling;
  // one render() can still decode several queued frames. Replaces BaseOS's
  // per-cycle model.revision++ pump.
  uint16_t desiredFrameMs() const override { return 33; }

private:
  // Frame-parser state machine.
  enum class Parse : uint8_t { Magic0, Magic1, Len, Payload, Csum };
  Parse    st        = Parse::Magic0;
  uint8_t *jpegBuf   = nullptr;     // PSRAM scratch for one JPEG payload
  uint32_t jpegCap   = 0;           // capacity of jpegBuf
  uint32_t frameLen  = 0;           // declared payload length of current frame
  uint32_t got       = 0;           // payload bytes received so far
  uint8_t  lenBytes  = 0;           // how many of the 4 length bytes seen
  uint8_t  csumAcc   = 0;           // running XOR of payload

  bool     haveFrame = false;       // at least one good frame shown
  bool     waitingDrawn = false;    // waiting screen already painted
  uint32_t frames    = 0;           // decoded OK
  uint32_t bad       = 0;           // dropped (bad csum / decode fail / oversize)
  uint32_t lastFrameMs = 0;         // for the "no data" fallback
  uint32_t lastHelloMs = 0;         // throttle >>READY pings while waiting
  uint32_t lastRxMs    = 0;         // last byte seen (mid-frame stall reset)

  void resetParser();
  void pumpSerial();                // drain available bytes through the FSM
  bool decodeAndBlit(uint32_t len); // JPEG -> frame canvas -> panel
  void drawWaiting();
};
