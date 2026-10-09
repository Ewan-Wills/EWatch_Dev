// ewsim_host: Preferences (NVS) in memory and LittleFS on a host directory.
#include <Preferences.h>
#include <LittleFS.h>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// ---------------------------------------------------------------- Preferences
struct PVal { bool isInt; int64_t i; std::vector<uint8_t> b; };
static std::map<std::string, PVal> sNvs;     // "ns/key"
static std::string pk(const char *ns, const char *key) { return std::string(ns) + "/" + key; }

bool ewsimPrefSeed(const char *ns, const char *key, const char *value) {
  char *end = nullptr;
  long long v = strtoll(value, &end, 0);
  PVal p;
  if (end && *end == 0 && *value) { p.isInt = true; p.i = v; }
  else { p.isInt = false; p.i = 0; p.b.assign(value, value + strlen(value) + 1); }
  sNvs[pk(ns, key)] = p;
  return true;
}

bool Preferences::begin(const char *name, bool ro, const char *) {
  snprintf(_ns, sizeof _ns, "%s", name ? name : "");
  _ro = ro;
  return true;
}
bool Preferences::clear() {
  std::string pre = std::string(_ns) + "/";
  for (auto it = sNvs.begin(); it != sNvs.end();)
    it = it->first.compare(0, pre.size(), pre) == 0 ? sNvs.erase(it) : std::next(it);
  return true;
}
bool Preferences::remove(const char *key) { return sNvs.erase(pk(_ns, key)) > 0; }
size_t Preferences::putI(const char *key, int64_t v, int width) {
  if (_ro || !_ns[0]) return 0;
  PVal p; p.isInt = true; p.i = v;
  sNvs[pk(_ns, key)] = p;
  return (size_t)width;
}
int64_t Preferences::getI(const char *key, int64_t d) {
  auto it = sNvs.find(pk(_ns, key));
  return (it != sNvs.end() && it->second.isInt) ? it->second.i : d;
}
size_t Preferences::putString(const char *key, const char *value) {
  if (_ro || !_ns[0]) return 0;
  PVal p; p.isInt = false; p.i = 0; p.b.assign(value, value + strlen(value) + 1);
  sNvs[pk(_ns, key)] = p;
  return strlen(value);
}
size_t Preferences::putBytes(const char *key, const void *value, size_t len) {
  if (_ro || !_ns[0]) return 0;
  PVal p; p.isInt = false; p.i = 0; p.b.assign((const uint8_t *)value, (const uint8_t *)value + len);
  sNvs[pk(_ns, key)] = p;
  return len;
}
bool Preferences::isKey(const char *key) { return sNvs.count(pk(_ns, key)) > 0; }
PreferenceType Preferences::getType(const char *key) {
  auto it = sNvs.find(pk(_ns, key));
  if (it == sNvs.end()) return PT_INVALID;
  return it->second.isInt ? PT_I32 : PT_BLOB;
}
size_t Preferences::getString(const char *key, char *value, size_t maxLen) {
  auto it = sNvs.find(pk(_ns, key));
  if (it == sNvs.end() || it->second.isInt || !maxLen) return 0;
  size_t n = std::min(maxLen - 1, strnlen((const char *)it->second.b.data(), it->second.b.size()));
  memcpy(value, it->second.b.data(), n);
  value[n] = 0;
  return n + 1;
}
String Preferences::getString(const char *key, String d) {
  auto it = sNvs.find(pk(_ns, key));
  if (it == sNvs.end() || it->second.isInt) return d;
  return String((const char *)it->second.b.data());
}
size_t Preferences::getBytesLength(const char *key) {
  auto it = sNvs.find(pk(_ns, key));
  return (it == sNvs.end() || it->second.isInt) ? 0 : it->second.b.size();
}
size_t Preferences::getBytes(const char *key, void *buf, size_t maxLen) {
  auto it = sNvs.find(pk(_ns, key));
  if (it == sNvs.end() || it->second.isInt) return 0;
  size_t n = std::min(maxLen, it->second.b.size());
  memcpy(buf, it->second.b.data(), n);
  return n;
}

// ---------------------------------------------------------------- LittleFS on a host directory
static std::string sRoot = "/tmp/ewsim-fs";
void ewsimFsSetRoot(const char *dir) { sRoot = dir; }
static std::string hp(const char *p) { return sRoot + (p && p[0] == '/' ? "" : "/") + (p ? p : ""); }

namespace fs {
struct HostFileImpl {
  std::string path, name;
  FILE *f = nullptr;
  DIR *d = nullptr;
  bool isDir = false;
  ~HostFileImpl() { if (f) fclose(f); if (d) closedir(d); }
};
}  // namespace fs

fs::LittleFSFS LittleFS;

