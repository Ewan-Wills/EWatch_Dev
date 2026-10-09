// ewsim_host: arduino-esp32 FS API backed by a host directory (fresh per run).
#pragma once
#include <Arduino.h>
#include <memory>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
namespace fs {
enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };
struct HostFileImpl;
class File : public Stream {
public:
  File() {}
  explicit File(std::shared_ptr<HostFileImpl> p) : _p(p) {}
  size_t write(uint8_t) override;
  size_t write(const uint8_t *buf, size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t read(uint8_t *buf, size_t size);
  size_t readBytes(char *buffer, size_t length) { return read((uint8_t *)buffer, length); }
  bool seek(uint32_t pos, SeekMode mode);
  bool seek(uint32_t pos) { return seek(pos, SeekSet); }
  size_t position() const;
  size_t size() const;
  bool setBufferSize(size_t) { return true; }
  void close();
  operator bool() const;
  time_t getLastWrite() { return 0; }
  const char *path() const;
  const char *name() const;
  boolean isDirectory(void);
  boolean seekDir(long) { return false; }
  File openNextFile(const char *mode = FILE_READ);
  String getNextFileName(void);
  String getNextFileName(bool *isDir);
  void rewindDirectory(void);
  using Print::write;
private:
  std::shared_ptr<HostFileImpl> _p;
};
class FS {
public:
  File open(const char *path, const char *mode = FILE_READ, const bool create = false);
  File open(const String &path, const char *mode = FILE_READ, const bool create = false) { return open(path.c_str(), mode, create); }
  bool exists(const char *path);
  bool exists(const String &path) { return exists(path.c_str()); }
  bool remove(const char *path);
  bool remove(const String &path) { return remove(path.c_str()); }
  bool rename(const char *a, const char *b);
  bool rename(const String &a, const String &b) { return rename(a.c_str(), b.c_str()); }
  bool mkdir(const char *path);
  bool mkdir(const String &path) { return mkdir(path.c_str()); }
  bool rmdir(const char *path);
  bool rmdir(const String &path) { return rmdir(path.c_str()); }
};
}  // namespace fs
using fs::FS;
using fs::File;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
// host side
void ewsimFsSetRoot(const char *dir);
