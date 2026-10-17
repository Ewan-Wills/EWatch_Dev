// bookreader — implementation. See bookreader.h for the overview.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "bookreader.h"
#include "ewlog.h"     // ewlogConsolePause
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "haptic_roles.h"
#include "model.h"     // model + ModelLock
#include "event.h"
#include "system/ui_style.h"
#include "system/ui_icons.h"

#include "FreeSans12pt7b.h"
#include "FreeSerifBold12pt7b.h"
#include "FreeSerifBold24pt7b.h"

// ===========================================================================
// Layout constants
// ===========================================================================
static const int16_t W = 240;
static const int16_t H = 280;
static const int16_t MARGIN   = 8;              // left/right text inset
static const int16_t TEXT_W   = W - 2 * MARGIN; // usable line width (224)
static const int16_t PROG_H   = 4;              // bottom progress bar height
static const int16_t TEXT_H   = H - PROG_H;     // text area height

static const char *BOOK_DIR = "/books";

// Idle timeouts (ms): return to a lower screen so the system can auto-sleep,
// since the reader/library block the normal 5 s auto-sleep while open.
static const uint32_t READER_IDLE_MS  = 60000;
static const uint32_t LIBRARY_IDLE_MS = 30000;

// ===========================================================================
// Reader fonts (the "font size" setting steps through these)
// ===========================================================================
struct RFont { const GFXfont *font; const char *label; };
static const RFont kReaderFonts[] = {
  { &FreeSans12pt7b,      "S" },   // light sans, most text per screen
  { &FreeSerifBold12pt7b, "M" },   // serif body
  { &FreeSerifBold24pt7b, "L" },   // large serif
};
static const int kReaderFontCount = sizeof(kReaderFonts) / sizeof(kReaderFonts[0]);

// Auto-scroll speeds in pixels/second.
static const int kSpeeds[] = { 8, 14, 22, 34, 50 };
static const int kSpeedCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);

// ===========================================================================
// Persistent prefs (own NVS namespace — does not touch the system schema)
// ===========================================================================
static Preferences gPrefs;
static bool        gPrefsOpen = false;
static void prefsBegin() {
  if (!gPrefsOpen) { gPrefs.begin("bookrdr", /*readOnly=*/false); gPrefsOpen = true; }
}
static uint8_t gFontIdx  = 0;
static uint8_t gSpeedIdx = 1;

static uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}
static void posKey(const char *path, char *out) {   // out >= 10 bytes
  snprintf(out, 10, "p%08lx", (unsigned long)fnv1a(path));
}
static uint32_t loadPos(const char *path) {
  prefsBegin(); char k[10]; posKey(path, k);
  return gPrefs.getUInt(k, 0);
}
static void savePos(const char *path, uint32_t byteOff) {
  prefsBegin(); char k[10]; posKey(path, k);
  gPrefs.putUInt(k, byteOff);
}

// ===========================================================================
// Filesystem (LittleFS on the "spiffs" partition) + a mutex guarding every
// FS access, because the upload task and the render task both touch it.
// ===========================================================================
static SemaphoreHandle_t gFsMutex = nullptr;
static bool              gFsOk    = false;
struct FsLock {
  bool held = false;
  FsLock()  { if (gFsMutex) { xSemaphoreTake(gFsMutex, portMAX_DELAY); held = true; } }
  ~FsLock() { if (held) xSemaphoreGive(gFsMutex); }
};
static void fsInit() {
  if (!gFsMutex) gFsMutex = xSemaphoreCreateMutex();
  FsLock lk;
  if (gFsOk) return;
  gFsOk = LittleFS.begin(/*formatOnFail=*/true);
  if (gFsOk) {
    if (!LittleFS.exists(BOOK_DIR)) LittleFS.mkdir(BOOK_DIR);
  }
}

// ===========================================================================
// Line index — array of per-line start byte offsets, held in PSRAM. Lets us
// page a huge book from flash one visible line at a time.
// ===========================================================================
static uint32_t *gLineStart = nullptr;   // PSRAM buffer
static uint32_t  gLineCap   = 0;
static uint32_t  gLineCount = 0;
static const uint32_t kMaxLines = 300000; // safety cap (~1.2 MB index)

static File     gFile;          // the open book (index + render both use it)
static char     gOpenPath[64] = "";
static uint32_t gFileSize = 0;
static int16_t  gLineH = 29, gAscent = 18, gSpaceW = 6; // for current font
static const GFXfont *gFont = nullptr;

