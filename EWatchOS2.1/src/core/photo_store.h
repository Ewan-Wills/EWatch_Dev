// Photo store — user images on LittleFS (/img/pNN.jpg), uploaded from the
// web settings page, browsed by the Photos app, and one of them optionally
// the "background": shown by the Photo watch face and the screensaver.
//
// Threading contract:
//   * list mutations (upload/delete/set-bg) come from the WiFi task's web
//     handlers; listing/refresh is mutex-guarded.
//   * photoBgGet()/photoDecodeTo() DECODE and must only be called from the
//     render task (they're heavy and draw into render-owned buffers).
//
// Images are 240x280 baseline JPEGs — the web page resizes client-side, so
// the watch never scales; wrong-sized files are centered/cropped.
#pragma once
#include <Arduino.h>

static const int PHOTO_MAX = 16;                // slots pNN.jpg, NN in 00..15
static const size_t PHOTO_MAX_BYTES = 120 * 1024;  // per-file upload cap

void photoStoreInit();                          // after ewlogMountFs()

// ---- listing (any task; snapshot semantics) ----
void photoRefresh();                            // rescan /img
int  photoCount();
bool photoName(int idx, char out[16]);          // "p03.jpg"

// ---- lifecycle (WiFi task / web handlers) ----
bool photoNextSlot(char out[16]);               // first free slot name, or false
bool photoDelete(const char *name);             // clears bg if it was the bg
size_t photoFsFree();                           // bytes free on the partition

// ---- background selection ----
void        photoSetBg(const char *name);       // "" or nullptr clears
const char *photoBgName();                      // "" when unset

// ---- decoding (RENDER TASK ONLY) ----
// Decode /img/<name> into dst (240x280 RGB565 little-endian, centered/cropped).
bool photoDecodeTo(const char *name, uint16_t *dst);
// Lazily-decoded background bitmap (PSRAM, 240x280) or nullptr when unset /
// decode failed. Re-decodes automatically after photoSetBg().
const uint16_t *photoBgGet();
