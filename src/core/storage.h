// Persistent settings backed by ESP32 NVS via the Preferences library.
// Lives in NVS namespace "ewatch". Add new fields here when adding settings.
#pragma once

namespace Storage {
  // Open the NVS namespace. Call once during setup() before load().
  void begin();

  // Pull persisted values into the global model. Missing keys keep their
  // model defaults — first boot is a no-op.
  void load();

  // Push current model values out to NVS. Call from views after the user
  // commits a settings change. Cheap (NVS is wear-levelled).
  void save();
}
