// Claude Face — USB-CDC link service.
//
// Host -> watch: `#CF<type> <base64(payload||crc16)>` text lines arrive on the
// same USB serial as the ewlog console. ewlog's console task hands every line
// that starts with "#CF" to cfLinkFeedLine(); valid STATE / SESSIONS frames
// are copied into a double-buffered snapshot (cf_policy.h CFDoubleBuffer)
// that the render task reads with cfLinkRead(). Nothing here ever draws, and
// the render task never waits on the link.
//
// Watch -> host: HELLO (boot, USB re-attach, `#CFQ`) and EVENT frames
// (ACK / SELECT_SESSION). cfLinkSendEvent() only queues; the console task
// writes the line under ewlog's mutex so it can never interleave mid-line
// with log output.
//
// TIME frames set the RV-3028 through requestSetRTC() — same convention as
// the NTP sync in wifi_svc.cpp: the RTC holds LOCAL time
// (UTC epoch + utc_offset_min * 60), weekday 0 = Sunday.
#pragma once
#include <stdint.h>
#include "cf_protocol.h"

class Stream;

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

// Console-task side (called from ewlog.cpp).
void cfLinkInit();                         // before the console task starts
void cfLinkFeedLine(const char *line);     // a line starting with "#CF"
void cfLinkService();                      // console-task loop hook: HELLO + queued EVENTs
bool cfLinkConsole(const char *line);      // "cf ..." commands; true if handled

// Any task.
uint32_t cfLinkRead(CFLinkSnap &out);      // copy the snapshot; returns its generation
void     cfLinkSendEvent(uint8_t code, uint8_t arg);   // queue ACK / SELECT_SESSION

// Liveness helpers over a snapshot copy.
bool cfLinkLive(const CFLinkSnap &s, uint32_t nowMs);          // valid frame < 90 s
bool cfLinkVersionMismatch(const CFLinkSnap &s, uint32_t nowMs);
