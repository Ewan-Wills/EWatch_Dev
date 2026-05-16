// Persistent settings backed by ESP32 NVS via the Preferences library.
// Lives in NVS namespace "ewatch". Add new fields here when adding settings.
//
// Known-networks list is kept as an in-memory array guarded by an internal
// mutex; mutations should call knownSave() to commit to NVS. The cap is
// intentionally small — 8 SSIDs is plenty for a watch.
#pragma once
#include <stdint.h>

namespace Storage {
  // Open the NVS namespace. Call once during setup() before load().
  void begin();

  // Pull persisted values into the global model + known-networks cache.
  // Missing keys keep their model defaults — first boot is a no-op.
  void load();

  // Push current model values + known networks out to NVS. Cheap.
  void save();

  // ---- known networks ----
  static constexpr uint8_t KNOWN_MAX     = 8;
  static constexpr uint8_t KNOWN_SSID    = 33;     // 32 + NUL
  static constexpr uint8_t KNOWN_PASS    = 65;     // 64 + NUL

  uint8_t knownCount();
  // Copy entry i into caller-provided buffers. Returns false if i is out of
  // range. Buffers must be at least KNOWN_SSID / KNOWN_PASS bytes.
  bool    knownAt(uint8_t i, char *ssidOut, char *passOut);
  // Add or update (matches existing SSID -> overwrites password). Returns
  // false if the list is full and the SSID is new, or if SSID is empty/too
  // long. Caller should knownSave() afterwards.
  bool    knownUpsert(const char *ssid, const char *password);
  // Delete entry i, shifting the rest down. Returns false on bad index.
  bool    knownRemove(uint8_t i);
  // Linear search for an SSID and copy its password (if found). Returns true
  // if matched.
  bool    knownLookup(const char *ssid, char *passOut);
  // Commit current list to NVS.
  void    knownSave();
}
