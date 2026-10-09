// Persistent settings backed by ESP32 NVS via the Preferences library.
// Lives in NVS namespace "ewatch". Add new fields here when adding settings.
#pragma once
#include <stdint.h>

namespace Storage {
  // Open the NVS namespace. Call once during setup() before load().
  void begin();

  // Pull persisted values into the global model. Missing keys keep their model
  // defaults — first boot is a no-op.
  void load();

  // Push current model values out to NVS. Cheap.
  void save();
}
