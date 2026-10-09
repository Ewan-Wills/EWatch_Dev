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
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock
#include "event.h"

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

  Serial.printf(">>BOOK RECV %s %lu\n", name, (unsigned long)dataLen);
  gUploadBusy = true;

  fsInit();
  bool wrote = false; uint32_t crc = 0; uint32_t remaining = dataLen;
  {
    FsLock lk;
    if (gFsOk) {
      File out = LittleFS.open(tmp, "w");
      if (out) {
        static uint8_t chunk[512];
        wrote = true;
        while (remaining > 0) {
          size_t want = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
          size_t got = serialReadN(chunk, want, 3000);
          if (got == 0) { wrote = false; break; }
          crc = crc32_update(crc, chunk, got);
          if (out.write(chunk, got) != got) { wrote = false; break; }
          remaining -= got;
          esp_task_wdt_reset();
        }
        out.close();
      }
    }
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
  gUploadBusy = false;
  if (ok) {
    Serial.printf(">>BOOK OK %s %lu\n", name, (unsigned long)dataLen);
    { ModelLock mlk; model.revision++; }     // nudge the library to refresh
  } else {
    Serial.println(">>BOOK ERR write");
  }
}

static void doList() {
  listBooks();
  Serial.printf(">>BOOK LIST %d\n", gBookN);
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
  if (ok) { ModelLock mlk; model.revision++; }
}

