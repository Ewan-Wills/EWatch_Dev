// Text typed in the Write app, shared with the rest of the firmware.
//
// The Write app (handwriting -> text) owns this buffer; any other feature can
// read or replace it. Thread-safe (short critical section), any task.
// The text survives leaving the app and reboots (NVS "ewwrite"/"text").
// Newlines are '\n'. Letters are as typed (upper or lower case), digits 0-9,
// spaces. Max kWriteTextMax chars.
//
//   char buf[kWriteTextMax + 1];
//   size_t n = writeGetText(buf, sizeof buf);   // copy out, NUL-terminated
//   uint32_t r = writeTextRevision();            // changes on every edit
#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr size_t kWriteTextMax = 256;

size_t   writeGetText(char *out, size_t cap);   // returns length (excl. NUL)
void     writeSetText(const char *s);           // replace (truncated to max)
void     writeClearText();
uint32_t writeTextRevision();

// power_mgr: keep the screen on while someone is writing (last input < 60 s).
bool     writeKeepAwake();
