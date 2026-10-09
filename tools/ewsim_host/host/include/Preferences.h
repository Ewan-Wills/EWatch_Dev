// ewsim_host: NVS Preferences in memory, fresh every run (seeded by --pref).
#pragma once
#include <Arduino.h>
typedef enum { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID } PreferenceType;
class Preferences {
public:
  bool begin(const char *name, bool readOnly = false, const char *partition_label = nullptr);
  void end() { _ns[0] = 0; }
  bool clear();
  bool remove(const char *key);
  size_t putChar(const char *key, int8_t value) { return putI(key, value, 1); }
  size_t putUChar(const char *key, uint8_t value) { return putI(key, value, 1); }
  size_t putShort(const char *key, int16_t value) { return putI(key, value, 2); }
  size_t putUShort(const char *key, uint16_t value) { return putI(key, value, 2); }
  size_t putInt(const char *key, int32_t value) { return putI(key, value, 4); }
  size_t putUInt(const char *key, uint32_t value) { return putI(key, value, 4); }
  size_t putLong(const char *key, int32_t value) { return putI(key, value, 4); }
  size_t putULong(const char *key, uint32_t value) { return putI(key, value, 4); }
  size_t putLong64(const char *key, int64_t value) { return putI(key, value, 8); }
  size_t putULong64(const char *key, uint64_t value) { return putI(key, (int64_t)value, 8); }
  size_t putFloat(const char *key, float value) { return putBytes(key, &value, sizeof value); }
  size_t putDouble(const char *key, double value) { return putBytes(key, &value, sizeof value); }
  size_t putBool(const char *key, bool value) { return putI(key, value ? 1 : 0, 1); }
  size_t putString(const char *key, const char *value);
  size_t putString(const char *key, String value) { return putString(key, value.c_str()); }
  size_t putBytes(const char *key, const void *value, size_t len);
  bool isKey(const char *key);
  PreferenceType getType(const char *key);
  int8_t getChar(const char *key, int8_t d = 0) { return (int8_t)getI(key, d); }
  uint8_t getUChar(const char *key, uint8_t d = 0) { return (uint8_t)getI(key, d); }
  int16_t getShort(const char *key, int16_t d = 0) { return (int16_t)getI(key, d); }
  uint16_t getUShort(const char *key, uint16_t d = 0) { return (uint16_t)getI(key, d); }
  int32_t getInt(const char *key, int32_t d = 0) { return (int32_t)getI(key, d); }
  uint32_t getUInt(const char *key, uint32_t d = 0) { return (uint32_t)getI(key, d); }
  int32_t getLong(const char *key, int32_t d = 0) { return (int32_t)getI(key, d); }
  uint32_t getULong(const char *key, uint32_t d = 0) { return (uint32_t)getI(key, d); }
  int64_t getLong64(const char *key, int64_t d = 0) { return getI(key, d); }
  uint64_t getULong64(const char *key, uint64_t d = 0) { return (uint64_t)getI(key, (int64_t)d); }
  float getFloat(const char *key, float d = NAN) { float v = d; getBytes(key, &v, sizeof v); return v; }
  double getDouble(const char *key, double d = NAN) { double v = d; getBytes(key, &v, sizeof v); return v; }
  bool getBool(const char *key, bool d = false) { return getI(key, d ? 1 : 0) != 0; }
  size_t getString(const char *key, char *value, size_t maxLen);
  String getString(const char *key, String defaultValue = String());
  size_t getBytesLength(const char *key);
  size_t getBytes(const char *key, void *buf, size_t maxLen);
  size_t freeEntries() { return 400; }
private:
  char _ns[16] = "";
  bool _ro = false;
  size_t putI(const char *key, int64_t v, int width);
  int64_t getI(const char *key, int64_t d);
};
// host side: seed a value before boot ("ns.key=value"; numbers or strings)
bool ewsimPrefSeed(const char *ns, const char *key, const char *value);