static void linesFree() {
  if (gLineStart) { heap_caps_free(gLineStart); gLineStart = nullptr; }
  gLineCap = gLineCount = 0;
}
static bool linePush(uint32_t off) {
  if (gLineCount >= kMaxLines) return false;
  if (gLineCount >= gLineCap) {
    uint32_t want = gLineCap ? gLineCap * 2 : 2048;
    void *p = heap_caps_realloc(gLineStart, want * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_realloc(gLineStart, want * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (!p) return false;
    gLineStart = (uint32_t *)p; gLineCap = want;
  }
  gLineStart[gLineCount++] = off;
  return true;
}

static int glyphAdv(const GFXfont *f, uint8_t c) {
  if (c == '\t') c = ' ';
  if (c < f->first || c > f->last) {
    if (' ' >= f->first && ' ' <= f->last) c = ' ';
    else return f->yAdvance / 2;
  }
  return f->glyph[c - f->first].xAdvance;
}

// Word buffer for the wrap builder (hard-breaks words longer than a line).
static const int   kWordMax = 1024;
static uint32_t    gWordOff[kWordMax];
static int16_t     gWordAdv[kWordMax];

// Build the line index for the currently-open gFile using font `f`.
// Greedy word wrap to TEXT_W; records the byte offset of every line start.
static bool buildIndex(const GFXfont *f) {
  gFont   = f;
  gLineH  = f->yAdvance;
  gSpaceW = glyphAdv(f, ' ');
  gLineCount = 0;
  if (!linePush(0)) return false;

  gFile.seek(0);
  int16_t lineW = 0;
  bool    lineHasContent = false;
  int     wordLen = 0; int16_t wordW = 0;

  static uint8_t buf[1024];
  uint32_t pos = 0;
  uint32_t fedCounter = 0;

  // flush the pending word onto the current line (wrapping / hard-break).
  auto flushWord = [&]() {
    if (wordLen == 0) return true;
    int16_t spaceW = lineHasContent ? gSpaceW : 0;
    if (lineHasContent && lineW + spaceW + wordW > TEXT_W) {
      if (!linePush(gWordOff[0])) return false;
      lineW = 0; lineHasContent = false; spaceW = 0;
    } else {
      lineW += spaceW;
    }
    for (int k = 0; k < wordLen; k++) {
      if (lineHasContent && lineW + gWordAdv[k] > TEXT_W) {
        if (!linePush(gWordOff[k])) return false;
        lineW = 0; lineHasContent = false;
      }
      lineW += gWordAdv[k]; lineHasContent = true;
    }
    wordLen = 0; wordW = 0;
    return true;
  };

  while (pos < gFileSize) {
    int n = gFile.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      uint8_t c = buf[i];
      uint32_t off = pos + i;
      if (c == '\r') continue;
      if (c == '\n') {
        if (!flushWord()) return false;
        if (!linePush(off + 1)) return false;   // next line starts after NL
        lineW = 0; lineHasContent = false;
      } else if (c == ' ' || c == '\t') {
        if (!flushWord()) return false;
      } else {
        if (wordLen < kWordMax) {
          gWordOff[wordLen] = off;
          gWordAdv[wordLen] = glyphAdv(f, c);
          wordW += gWordAdv[wordLen];
          wordLen++;
        } else {
          // Pathologically long token: flush what we have and keep going.
          if (!flushWord()) return false;
          gWordOff[0] = off; gWordAdv[0] = glyphAdv(f, c); wordW = gWordAdv[0]; wordLen = 1;
        }
      }
    }
    pos += n;
    if ((++fedCounter & 0x7) == 0) esp_task_wdt_reset();  // keep WDT fed
  }
  if (!flushWord()) return false;
  return true;
}

// Read a single display line's text into `out` (whitespace collapsed+trimmed).
static void lineText(uint32_t i, char *out, size_t cap) {
  out[0] = '\0';
  if (i >= gLineCount || cap == 0) return;
  uint32_t start = gLineStart[i];
  uint32_t end   = (i + 1 < gLineCount) ? gLineStart[i + 1] : gFileSize;
  if (end <= start) return;
  uint32_t len = end - start;
  if (len > 300) len = 300;             // a line never needs more than this
  static uint8_t raw[301];
  gFile.seek(start);
  int n = gFile.read(raw, len);
  if (n <= 0) return;
  size_t o = 0; bool prevSpace = false; bool started = false;
  for (int k = 0; k < n && o + 1 < cap; k++) {
    uint8_t c = raw[k];
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c == ' ') {
      if (!started) continue;           // trim leading
      prevSpace = true; continue;
    }
    if (prevSpace && started) { out[o++] = ' '; if (o + 1 >= cap) break; }
    prevSpace = false; started = true;
    out[o++] = (char)c;
  }
  out[o] = '\0';
}

static uint32_t maxScrollPx() {
  int64_t total = (int64_t)gLineCount * gLineH;
  int64_t m = total - TEXT_H;
  return (m > 0) ? (uint32_t)m : 0;
}

// A "page" is one screenful of whole lines; "page.line" in the controls is
// page number and line within it (both 1-based) of the top line.
static uint32_t linesPerPage() {
  int32_t n = TEXT_H / gLineH;
  return n > 0 ? (uint32_t)n : 1;
}
static uint32_t pageCount() {
  uint32_t lpp = linesPerPage();
  return gLineCount ? (gLineCount + lpp - 1) / lpp : 1;
}

// Find the line whose start byte offset is the greatest <= byteOff.
static uint32_t lineForByte(uint32_t byteOff) {
  if (gLineCount == 0) return 0;
  uint32_t lo = 0, hi = gLineCount - 1, ans = 0;
  while (lo <= hi) {
    uint32_t mid = (lo + hi) / 2;
    if (gLineStart[mid] <= byteOff) { ans = mid; if (mid == gLineCount - 1) break; lo = mid + 1; }
    else { if (mid == 0) break; hi = mid - 1; }
  }
  return ans;
}

// Open a book file and index it with the current global font. Returns false
// on any failure (file missing, empty, OOM).
// Bumped only when the book set changes (upload finished / file deleted). The
// library used to rescan on any model.revision change, but the list itself
// bumps revision while animating, so a rescan landed between press and
// release (setEntries clears the pressed row) and every tap was cancelled.
static volatile uint32_t gBooksGen = 0;

