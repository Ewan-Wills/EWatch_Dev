// Claude Face — pure protocol + behaviour logic (no Arduino, no FreeRTOS, no
// hardware). Compiled into the firmware AND into the host unit tests in
// test/host/claude/, so keep it plain C++11.
//
//   * base64 encode/decode (RFC 4648 standard alphabet, '=' padding)
//   * frame parse: one text line `#CF<type> <base64(payload || crc16le)>`
//   * frame encode for the watch -> host direction (EVENT, HELLO)
//   * the haptic trigger state machine (SPEC §4 haptics table)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cf_protocol.h"

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------
// Decode `n` chars of standard base64. Returns decoded length, or -1 on any
// invalid character / bad padding / output overflow.
int    cfB64Decode(const char *in, size_t n, uint8_t *out, size_t outCap);
// Encode; always NUL-terminates. Returns chars written (excl. NUL), or 0 if
// outCap is too small.
size_t cfB64Encode(const uint8_t *in, size_t n, char *out, size_t outCap);

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
enum CFParseResult : uint8_t {
  CF_PARSE_NOT_CF = 0,   // not a #CF line -> caller hands it to the console
  CF_PARSE_QUERY,        // "#CFQ" -> answer with HELLO
  CF_PARSE_OK,           // valid frame, payload is the expected packed struct
  CF_PARSE_BAD,          // #CF line but malformed / bad CRC / wrong size / unknown type
  CF_PARSE_VERSION,      // CRC-valid frame whose version byte != CF_PROTO_VERSION
};

#define CF_MAX_PAYLOAD 128

struct CFFrame {
  char     type;                       // CF_TYPE_STATE / _SESSIONS / _TIME
  uint8_t  version;                    // first payload byte (valid for OK / VERSION)
  uint16_t len;                        // payload length (CRC stripped)
  uint8_t  payload[CF_MAX_PAYLOAD];
};

// Parse one line (with or without trailing CR/LF/spaces). Only host->watch
// types (S, L, T) are accepted.
CFParseResult cfParseLine(const char *line, CFFrame &out);

// Build "#CF<type> <b64>\n" into out. Returns the line length (excl. NUL) or
// 0 if it doesn't fit.
size_t cfEncodeFrame(char type, const void *payload, size_t len,
                     char *out, size_t outCap);

// ---------------------------------------------------------------------------
// Haptic trigger state machine
// ---------------------------------------------------------------------------
enum CFBuzz : uint8_t {
  CF_BUZZ_NONE = 0,
  CF_BUZZ_BLOCKED,   // 3 short pulses (repeats every 60 s, max 5, until ack/resolved)
  CF_BUZZ_DONE,      // 1 medium pulse (turn > 60 s finished)
  CF_BUZZ_CROSS,     // 2 short pulses (5h crossed 80 % / 95 %)
  CF_BUZZ_RESET,     // 1 short pulse (left LIMITED because the window reset)
};

struct CFHapticCfg {
  bool    enabled;
  uint8_t quietStart;   // hour 0..23
  uint8_t quietEnd;     // hour 0..23; == quietStart disables quiet hours
};

#define CF_BLOCKED_REPEAT_MS 60000UL
#define CF_BLOCKED_MAX_FIRES 5
#define CF_DONE_MIN_TURN_S   60
#define CF_WINDOW_SLOP_S     600     // resets_at jitter tolerated within one window

struct CFHapticState {
  // previous STATE (for edge detection)
  bool     havePrev;
  uint8_t  prevPState;
  uint8_t  prevBlockedCount;
  char     prevProject[16];
  // BLOCKED alert
  bool     blkActive, blkAcked;
  uint8_t  blkFires;
  uint32_t blkLastMs;
  // 5h threshold crossings, keyed on five_resets_at
  uint32_t winKey;
  bool     fired80, fired95;
  // LIMITED exit
  bool     wasLimited;
  uint32_t limFiveResets, limSevenResets;
  // a watch-initiated SELECT_SESSION changes the primary session; suppress
  // the DONE edge that swap can fake for this long
  uint32_t selectGuardUntilMs;
  // counters (cf stats)
  uint32_t played[5];
  uint32_t suppressed;
};

void   cfHapticInit(CFHapticState &s);
// Feed a newly received STATE (st != nullptr) and/or a periodic tick
// (st == nullptr, drives the BLOCKED 60 s repeats). Returns the pattern to
// play now, already filtered by cfg.enabled / quiet hours (suppressed
// triggers still advance the state, so nothing bursts out when quiet hours
// end — visual only, as the spec says).
CFBuzz cfHapticStep(CFHapticState &s, const CFStateMsg *st, uint32_t nowMs,
                    bool linkLive, uint8_t localHour, const CFHapticCfg &cfg);
void   cfHapticAck(CFHapticState &s);                    // user tapped: stop repeats
void   cfHapticNoteSelect(CFHapticState &s, uint32_t nowMs);