static void bookUploadTask(void *) {
  esp_task_wdt_add(nullptr);
  Serial.println(">>BOOK READY");
  uint8_t win[4] = { 0, 0, 0, 0 };
  for (;;) {
    esp_task_wdt_reset();
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
// LibraryView
// ===========================================================================
static const int16_t LIB_TITLE_Y = 14, LIB_LINE_Y = 46;
static const int16_t ROW_H = 40, ROW_TOP = 56;
static int libVisibleRows() { return (H - ROW_TOP) / ROW_H; }

void LibraryView::onEnter() {
  bookUploadStart();
  listBooks();
  firstDraw = true;
  confirmDel = false; sel = -1;
  if (scrollTop > gBookN) scrollTop = 0;
  lastInteract = millis();
  { ModelLock lk; lastSeenUploads = model.revision; }
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
}

void LibraryView::rescan() { listBooks(); }

void LibraryView::drawList() {
  if (!gfx) return;
  ThemeColors t = theme();
  drawTitleBar("Library", LIB_TITLE_Y, LIB_LINE_Y);
  uint16_t txtOnAcc = contrastFor(t.accent);

  if (gBookN == 0) {
    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(MARGIN, 90);  gfx->print("No books yet.");
    gfx->setCursor(MARGIN, 108); gfx->print("Upload a .txt over USB:");
    gfx->setCursor(MARGIN, 130); gfx->print("  pc/upload.py book.txt");
    return;
  }

  int vis = libVisibleRows();
  if (sel >= 0 && sel < scrollTop) scrollTop = sel;
  if (sel >= scrollTop + vis)      scrollTop = sel - vis + 1;
  if (scrollTop < 0) scrollTop = 0;

  for (int r = 0; r < vis; r++) {
    int idx = scrollTop + r;
    if (idx >= gBookN) break;
    int16_t y = ROW_TOP + r * ROW_H;
    bool selected = (idx == sel);
    uint16_t rowBg = selected ? t.accent : t.bg;
    uint16_t rowFg = selected ? txtOnAcc : t.fg;
    gfx->fillRoundRect(MARGIN, y, W - 2 * MARGIN, ROW_H - 6, 6, rowBg);
    if (!selected) gfx->drawRoundRect(MARGIN, y, W - 2 * MARGIN, ROW_H - 6, 6, t.line);
    gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(rowFg, rowBg);
    // name (trim .txt for display), truncated to fit
    char disp[28];
    strncpy(disp, gBooks[idx].name, sizeof(disp) - 1); disp[sizeof(disp) - 1] = '\0';
    size_t L = strlen(disp);
    if (L > 4 && strcasecmp(disp + L - 4, ".txt") == 0) disp[L - 4] = '\0';
    if (strlen(disp) > 24) disp[24] = '\0';
    gfx->setCursor(MARGIN + 8, y + 6); gfx->print(disp);
    // size in KB
    gfx->setCursor(MARGIN + 8, y + 20);
    gfx->setTextColor(selected ? rowFg : t.line, rowBg);
    gfx->printf("%lu KB", (unsigned long)((gBooks[idx].size + 1023) / 1024));
  }

  // scroll hint
  if (gBookN > vis) {
    gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(W - 70, H - 12); gfx->print("swipe \x18\x19");
  }
}

void LibraryView::render() {
  if (!gfx) return;
  uint32_t rev; { ModelLock lk; rev = model.revision; }
  if (rev != lastSeenUploads) { rescan(); lastSeenUploads = rev; firstDraw = true; }
  if (confirmDel) {
    // confirmation overlay
    ThemeColors t = theme();
    gfx->fillRoundRect(20, 90, 200, 110, 10, t.accent);
    uint16_t fg = contrastFor(t.accent);
    gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(fg, t.accent);
    gfx->setCursor(36, 104); gfx->print("Delete this book?");
    char disp[24];
    if (sel >= 0 && sel < gBookN) { strncpy(disp, gBooks[sel].name, 23); disp[23] = '\0'; }
    else disp[0] = '\0';
    gfx->setCursor(36, 122); gfx->print(disp);
    gfx->fillRoundRect(36, 150, 76, 34, 6, t.bg);  gfx->drawRoundRect(36, 150, 76, 34, 6, t.line);
    gfx->fillRoundRect(128, 150, 76, 34, 6, t.bg); gfx->drawRoundRect(128, 150, 76, 34, 6, t.line);
    gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(58, 160); gfx->print("Cancel");
    gfx->setTextColor(RED, t.bg);
    gfx->setCursor(150, 160); gfx->print("Del");
    firstDraw = true;  // force a clean redraw of the list after the overlay closes
    return;
  }
  if (firstDraw) { drawList(); firstDraw = false; }

  // idle → go back to the watch so the system can auto-sleep (unless a serial
  // upload is in progress).
  if (!gUploadBusy && millis() - lastInteract > LIBRARY_IDLE_MS) {
    switchTo(Screen::Watch);
  }
}

void LibraryView::onEvent(const Event &e) {
  lastInteract = millis();

  if (confirmDel) {
    if (e.type == EventType::Touch) {
      // Cancel (36..112) / Del (128..204) at y 150..184
      if (e.y >= 150 && e.y <= 190) {
        if (e.x >= 128) {
          if (sel >= 0 && sel < gBookN) {
            char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, gBooks[sel].name);
            { FsLock lk; if (gFsOk) LittleFS.remove(path); }
            hapticBuzz(160, 40);
            listBooks(); sel = -1;
          }
        }
      }
      confirmDel = false; firstDraw = true;
      { ModelLock lk; model.revision++; }
    }
    if (e.type == EventType::ButtonShort) { confirmDel = false; firstDraw = true; { ModelLock lk; model.revision++; } }
    return;
  }

  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  if (e.type == EventType::Gesture) {
    int vis = libVisibleRows();
    if (e.gesture == Gesture::SwipeUp   && scrollTop + vis < gBookN) { scrollTop++; firstDraw = true; { ModelLock lk; model.revision++; } }
    if (e.gesture == Gesture::SwipeDown && scrollTop > 0)            { scrollTop--; firstDraw = true; { ModelLock lk; model.revision++; } }
    if (e.gesture == Gesture::LongPress) {
      // delete the row under the last touch? use current sel row under finger
      int vis2 = libVisibleRows(); (void)vis2;
      // determine row from y of this gesture
      if (e.y >= ROW_TOP) {
        int r = (e.y - ROW_TOP) / ROW_H;
        int idx = scrollTop + r;
        if (idx >= 0 && idx < gBookN) { sel = idx; confirmDel = true; hapticBuzz(200, 50); { ModelLock lk; model.revision++; } }
      }
    }
    return;
  }

  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
    if (e.y >= ROW_TOP) {
      int r = (e.y - ROW_TOP) / ROW_H;
      int idx = scrollTop + r;
      if (idx >= 0 && idx < gBookN) {
        char path[96]; snprintf(path, sizeof(path), "%s/%s", BOOK_DIR, gBooks[idx].name);
        hapticBuzz(140, 30);
        if (bookOpen(path)) {
          switchTo(Screen::BookReader);
        } else {
          sel = idx; firstDraw = true; { ModelLock lk; model.revision++; }
        }
      }
    }
    return;
  }
}

// ===========================================================================
// ReaderView
// ===========================================================================
void ReaderView::onEnter() {
  controls = false; playing = false;
  lastFrameMs = millis(); lastInteract = millis(); lastSaveMs = millis();
  // restore saved position (by byte offset, font-independent)
  uint32_t byteOff = loadPos(gOpenPath);
  uint32_t topLine = lineForByte(byteOff);
  scrollPx = scrollGoal = (float)topLine * gLineH;
  float mx = (float)maxScrollPx();
  if (scrollGoal > mx) scrollGoal = scrollPx = mx;
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
}

void ReaderView::onExit() { saveProgress(); bookClose(); }