static bool bookOpen(const char *path) {
  fsInit();
  FsLock lk;
  if (!gFsOk) return false;
  if (gFile) gFile.close();
  gFile = LittleFS.open(path, "r");
  if (!gFile) return false;
  gFileSize = gFile.size();
  strncpy(gOpenPath, path, sizeof(gOpenPath) - 1); gOpenPath[sizeof(gOpenPath) - 1] = '\0';
  const GFXfont *f = kReaderFonts[gFontIdx < kReaderFontCount ? gFontIdx : 0].font;
  bool ok = buildIndex(f);
  if (!ok) { gFile.close(); linesFree(); gOpenPath[0] = '\0'; }
  return ok;
}
static void bookClose() {
  FsLock lk;
  if (gFile) gFile.close();
  linesFree();
  gOpenPath[0] = '\0';
}
// Re-index the already-open book (e.g. after a font change). The caller
// re-derives the top line from the kept byte offset afterwards.
static bool bookReindex() {
  FsLock lk;
  if (!gFile) return false;
  const GFXfont *f = kReaderFonts[gFontIdx < kReaderFontCount ? gFontIdx : 0].font;
  return buildIndex(f);
}

// ===========================================================================
// Book directory listing (names only, read under the FS lock)
// ===========================================================================
static const int kMaxBooks = 64;
struct BookEntry { char name[64]; uint32_t size; };
static BookEntry gBooks[kMaxBooks];
static int       gBookN = 0;

static void listBooks() {
  fsInit();
  FsLock lk;
  gBookN = 0;
  if (!gFsOk) return;
  File dir = LittleFS.open(BOOK_DIR);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f && gBookN < kMaxBooks; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const char *n = f.name();
      const char *slash = strrchr(n, '/');
      if (slash) n = slash + 1;
      if (n[0] != '.') {                 // skip the .incoming temp
        strncpy(gBooks[gBookN].name, n, sizeof(gBooks[gBookN].name) - 1);
        gBooks[gBookN].name[sizeof(gBooks[gBookN].name) - 1] = '\0';
        gBooks[gBookN].size = f.size();
        gBookN++;
      }
    }
    f.close();
  }
  dir.close();
}

