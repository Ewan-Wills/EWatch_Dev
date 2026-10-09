#pragma once
#include <stdint.h>
// ESP object: fixed, plausible numbers (an ESP32-S3FH4R2 with 2 MB PSRAM).
class EspClass {
public:
  uint32_t getHeapSize();
  uint32_t getFreeHeap();
  uint32_t getMinFreeHeap();
  uint32_t getMaxAllocHeap();
  uint32_t getPsramSize();
  uint32_t getFreePsram();
  uint32_t getMinFreePsram();
  uint32_t getMaxAllocPsram();
  uint32_t getSketchSize() { return 1800000; }
  uint32_t getFreeSketchSpace() { return 3145728 - 1800000; }
  uint32_t getFlashChipSize() { return 4194304; }
  uint32_t getCpuFreqMHz();
  uint32_t getCycleCount();
  const char *getChipModel() { return "ESP32-S3 (ewsim host)"; }
  uint8_t getChipRevision() { return 0; }
  uint8_t getChipCores() { return 2; }
  const char *getSdkVersion() { return "ewsim-host"; }
  uint64_t getEfuseMac() { return 0xBCC9CF6E27ACULL; }
  void restart();
  void deepSleep(uint64_t) { restart(); }
};
extern EspClass ESP;