void ReaderView::saveProgress() {
  if (gOpenPath[0] == '\0' || gLineCount == 0) return;
  uint32_t topLine = (uint32_t)(scrollPx / gLineH + 0.5f);
  if (topLine >= gLineCount) topLine = gLineCount - 1;
  savePos(gOpenPath, gLineStart[topLine]);
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

  char buf[160];
  for (int slot = 0; ; slot++) {
    int li = firstLine + slot;
    if (li >= (int)gLineCount) break;
    int16_t slotTop = slot * gLineH - subPx;
    if (slotTop >= TEXT_H) break;
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

  if (controls) {
    // bottom control panel
    int16_t py = H - 92;
    d->fillRoundRect(6, py, W - 12, 84, 10, t.accent);
    uint16_t fg = contrastFor(t.accent);
    d->setTextColor(fg, t.accent); d->setTextSize(1);
    d->setCursor(14, py + 8); d->print("Font");
    d->setCursor(118, py + 8); d->print("Speed");
    // Font A- / label / A+
    d->fillRoundRect(14, py + 22, 26, 26, 5, t.bg);  d->setTextColor(t.fg, t.bg);
    d->setCursor(22, py + 30); d->print("-");
    d->fillRoundRect(46, py + 22, 26, 26, 5, t.bg);
    d->setCursor(54, py + 30); d->print(kReaderFonts[gFontIdx].label);
    d->fillRoundRect(78, py + 22, 26, 26, 5, t.bg);
    d->setCursor(86, py + 30); d->print("+");
    // Speed - / val / +
    d->fillRoundRect(118, py + 22, 26, 26, 5, t.bg);
    d->setCursor(126, py + 30); d->print("-");
    d->fillRoundRect(150, py + 22, 32, 26, 5, t.bg);
    d->setCursor(156, py + 30); d->printf("%d", kSpeeds[gSpeedIdx]);
    d->fillRoundRect(188, py + 22, 26, 26, 5, t.bg);
    d->setCursor(196, py + 30); d->print("+");
    // Play/Pause
    d->fillRoundRect(14, py + 54, W - 28, 24, 6, t.bg);
    d->setTextColor(t.fg, t.bg);
    d->setCursor(96, py + 61); d->print(playing ? "|| Pause" : "> Play");
  }

  // top-left back chevron (always available)
  d->setFont(nullptr);
  d->fillRoundRect(2, 2, 40, 30, 6, t.accent);
  d->setTextColor(contrastFor(t.accent), t.accent); d->setTextSize(2);
  d->setCursor(14, 10); d->print('<');

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

void ReaderView::onEvent(const Event &e) {
  lastInteract = millis();

  if (e.type == EventType::ButtonShort) {
    saveProgress();
    switchTo(Screen::BookLibrary);
    return;
  }

  if (e.type == EventType::Touch) {
    // back chevron
    if (e.x < 46 && e.y < 36) { saveProgress(); switchTo(Screen::BookLibrary); return; }

    if (controls) {
      int16_t py = H - 92;
      bool handled = false;
      if (e.y >= py + 22 && e.y <= py + 48) {
        if (e.x >= 14 && e.x < 40) { if (gFontIdx > 0) { gFontIdx--; } handled = true; }
        else if (e.x >= 78 && e.x < 104) { if (gFontIdx + 1 < kReaderFontCount) { gFontIdx++; } handled = true; }
        else if (e.x >= 118 && e.x < 144) { if (gSpeedIdx > 0) { gSpeedIdx--; } handled = true; }
        else if (e.x >= 188 && e.x < 214) { if (gSpeedIdx + 1 < kSpeedCount) { gSpeedIdx++; } handled = true; }
        if (handled) {
          prefsBegin(); gPrefs.putUChar("font", gFontIdx); gPrefs.putUChar("speed", gSpeedIdx);
          hapticBuzz(120, 20);
          // font change requires a re-index; keep position by byte offset
          uint32_t topLine = (uint32_t)(scrollPx / gLineH + 0.5f);
          uint32_t keepOff = (gLineCount && topLine < gLineCount) ? gLineStart[topLine] : 0;
          bookReindex();
          uint32_t nl = lineForByte(keepOff);
          scrollPx = scrollGoal = (float)nl * gLineH;
          { ModelLock lk; model.revision++; }
          return;
        }
      }
      if (e.y >= py + 54 && e.y <= py + 80) {   // play/pause
        playing = !playing; hapticBuzz(140, 25);
        { ModelLock lk; model.revision++; }
        return;
      }
      // tap elsewhere closes controls
      controls = false; { ModelLock lk; model.revision++; }
      return;
    }

    // no controls showing: a tap toggles the control panel
    controls = true; hapticBuzz(100, 20);
    { ModelLock lk; model.revision++; }
    return;
  }

  if (e.type == EventType::Gesture) {
    float page = (float)((TEXT_H / gLineH) * gLineH) * 0.9f;  // ~90% of a screen
    if (page < gLineH) page = gLineH;
    if (e.gesture == Gesture::SwipeUp)   { scrollGoal += page; { ModelLock lk; model.revision++; } }
    if (e.gesture == Gesture::SwipeDown) { scrollGoal -= page; { ModelLock lk; model.revision++; } }
    if (e.gesture == Gesture::DoubleTap) { playing = !playing; hapticBuzz(140, 25); { ModelLock lk; model.revision++; } }
    return;
  }
}
