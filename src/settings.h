// NVS-backed settings (namespace "claudeface"), set with the `cf set ...`
// console commands (cf_link.cpp) and by TIME frames (UTC offset).
//
//   quiet start/end  whole hours; alerts are visual-only inside (start == end
//                    disables; default 23 -> 8)
//   ambient          backlight % outside the 10 s bright window (default 15)
//   haptics          master switch for the alert patterns (default on)
//   tz               local = UTC + tzOffsetMin (from the bridge's TIME frame)
//
// First boot of this firmware copies the same keys from AuroraOS' "ewatch"
// namespace when they exist (cfQStart / cfQEnd / cfAmbient / cfHaptics /
// tzOffMin), so switching firmware keeps Ewan's settings. AuroraOS' own
// namespace is only read, never written.
//
// Writers: the link task only. Readers: any task, through settingsGet().
#pragma once
#include <stdint.h>

struct CFSettings {
  uint8_t quietStart;
  uint8_t quietEnd;
  uint8_t ambientPct;
  bool    haptics;
  int16_t tzOffsetMin;
};

void       settingsLoad();                 // setup(), before any task starts
CFSettings settingsGet();                  // consistent copy, any task

void settingsSetQuiet(uint8_t start, uint8_t end);
void settingsSetAmbient(uint8_t pct);
void settingsSetHaptics(bool on);
void settingsSetTz(int16_t offsetMin);     // saves only when it changed
