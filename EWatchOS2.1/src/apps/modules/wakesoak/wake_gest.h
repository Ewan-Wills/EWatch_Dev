// Wake gesture traces — the accelerometer story behind every soak wake, so a
// proper raise-to-wake gesture can be characterised from labelled data.
//
// While the wake soak is on, each wake (motion, touch, button) records:
//   pre   up to 32 samples at 50 Hz BEFORE the trigger (MMA8451 FIFO, armed
//         at screen-off), oldest first: what the wrist did leading up to it
//   base  the settled pose the raise check compared against
//   chk   the raise check's samples (200 Hz, every 2nd kept = 100 Hz), from
//         the trigger to the verdict (motion wakes only)
//   post  ~1 s after the screen came on, from the I/O task (~35 Hz, timed)
// plus the check verdict and, once known, the soak answer (yes / no /
// timeout / rejected / auto_yes / superseded). The soak CSV row carries the
// same trace_id.
//
// File: LittleFS /wakegest.bin, appended records (capped, see kGestCapBytes):
//   'W' 'G' ver(1) src(1 motion 2 touch 3 button)
//   u32 id  u32 trig_ms(uptime)  u8 why(0 ok 1 not_facing 2 not_steady
//   3 no_turn 4 no_imu 255 n/a)  u8 checkOk  u16 check_ms  i16 base[3]
//   u8 answer(0 yes 1 no 2 timeout 3 rejected 4 auto_yes 5 superseded)
//   u8 preN  u16 chkN  u8 postN
//   preN  x i16[3]           t = -(preN - i) * 20 ms
//   chkN  x i16[3]           t = i * 10 ms
//   postN x {u16 t_ms, i16[3]}   t from the trigger
// All little-endian; raw MMA counts, 4096 = 1 g, screen face up = az < 0.
#pragma once
#include <stdint.h>

enum : uint8_t { GEST_SRC_MOTION = 1, GEST_SRC_TOUCH = 2, GEST_SRC_BUTTON = 3 };

// Render task (screen-off loop): start a trace. Returns its id (0 = off).
uint32_t gestBegin(uint8_t src);
void     gestPre(const int16_t (*xyz)[3], int n);      // FIFO history, oldest first
void     gestBase(int16_t x, int16_t y, int16_t z);
void     gestCheckSample(int16_t x, int16_t y, int16_t z);   // raise-check task
void     gestVerdict(uint8_t why, bool ok, uint16_t checkMs);
void     gestPostStart();                               // screen is on: start post
void     gestFeed(int16_t x, int16_t y, int16_t z);     // I/O task, every accel read
void     gestAnswer(uint32_t id, const char *answer);   // soak row written
void     gestPoll();                                    // render loop: write when complete
void     gestAbandon();                                 // no wake after all (not soak)
uint32_t gestCurrentId();

bool     gestHandleLine(const char *args);              // `soak g...` console
void     gestClear();
void     gestStats(char *out, int n);                   // "traces=N bytes=B full=0"
