// ewstream — low-latency frame sink shared by the screen-mirroring modules
// (screenshare over USB, livestream over WiFi). The transport only moves
// bytes; this file parses packets, decodes JPEG rectangles and pushes them to
// the panel by GDMA, strip by strip, while the next strip decodes.
//
// Packet (little-endian), same on every transport:
//   'E' 'X'          magic
//   uint8  type      1 = JPEG rect, 2 = ping, 3 = snapshot request, 4 = haptic
//                    (haptic: x = ms, y = strength 0-255, no payload)
//   uint8  flags     JPEG: bit0 KEEP = also copy into the snapshot frame
//                    (a PSRAM copy costs ~9 ms per full frame, so the host
//                    sets it only on a full frame before SNAP / on exit)
//   uint16 seq
//   uint16 x, y, w, h   destination rectangle on the 240x280 panel
//   uint32 len       payload bytes (0 for ping / snap)
//   len bytes        baseline JPEG, exactly w x h
//   uint8  csum      XOR of the payload bytes (0 when len == 0)
//
// Watch -> host replies are ASCII lines, all starting with ">>":
//   >>R <seq>                 packet received (the host may send the next one)
//   >>D <seq> <dec_us> <push_us>  rect is on the panel (DMA finished)
//   >>P <seq> <t_us>          ping reply (watch esp_timer clock)
//   >>ID <MAC> reset=R up=MS  after a ping with seq 0: this watch's id (12 hex digits,
//                             so a host can tell several watches apart), why it last
//                             reset (POWERON, PANIC, TASK_WDT, ...) and its uptime
//   >>SNAP 134400             then 240*280 RGB565 little-endian: the KEEP frames
//   >>BAD <why> <n>           dropped packet (checksum, size, decode)
//
// Flow control: the host keeps at most one packet un-acknowledged by >>R, so
// receive of frame N+1 overlaps decode/push of frame N and nothing queues
// (queued frames are latency). The transport itself must not drop bytes:
// USB reads the hardware FIFO directly (the host is NAKed while we're busy);
// TCP is reliable.
#pragma once
#include <Arduino.h>

#if (defined(EW_APP_SCREENSHARE) && EW_APP_SCREENSHARE) || \
    (defined(EW_APP_LIVESTREAM) && EW_APP_LIVESTREAM)
#define EW_STREAM 1
#else
#define EW_STREAM 0
#endif

#if EW_STREAM

// Bytes back to the host (called from either task; must be thread-safe).
typedef void (*EwsWriteFn)(const uint8_t *p, size_t n);

struct EwsStats {
  uint32_t frames, bad;          // shown / dropped since begin
  uint32_t lastFrameMs;          // millis() of the last frame shown
  volatile uint32_t lastPktMs;   // millis() of the last packet of any kind (liveness)
  uint32_t decUs, pushUs;        // last frame: decode CPU time, waiting on DMA
};

bool ewsBegin(EwsWriteFn write);  // allocate buffers + GDMA; false = no memory
void ewsStop();                   // unblock a feeding RX task (call, then stop it)
void ewsEnd();                    // free everything, back to CPU SPI (RX task gone)
// RX side (transport task): parse bytes. Blocks while every frame slot is
// busy, which is the back-pressure. Text lines between packets are passed
// to `line` (e.g. ewt's "sim ..." commands) when given.
void ewsFeed(const uint8_t *p, size_t n, void (*line)(char *) = nullptr);
void ewsResetParser();            // host went away mid-packet
bool ewsInPacket();               // parser is inside a packet (not hunting for magic)
// Render side: wait up to waitMs for a received packet and show it.
// Returns true if something was drawn.
bool ewsService(uint32_t waitMs);
// Copy what was streamed into the ewsim shadow (so `ewt snap` sees it).
void ewsSyncShadow();
const EwsStats &ewsStats();
void ewsReply(const char *fmt, ...);   // ">>..." helper for transports

#endif