// ===========================================================================
// Serial upload listener
//   Frame (PC -> watch), little-endian:
//     magic "EWBK" | cmd | ...
//   cmd 'U' upload : u16 nameLen | u32 dataLen | name | data | u32 crc32(data)
//   cmd 'L' list   : (no args) -> ">>BOOK LIST n" then ">>BOOK FILE name size"
//   cmd 'D' delete : u16 nameLen | name          -> ">>BOOK DEL name"
//   Replies are newline ASCII lines prefixed ">>BOOK ".
// ===========================================================================
static volatile bool gUploadBusy = false;
static volatile bool gLibraryVisible = false;  // console stays paused while true or uploading
static void uploadDone() {
  gUploadBusy = false;
  if (!gLibraryVisible) ewlogConsolePause(false);  // Library left mid-upload
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// Blocking read of exactly n bytes with a timeout. Returns bytes read.
static size_t serialReadN(uint8_t *dst, size_t n, uint32_t toMs) {
  size_t got = 0; uint32_t t0 = millis();
  while (got < n) {
    int a = Serial.available();
    if (a > 0) {
      int r = Serial.read(dst + got, n - got);
      if (r > 0) { got += r; t0 = millis(); continue; }
    }
    if (millis() - t0 > toMs) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return got;
}
static uint16_t rdU16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rdU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sanitizeName(const char *in, char *out, size_t cap) {
  // keep basename, allow a safe charset, force a .txt suffix
  const char *slash = strrchr(in, '/'); if (slash) in = slash + 1;
  size_t o = 0;
  for (; *in && o + 1 < cap; ++in) {
    char c = *in;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ';
    if (ok) out[o++] = c;
  }
  out[o] = '\0';
  if (o == 0) { strncpy(out, "book.txt", cap); return; }
  size_t L = strlen(out);
  if (L < 4 || strcasecmp(out + L - 4, ".txt") != 0) {
    if (L + 4 < cap) strcat(out, ".txt");
  }
}

static void doUpload() {
  uint8_t hdr[6];
  if (serialReadN(hdr, 6, 2000) != 6) { Serial.println(">>BOOK ERR hdr"); return; }
  uint16_t nameLen = rdU16(hdr);
  uint32_t dataLen = rdU32(hdr + 2);
  if (nameLen == 0 || nameLen > 200 || dataLen > 2u * 1024u * 1024u) {
    Serial.println(">>BOOK ERR size"); return;
  }
  char rawName[208];
  if (serialReadN((uint8_t *)rawName, nameLen, 2000) != nameLen) { Serial.println(">>BOOK ERR name"); return; }
  rawName[nameLen] = '\0';
  char name[64]; sanitizeName(rawName, name, sizeof(name));
  char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
  char tmp[96];  snprintf(tmp,  sizeof(tmp),  "%s/.incoming", BOOK_DIR);

  // Flow control: the USB-Serial/JTAG RX queue silently drops bytes when
  // full, so the PC sends one block, then waits for ">>BOOK ACK <total>"
  // before the next. RECV means "file open, send the first block".
  static const size_t kBlock = 2048;   // must match upload.py; < RX queue
  fsInit();
  gUploadBusy = true;
  ewlogConsolePause(true);   // even if the Library is left mid-upload
  bool wrote = false; uint32_t crc = 0; uint32_t remaining = dataLen;
  // The FS lock is taken per block, never across the transfer: the Library
  // (render task) rescans under the same lock, and a render task blocked for
  // the whole upload trips the task WDT and reboots the watch.
  File out;
  const char *err = nullptr;
  {
    FsLock lk;
    size_t freeB = gFsOk ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
    if (!gFsOk) err = "fs";
    else if (dataLen + 8192 > freeB) err = "space";  // LittleFS needs a little slack
    else if (!(out = LittleFS.open(tmp, "w"))) err = "open";
    if (err) Serial.printf(">>BOOK ERR %s need=%lu free=%lu\n", err, (unsigned long)dataLen, (unsigned long)freeB);
  }
  if (err) { uploadDone(); return; }
  Serial.printf(">>BOOK RECV %s %lu\n", name, (unsigned long)dataLen);
  Serial.flush();
  static uint8_t chunk[kBlock];
  const char *why = nullptr;
  wrote = true;
  while (remaining > 0) {
    size_t want = remaining > kBlock ? kBlock : remaining;
    size_t got = serialReadN(chunk, want, 5000);
    if (got != want) { wrote = false; why = "read"; break; }
    crc = crc32_update(crc, chunk, got);
    size_t put;
    { FsLock lk; put = out.write(chunk, got); }
    if (put != got) { wrote = false; why = "flash"; break; }
    remaining -= got;
    Serial.printf(">>BOOK ACK %lu\n", (unsigned long)(dataLen - remaining));
    Serial.flush();   // HWCDC holds short writes in its TX buffer; the PC waits on this
    esp_task_wdt_reset();
  }
  { FsLock lk; out.close(); }
  if (why) {
    FsLock lk;
    Serial.printf(">>BOOK ERR %s at=%lu used=%lu total=%lu\n", why, (unsigned long)(dataLen - remaining),
                  (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
    LittleFS.remove(tmp);
    uploadDone();
    return;
  }
  // CRC trailer (read outside the lock; small)
  uint8_t crcb[4]; uint32_t wantCrc = 0; bool haveCrc = (serialReadN(crcb, 4, 2000) == 4);
  if (haveCrc) wantCrc = rdU32(crcb);

  bool ok = wrote && remaining == 0 && (!haveCrc || wantCrc == crc);
  {
    FsLock lk;
    if (gFsOk) {
      if (ok) {
        LittleFS.remove(path);
        if (!LittleFS.rename(tmp, path)) ok = false;
      } else {
        LittleFS.remove(tmp);
      }
    }
  }
  uploadDone();
  if (ok) {
    Serial.printf(">>BOOK OK %s %lu\n", name, (unsigned long)dataLen);
    Serial.flush();
    gBooksGen++;
    { ModelLock mlk; model.revision++; }     // nudge the library to refresh
  } else {
    Serial.printf(">>BOOK ERR %s\n", !wrote ? "write" : remaining ? "short" : "crc");
    Serial.flush();
  }
}

static void doList() {
  listBooks();
  Serial.printf(">>BOOK LIST %d\n", gBookN);
  {
    FsLock lk;
    if (gFsOk) {
      Serial.printf(">>BOOK FS used=%lu total=%lu\n", (unsigned long)LittleFS.usedBytes(),
                    (unsigned long)LittleFS.totalBytes());
      // Everything else on the partition (logs, photos, leftovers) for diagnosis.
      File root = LittleFS.open("/");
      for (File f = root.openNextFile(); f; f = root.openNextFile())
        Serial.printf(">>BOOK FSFILE /%s %lu%s\n", f.name(), (unsigned long)f.size(), f.isDirectory() ? " dir" : "");
    }
  }
  for (int i = 0; i < gBookN; i++)
    Serial.printf(">>BOOK FILE %s %lu\n", gBooks[i].name, (unsigned long)gBooks[i].size);
}

static void doDelete() {
  uint8_t h[2];
  if (serialReadN(h, 2, 2000) != 2) { Serial.println(">>BOOK ERR hdr"); return; }
  uint16_t nameLen = rdU16(h);
  if (nameLen == 0 || nameLen > 200) { Serial.println(">>BOOK ERR name"); return; }
  char rawName[208];
  if (serialReadN((uint8_t *)rawName, nameLen, 2000) != nameLen) { Serial.println(">>BOOK ERR name"); return; }
  rawName[nameLen] = '\0';
  char name[64]; sanitizeName(rawName, name, sizeof(name));
  char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, name);
  fsInit();
  bool ok; { FsLock lk; ok = gFsOk && LittleFS.remove(path); }
  Serial.printf(">>BOOK %s %s\n", ok ? "DEL" : "ERR", name);
  if (ok) { gBooksGen++; ModelLock mlk; model.revision++; }
}

static void bookUploadTask(void *) {
  esp_task_wdt_add(nullptr);
  Serial.println(">>BOOK READY");
  uint8_t win[4] = { 0, 0, 0, 0 };
  for (;;) {
    esp_task_wdt_reset();
    // Only read while the Library owns the port; otherwise leave the bytes
    // for the ewlog console (both reading Serial corrupts uploads).
    if (!ewlogConsolePaused()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    int c = Serial.read();
    if (c < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    win[0] = win[1]; win[1] = win[2]; win[2] = win[3]; win[3] = (uint8_t)c;
    if (win[0] == 'E' && win[1] == 'W' && win[2] == 'B' && win[3] == 'K') {
      // next byte is the command
      uint8_t cmd;
      if (serialReadN(&cmd, 1, 1000) != 1) continue;
      if      (cmd == 'U') doUpload();
      else if (cmd == 'L') doList();
      else if (cmd == 'D') doDelete();
      win[0] = win[1] = win[2] = win[3] = 0;   // reset the magic window
    }
  }
}

void bookUploadStart() {
  static bool started = false;
  if (started) return;
  started = true;
  fsInit();
  prefsBegin();
  gFontIdx  = gPrefs.getUChar("font",  0);  if (gFontIdx  >= kReaderFontCount) gFontIdx  = 0;
  gSpeedIdx = gPrefs.getUChar("speed", 1);  if (gSpeedIdx >= kSpeedCount)      gSpeedIdx = 1;
  xTaskCreatePinnedToCore(bookUploadTask, "bookup", 4096, nullptr, 3, nullptr, 0);
}

// ===========================================================================
// LibraryView (an AuraListView, so it looks and scrolls like the app list)
// ===========================================================================
static AuraEntry gEntries[AuraListView::kMaxRows];
static char      gLabels[AuraListView::kMaxRows][28];
static char      gSubs[AuraListView::kMaxRows][24];

void LibraryView::rescan() {
  listBooks();
  int n = gBookN < AuraListView::kMaxRows ? gBookN : AuraListView::kMaxRows;
  for (int i = 0; i < n; i++) {
    strncpy(gLabels[i], gBooks[i].name, sizeof(gLabels[i]) - 1);
    gLabels[i][sizeof(gLabels[i]) - 1] = '\0';
    size_t L = strlen(gLabels[i]);
    if (L > 4 && strcasecmp(gLabels[i] + L - 4, ".txt") == 0) gLabels[i][L - 4] = '\0';
    if (strlen(gLabels[i]) > 18) strcpy(gLabels[i] + 16, "..");
    snprintf(gSubs[i], sizeof(gSubs[i]), "%lu KB", (unsigned long)((gBooks[i].size + 1023) / 1024));
    gEntries[i] = { gLabels[i], Icon::Book, aura::kOrange, Screen::BookReader, nullptr, gSubs[i] };
  }
  setEntries(gEntries, n);
}

void LibraryView::onEnter() {
  gLibraryVisible = true;
  ewlogConsolePause(true);   // the uploader owns USB serial while the Library is up
  bookUploadStart();
  delRow = -1;
  lastInteract = millis();
  lastSeenUploads = gBooksGen;
  rescan();
  AuraListView::onEnter();
}

void LibraryView::onExit() {
  gLibraryVisible = false;
  if (!gUploadBusy) ewlogConsolePause(false);
}

void LibraryView::render() {
  uint32_t rev = gBooksGen;
  // No rescan mid-upload: the list can't change until the upload finishes
  // (which bumps the revision again), and it would contend for the FS lock.
  if (rev != lastSeenUploads && !gUploadBusy && delRow < 0) { rescan(); lastSeenUploads = rev; }
  // idle -> back to the watch so the system can auto-sleep (not mid-upload).
  if (!gUploadBusy && delRow < 0 && millis() - lastInteract > LIBRARY_IDLE_MS) {
    switchTo(Screen::Watch);
    return;
  }
  AuraListView::render();
}

void LibraryView::onEvent(const Event &e) {
  lastInteract = millis();
  AuraListView::onEvent(e);
}

void LibraryView::activate(int row) {
  if (row < 0 || row >= gBookN) return;
  char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, gBooks[row].name);
  if (bookOpen(path)) switchToAnimated(Screen::BookReader, Trans::PushLeft);
  else haptic(HapticRole::Failed);   // unreadable / out of memory: stay on the list
}

void LibraryView::longPress(int row) {
  if (row < 0 || row >= gBookN) return;
  delRow = row;
  haptic(HapticRole::LongPress);
}

// Delete dialog geometry (Cancel / Delete buttons).
static const int16_t DLG_X = 20, DLG_Y = 84, DLG_W = W - 40, DLG_H = 122;
static const int16_t BTN_Y = DLG_Y + 70, BTN_H = 38, BTN_W = 86;
static const int16_t CANCEL_X = DLG_X + 12, DEL_X = DLG_X + DLG_W - 12 - BTN_W;

void LibraryView::drawOverlay(Arduino_Canvas *cv) {
  AuraTheme th = auraTheme();
  if (rowCount() == 0) {
    auraTextCentered(cv, W / 2, 120, "No books yet", 2, th.text);
    auraTextCentered(cv, W / 2, 150, "Open this screen, then on the PC:", 1, th.textDim);
    auraTextCentered(cv, W / 2, 166, "pc/upload.py book.txt", 1, th.textDim);
  }
  if (delRow < 0) return;
  auraCard(cv, DLG_X, DLG_Y, DLG_W, DLG_H, aura::kRadius, th.cardHi);
  auraTextCentered(cv, W / 2, DLG_Y + 16, "Delete this book?", 2, th.text);
  if (delRow < rowCount()) auraTextCentered(cv, W / 2, DLG_Y + 44, gLabels[delRow], 1, th.textDim);
  auraCard(cv, CANCEL_X, BTN_Y, BTN_W, BTN_H, 12, th.card);
  auraTextCentered(cv, CANCEL_X + BTN_W / 2, BTN_Y + 15, "Cancel", 1, th.text);
  auraCard(cv, DEL_X, BTN_Y, BTN_W, BTN_H, 12, aura::kRed);
  auraTextCentered(cv, DEL_X + BTN_W / 2, BTN_Y + 15, "Delete", 1, aura::kText);
}

bool LibraryView::interceptEvent(const Event &e) {
  // Every event passes here, including the ones AuraListView's render loop
  // drains itself (they never reach onEvent), so this is the idle clock.
  lastInteract = millis();
  if (delRow < 0) return false;
  // Modal: act on the finger lifting inside a button, so a stray swipe or the
  // tail of the long-press can't confirm a delete.
  if (e.type == EventType::TouchUp) {
    bool inBtns = e.y >= BTN_Y && e.y < BTN_Y + BTN_H;
    if (inBtns && e.x >= DEL_X && e.x < DEL_X + BTN_W && delRow < gBookN) {
      char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, gBooks[delRow].name);
      { FsLock lk; if (gFsOk) LittleFS.remove(path); }
      haptic(HapticRole::Destructive);
      delRow = -1;
      rescan();
    } else if (inBtns && e.x >= CANCEL_X && e.x < CANCEL_X + BTN_W) {
      delRow = -1;
    }
  } else if (e.type == EventType::ButtonShort) {
    delRow = -1;
  }
  return true;   // swallow everything while the dialog is up
}

// ===========================================================================
// ReaderView
// ===========================================================================
// Control panel layout (y relative to the panel top). One table drives both
// drawing and hit-testing so they can't drift apart.
enum PanelBtnId : uint8_t {
  BTN_PG_M10, BTN_PG_M1, BTN_PG_P1, BTN_PG_P10, BTN_LN_M1, BTN_LN_P1,
  BTN_FONT_M, BTN_FONT, BTN_FONT_P, BTN_SPEED_M, BTN_SPEED, BTN_SPEED_P,
  BTN_PLAY, BTN_LIBRARY,
};
// Full-screen menu (slides in from the right). Absolute screen coordinates;
// big targets. BTN_FONT / BTN_SPEED are value displays, not buttons.
struct PanelBtn { int16_t x, y, w, h; PanelBtnId id; const char *label; };
static const PanelBtn kPanelBtns[] = {
  {   8,  40,  52, 48, BTN_PG_M10, "-10" }, {  66,  40,  52, 48, BTN_PG_M1,  "-1"  },
  { 124,  40,  52, 48, BTN_PG_P1,  "+1"  }, { 182,  40,  52, 48, BTN_PG_P10, "+10" },
  {   8,  94, 110, 42, BTN_LN_M1,  "Line -" }, { 124,  94, 110, 42, BTN_LN_P1, "Line +" },
  {  76, 142,  48, 42, BTN_FONT_M,  "-" }, { 130, 142,  50, 42, BTN_FONT,  "" },
  { 186, 142,  48, 42, BTN_FONT_P,  "+" },
  {  76, 190,  48, 42, BTN_SPEED_M, "-" }, { 130, 190,  50, 42, BTN_SPEED, "" },
  { 186, 190,  48, 42, BTN_SPEED_P, "+" },
  {   8, 238, 148, 38, BTN_PLAY,    "" }, { 162, 238,  72, 38, BTN_LIBRARY, "Books" },
};
static bool isValueBox(PanelBtnId id) { return id == BTN_FONT || id == BTN_SPEED; }

void ReaderView::onEnter() {
  controls = false; playing = false; resumeAfter = false;
  menuPos = menuTarget = 0.f; pendingTapMs = 0;
  lastFrameMs = millis(); lastInteract = millis(); lastSaveMs = millis();
  // restore saved position (by byte offset, font-independent)
  uint32_t byteOff = loadPos(gOpenPath);
  lastSavedOff = byteOff;
  uint32_t line = lineForByte(byteOff);
  scrollPx = scrollGoal = (float)line * gLineH;
  float mx = (float)maxScrollPx();
  if (scrollGoal > mx) scrollGoal = scrollPx = mx;
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
}

void ReaderView::onExit() { saveProgress(); bookClose(); }

uint32_t ReaderView::topLine() const {
  // The scroll *goal*, so leaving mid-animation saves where you were going.
  float g = scrollGoal < 0 ? 0 : scrollGoal;
  uint32_t t = (uint32_t)(g / gLineH + 0.5f);
  if (gLineCount && t >= gLineCount) t = gLineCount - 1;
  return t;
}

void ReaderView::jumpToLine(int32_t line) {
  if (line < 0) line = 0;
  if (gLineCount && line >= (int32_t)gLineCount) line = gLineCount - 1;
  float px = (float)line * gLineH;
  float mx = (float)maxScrollPx();
  if (px > mx) px = mx;
  scrollPx = scrollGoal = px;   // instant: a 10-page jump shouldn't animate
  playing = false;
}

// Saved as the top line's byte offset (font-independent), only when it
// changed, so the 3 s autosave doesn't wear NVS while you sit on a page.
void ReaderView::saveProgress() {
  if (gOpenPath[0] == '\0' || gLineCount == 0) return;
  uint32_t off = gLineStart[topLine()];
  if (off == lastSavedOff) return;
  savePos(gOpenPath, off);
  lastSavedOff = off;
}

void ReaderView::drawFrame() {
  Arduino_Canvas *cv = frameCanvas();
  ThemeColors t = theme();
  Arduino_GFX *d = cv ? (Arduino_GFX *)cv : gfx;
  if (!d) return;

  d->fillScreen(t.bg);

  const GFXfont *f = kReaderFonts[gFontIdx < kReaderFontCount ? gFontIdx : 0].font;
  d->setFont(f); d->setTextSize(1); d->setTextColor(t.fg);
  // measure ascent for this font once per frame (cheap)
  int16_t bx, by; uint16_t bw, bh;
  d->getTextBounds("Mg", 0, 0, &bx, &by, &bw, &bh);
  int16_t ascent = -by; if (ascent <= 0) ascent = gLineH * 3 / 4;

  float topPx = scrollPx;
  int   firstLine = (int)(topPx / gLineH);
  int   subPx     = (int)(topPx - (float)firstLine * gLineH);
  if (firstLine < 0) { firstLine = 0; subPx = 0; }

  // Arduino_GFX's custom-font drawChar clips only right/bottom: a glyph with
  // any pixel above y=0 is written *before* the canvas buffer and corrupts the
  // heap (crashed in the GPIO ISR table). So never draw a line whose tallest
  // glyph would cross the top edge; it just vanishes as it scrolls off.
  int16_t minYo = 0;
  for (uint16_t g = 0; g <= f->last - f->first; g++)
    if (f->glyph[g].yOffset < minYo) minYo = f->glyph[g].yOffset;

  char buf[160];
  for (int slot = 0; menuPos < 0.999f; slot++) {   // menu fully open: text hidden
    int li = firstLine + slot;
    if (li >= (int)gLineCount) break;
    int16_t slotTop = slot * gLineH - subPx;
    if (slotTop >= TEXT_H) break;
    if (slotTop + ascent + minYo < 0) continue;
    {
      FsLock lk;
      lineText((uint32_t)li, buf, sizeof(buf));
    }
    if (buf[0]) {
      d->setCursor(MARGIN, slotTop + ascent);
      d->print(buf);
    }
  }

  // progress bar along the bottom edge
  d->setFont(nullptr);
  uint32_t mx = maxScrollPx();
  float frac = mx > 0 ? (scrollPx / (float)mx) : 1.f;
  if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  int16_t pw = (int16_t)(frac * (W - 2 * MARGIN));
  d->fillRect(0, H - PROG_H, W, PROG_H, t.bg);
  d->fillRect(MARGIN, H - PROG_H, W - 2 * MARGIN, 2, t.line);
  d->fillRect(MARGIN, H - PROG_H, pw, 2, t.accent);

  // top-left back chevron (always available)
  d->setFont(nullptr);
  d->fillRoundRect(2, 2, 40, 30, 6, t.accent);
  d->setTextColor(contrastFor(t.accent), t.accent); d->setTextSize(2);
  d->setCursor(14, 10); d->print('<');

  // Menu: full screen, slid in from the right by menuPos (0 hidden, 1 open).
  if (menuPos > 0.001f) {
    const int16_t ox = (int16_t)((1.f - menuPos) * W);
    d->fillRect(ox, 0, W - ox, H, t.bg);
    d->drawFastVLine(ox, 0, H, t.accent);
    uint32_t tl = topLine(), lpp = linesPerPage();
    d->setFont(nullptr);
    d->setTextColor(t.fg, t.bg); d->setTextSize(2);
    d->setCursor(ox + 10, 12);
    d->printf("Pg %lu.%lu", (unsigned long)(tl / lpp + 1), (unsigned long)(tl % lpp + 1));
    d->setTextSize(1); d->setTextColor(t.line, t.bg);
    d->setCursor(ox + W - 70, 10); d->printf("of %lu", (unsigned long)pageCount());
    d->setCursor(ox + W - 70, 22); d->print("swipe > ");
    d->setTextSize(2); d->setTextColor(t.fg, t.bg);
    d->setCursor(ox + 8, 142 + 13); d->print("Font");
    d->setCursor(ox + 8, 190 + 13); d->print("Speed");
    uint16_t onAcc = contrastFor(t.accent);
    for (const PanelBtn &b : kPanelBtns) {
      int16_t bx = ox + b.x;
      if (bx >= W) continue;
      char lbl[12];
      if (b.id == BTN_FONT)       snprintf(lbl, sizeof(lbl), "%s", kReaderFonts[gFontIdx].label);
      else if (b.id == BTN_SPEED) snprintf(lbl, sizeof(lbl), "%d", kSpeeds[gSpeedIdx]);
      else if (b.id == BTN_PLAY)  snprintf(lbl, sizeof(lbl), "%s", resumeAfter ? "|| Pause" : "> Play");
      else                        snprintf(lbl, sizeof(lbl), "%s", b.label);
      uint16_t fg;
      if (isValueBox(b.id)) {
        d->drawRoundRect(bx, b.y, b.w, b.h, 10, t.line);
        fg = t.fg; d->setTextColor(fg, t.bg);
      } else {
        d->fillRoundRect(bx, b.y, b.w, b.h, 10, t.accent);
        fg = onAcc; d->setTextColor(fg, t.accent);
      }
      d->setTextSize(2);
      int16_t tw = (int16_t)strlen(lbl) * 12;
      d->setCursor(bx + (b.w - tw) / 2, b.y + (b.h - 16) / 2);
      d->print(lbl);
    }
  }

  if (cv) cv->flush();
}

void ReaderView::render() {
  if (!gfx) return;
  uint32_t now = millis();
  float dt = (now - lastFrameMs) / 1000.f; if (dt > 0.2f) dt = 0.2f;
  lastFrameMs = now;

  float mx = (float)maxScrollPx();

  if (playing) {
    scrollGoal += kSpeeds[gSpeedIdx] * dt;
    if (scrollGoal >= mx) { scrollGoal = mx; playing = false; }
  }
  if (scrollGoal < 0)  scrollGoal = 0;
  if (scrollGoal > mx) scrollGoal = mx;

  // Menu slide (ease toward target; ~200 ms).
  if (fabsf(menuTarget - menuPos) > 0.002f) {
    menuPos += (menuTarget - menuPos) * 0.35f;
    if (fabsf(menuTarget - menuPos) < 0.01f) menuPos = menuTarget;
  }

  if (pendingTapMs && now - pendingTapMs >= 160) {
    uint32_t t = pendingTapMs;
    pendingTapMs = 0;
    if (lastGestureMs < t) {               // no swipe arrived after the release
      handleTap(tapX, tapY);
      if (currentView != this) return;     // tapped back
    }
  }

  // ease current toward goal for buttery scrolling
  float diff = scrollGoal - scrollPx;
  bool animating = playing;
  if (fabsf(diff) > 0.3f) { scrollPx += diff * 0.30f; animating = true; }
  else scrollPx = scrollGoal;

  drawFrame();

  // persist progress at most every 3 s while reading
  if (now - lastSaveMs > 3000) { saveProgress(); lastSaveMs = now; }

  // idle → return to library (so the watch can auto-sleep). Auto-scroll and
  // active animation count as activity.
  if (!playing && !animating && now - lastInteract > READER_IDLE_MS) {
    switchTo(Screen::BookLibrary);
    return;
  }

  // keep frames coming while animating
  if (animating) { ModelLock lk; model.revision++; }
}

void ReaderView::openControls() {
  resumeAfter = playing;
  playing = false;
  controls = true;
  menuTarget = 1.f;
}

void ReaderView::closeControls() {
  controls = false;
  menuTarget = 0.f;
  if (resumeAfter) playing = true;
  resumeAfter = false;
}

// A confirmed tap (see onEvent). Taps never open the menu (swipe left does);
// with the menu closed only the back chevron responds.
void ReaderView::handleTap(int16_t x, int16_t y) {
  if (!controls) {
    if (menuPos < 0.01f && x < 46 && y < 36) { saveProgress(); switchTo(Screen::BookLibrary); }
    return;
  }
  if (menuPos < 0.98f) return;     // still sliding in: ignore
  const PanelBtn *hit = nullptr;
  for (const PanelBtn &b : kPanelBtns)
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) { hit = &b; break; }
  if (!hit || isValueBox(hit->id)) return;
  int32_t tl = (int32_t)topLine(), lpp = (int32_t)linesPerPage();
  int32_t pageStart = tl - tl % lpp;   // page moves land on line 1 of the page
  bool fontOrSpeed = false;
  switch (hit->id) {
    case BTN_PG_M10: jumpToLine(pageStart - 10 * lpp); break;
    case BTN_PG_M1:  jumpToLine(pageStart - lpp); break;
    case BTN_PG_P1:  jumpToLine(pageStart + lpp); break;
    case BTN_PG_P10: jumpToLine(pageStart + 10 * lpp); break;
    case BTN_LN_M1:  jumpToLine(tl - 1); break;
    case BTN_LN_P1:  jumpToLine(tl + 1); break;
    case BTN_FONT_M:  if (gFontIdx > 0) gFontIdx--; fontOrSpeed = true; break;
    case BTN_FONT_P:  if (gFontIdx + 1 < kReaderFontCount) gFontIdx++; fontOrSpeed = true; break;
    case BTN_SPEED_M: if (gSpeedIdx > 0) gSpeedIdx--; fontOrSpeed = true; break;
    case BTN_SPEED_P: if (gSpeedIdx + 1 < kSpeedCount) gSpeedIdx++; fontOrSpeed = true; break;
    case BTN_PLAY:    // Play/Pause sets what happens on close, then closes
      resumeAfter = !resumeAfter;
      closeControls();
      break;
    case BTN_LIBRARY:
      resumeAfter = false;
      closeControls();
      saveProgress();
      switchTo(Screen::BookLibrary);
      return;
    default: break;   // value labels (font/speed) aren't buttons
  }
  haptic(HapticRole::Tap);
  if (fontOrSpeed) {
    prefsBegin(); gPrefs.putUChar("font", gFontIdx); gPrefs.putUChar("speed", gSpeedIdx);
    // font change requires a re-index; keep position by byte offset
    uint32_t keepOff = gLineCount ? gLineStart[topLine()] : 0;
    bookReindex();
    bool keep = resumeAfter;          // jumpToLine stops playback; keep the intent
    jumpToLine((int32_t)lineForByte(keepOff));
    resumeAfter = keep;
  }
  saveProgress();
}

void ReaderView::onEvent(const Event &e) {
  lastInteract = millis();
  auto kick = []() { ModelLock lk; model.revision++; };

  switch (e.type) {
    case EventType::ButtonShort:     // back: closes the menu first
      if (controls) { closeControls(); kick(); return; }
      saveProgress();
      switchTo(Screen::BookLibrary);
      return;

    case EventType::Touch:
      downX = e.x; downY = e.y; downMs = millis();
      moved = false;
      return;

    case EventType::TouchHold: {
      int32_t dx = (int32_t)e.x - downX, dy = (int32_t)e.y - downY;
      if (dx * dx + dy * dy > 12 * 12) moved = true;
      return;
    }

    case EventType::TouchUp: {
      int32_t dx = (int32_t)e.x - downX, dy = (int32_t)e.y - downY;
      bool still = !moved && dx * dx + dy * dy <= 12 * 12;
      bool quick = millis() - downMs < 450;
      bool afterSwipe = millis() - lastGestureMs < 300;
      if (still && quick && !afterSwipe) {
        pendingTapMs = millis(); tapX = downX; tapY = downY;
        kick();
      }
      return;
    }

    case EventType::Gesture:
      if (e.gesture == Gesture::SingleTap) return;   // our own tap detector decides
      lastGestureMs = millis();
      pendingTapMs = 0;                               // a swipe/double tap is not a tap
      // Swipe right arrives as ButtonShort (OS-wide back) and closes the menu.
      if (e.gesture == Gesture::SwipeLeft) {
        if (!controls) { openControls(); haptic(HapticRole::Open); kick(); }
        return;
      }
      if (controls) return;          // menu open: vertical swipes do nothing
      if (e.gesture == Gesture::DoubleTap) {
        playing = !playing; haptic(playing ? HapticRole::ToggleOn : HapticRole::ToggleOff); kick();
        return;
      }
      if (e.gesture == Gesture::SwipeUp || e.gesture == Gesture::SwipeDown) {
        // One page per swipe, snapped to a page boundary (animated).
        int32_t tl = (int32_t)topLine(), lpp = (int32_t)linesPerPage();
        int32_t pageStart = tl - tl % lpp;
        int32_t to = (e.gesture == Gesture::SwipeUp) ? pageStart + lpp
                   : ((tl % lpp) ? pageStart : pageStart - lpp);
        if (to < 0) to = 0;
        if (gLineCount && to >= (int32_t)gLineCount) to = gLineCount - 1;
        scrollGoal = (float)to * gLineH;
        kick();
      }
      return;

    default:
      return;
  }
}
