// Persistent settings backed by ESP32 NVS via the Preferences library.
// Lives in its own NVS namespace "basicd" (see storage.cpp) so it never reads
// FirstOS's "ewatch" settings. Add new fields here when adding settings.
//
// BasicDigital has no settings UI for these — they're persisted purely so the
// power/sleep/display/haptic defaults survive a reboot, matching FirstOS. The
// known-networks list FirstOS kept here is gone (no WiFi).
#pragma once
#include <stdint.h>

namespace Storage {
  // Open the NVS namespace. Call once during setup() before load().
  void begin();

  // Pull persisted values into the global model. Missing keys keep their
  // model defaults — first boot is a no-op.
  void load();

  // Push current model values out to NVS. Cheap.
  void save();
}
