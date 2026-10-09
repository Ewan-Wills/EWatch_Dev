// Claude Face — USB-CDC link service.
//
// Host -> watch: `#CF<type> <base64(payload||crc16)>` text lines arrive on the
// USB serial. The link task (serial_io.cpp) hands every line that starts
// with "#CF" to cfLinkFeedLine(); valid STATE / SESSIONS frames are copied
// into a double-buffered snapshot (cf_policy.h CFDoubleBuffer) that the UI
// loop reads with cfLinkRead(). Nothing here ever draws, and the UI loop
// never waits on the link.
//
// Watch -> host: HELLO (boot, USB re-attach, `#CFQ`) and EVENT frames
// (ACK / SELECT_SESSION). cfLinkSendEvent() only queues; the link task
// writes the line under the Serial mutex so it can never interleave
// mid-line with log output.
//
// TIME frames set the RV-3028 through rtcRequestSet() (the UI loop owns
// Wire): the RTC holds LOCAL time (UTC epoch + utc_offset_min * 60),
// weekday 0 = Sunday — the same convention as AuroraOS' NTP sync.
#pragma once
#include <stdint.h>
#include "cf_protocol.h"

class Stream;

// HELLO firmware id (char[16] on the wire, NUL-padded).
#define CF_FW_ID "ClaudeFace fw1"

struct CFLinkSnap {
  CFStateMsg    st;
  CFSessionsMsg ss;
  bool     haveState;
  bool     haveSessions;
  bool     everValid;        // any valid frame since boot
  bool     mismatch;         // last CRC-valid frame spoke another version
  uint32_t stateRxMs;        // millis() when `st` arrived
  uint32_t lastValidMs;      // millis() of the last valid frame (any type)
  uint32_t mismatchMs;       // millis() of the last version-mismatch frame
  uint32_t stateRxCount;     // bumps once per accepted STATE frame
};

// Link-task side (called from serial_io.cpp).
void cfLinkInit();                         // before the link task starts
void cfLinkFeedLine(const char *line);     // a line starting with "#CF"
void cfLinkService();                      // link-task loop hook: HELLO + queued EVENTs
bool cfLinkConsole(const char *line);      // "cf ..." commands; true if handled

// Any task.
uint32_t cfLinkRead(CFLinkSnap &out);      // copy the snapshot; returns its generation
void     cfLinkSendEvent(uint8_t code, uint8_t arg);   // queue ACK / SELECT_SESSION

// Liveness helpers over a snapshot copy.
bool cfLinkLive(const CFLinkSnap &s, uint32_t nowMs);          // valid frame < 90 s
bool cfLinkVersionMismatch(const CFLinkSnap &s, uint32_t nowMs);
