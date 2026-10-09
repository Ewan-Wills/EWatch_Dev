#pragma once
#include "FS.h"
namespace fs {
class LittleFSFS : public FS {
public:
  bool begin(bool formatOnFail = false, const char *basePath = "/littlefs", uint8_t maxOpenFiles = 10,
             const char *partitionLabel = "spiffs");
  bool format();
  size_t totalBytes();
  size_t usedBytes();
  void end() {}
};
}
extern fs::LittleFSFS LittleFS;
using fs::LittleFSFS;