bool fs::LittleFSFS::begin(bool, const char *, uint8_t, const char *) {
  ::mkdir(sRoot.c_str(), 0755);
  return true;
}
bool fs::LittleFSFS::format() { return true; }
size_t fs::LittleFSFS::totalBytes() { return 1441792; }   // huge_app.csv spiffs partition (1.375 MB)
size_t fs::LittleFSFS::usedBytes() { return 16384; }

fs::File fs::FS::open(const char *path, const char *mode, const bool) {
  std::string p = hp(path);
  struct stat st;
  auto impl = std::make_shared<HostFileImpl>();
  impl->path = path ? path : "/";
  const char *slash = strrchr(impl->path.c_str(), '/');
  impl->name = slash ? slash + 1 : impl->path;
  if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    impl->isDir = true;
    impl->d = opendir(p.c_str());
    return File(impl);
  }
  const char *m = !strcmp(mode, "w") ? "wb" : !strcmp(mode, "a") ? "ab" : !strcmp(mode, "r+") ? "r+b" : "rb";
  if (m[0] != 'r') {          // create parent dirs like LittleFS does on write
    std::string dir = p.substr(0, p.rfind('/'));
    ::mkdir(dir.c_str(), 0755);
  }
  impl->f = fopen(p.c_str(), m);
  if (!impl->f) return File();
  return File(impl);
}
bool fs::FS::exists(const char *path) { struct stat st; return stat(hp(path).c_str(), &st) == 0; }
bool fs::FS::remove(const char *path) { return ::unlink(hp(path).c_str()) == 0; }
bool fs::FS::rename(const char *a, const char *b) { return ::rename(hp(a).c_str(), hp(b).c_str()) == 0; }
bool fs::FS::mkdir(const char *path) { return ::mkdir(hp(path).c_str(), 0755) == 0 || exists(path); }
bool fs::FS::rmdir(const char *path) { return ::rmdir(hp(path).c_str()) == 0; }

size_t fs::File::write(uint8_t c) { return write(&c, 1); }
size_t fs::File::write(const uint8_t *buf, size_t size) { return (_p && _p->f) ? fwrite(buf, 1, size, _p->f) : 0; }
int fs::File::available() {
  if (!_p || !_p->f) return 0;
  long pos = ftell(_p->f); fseek(_p->f, 0, SEEK_END); long end = ftell(_p->f); fseek(_p->f, pos, SEEK_SET);
  return (int)(end - pos);
}
int fs::File::read() { return (_p && _p->f) ? fgetc(_p->f) : -1; }
int fs::File::peek() { if (!_p || !_p->f) return -1; int c = fgetc(_p->f); if (c >= 0) ungetc(c, _p->f); return c; }
void fs::File::flush() { if (_p && _p->f) fflush(_p->f); }
size_t fs::File::read(uint8_t *buf, size_t size) { return (_p && _p->f) ? fread(buf, 1, size, _p->f) : 0; }
bool fs::File::seek(uint32_t pos, SeekMode mode) {
  return _p && _p->f && fseek(_p->f, pos, mode == SeekSet ? SEEK_SET : mode == SeekCur ? SEEK_CUR : SEEK_END) == 0;
}
size_t fs::File::position() const { return (_p && _p->f) ? (size_t)ftell(_p->f) : 0; }
size_t fs::File::size() const {
  if (!_p || !_p->f) return 0;
  long pos = ftell(_p->f); fseek(_p->f, 0, SEEK_END); long end = ftell(_p->f); fseek(_p->f, pos, SEEK_SET);
  return (size_t)end;
}
void fs::File::close() { _p.reset(); }
fs::File::operator bool() const { return _p && (_p->f || _p->d); }
const char *fs::File::path() const { return _p ? _p->path.c_str() : ""; }
const char *fs::File::name() const { return _p ? _p->name.c_str() : ""; }
boolean fs::File::isDirectory(void) { return _p && _p->isDir; }
fs::File fs::File::openNextFile(const char *mode) {
  if (!_p || !_p->d) return File();
  struct dirent *e;
  while ((e = readdir(_p->d))) {
    if (e->d_name[0] == '.') continue;
    std::string child = _p->path + (_p->path.size() && _p->path.back() == '/' ? "" : "/") + e->d_name;
    return LittleFS.open(child.c_str(), mode);
  }
  return File();
}
String fs::File::getNextFileName(void) { File f = openNextFile(); return f ? String(f.path()) : String(""); }
String fs::File::getNextFileName(bool *isDir) { File f = openNextFile(); if (isDir) *isDir = f && f.isDirectory(); return f ? String(f.path()) : String(""); }
void fs::File::rewindDirectory(void) { if (_p && _p->d) rewinddir(_p->d); }
