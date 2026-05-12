// MediaView — list of compiled-in images and videos. Storage format is the
// native panel format (RGB565) so display() is a straight stream from flash
// to the SPI bus. Videos are concatenated frames played at a fixed FPS
// without any decoder.
//
// Playback drains the event queue itself (xQueueReceive) instead of returning
// to the controller per frame; the back button still responds because the
// receive call uses the per-frame budget as its timeout.
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "media.h"
#include "display.h"
#include "haptic.h"
#include "controller.h"
#include "manifest.h"

// ---------- chrome (mirrors view.cpp helpers; kept local to avoid coupling) ----------
static const int16_t W = 240;
static const int16_t H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
static bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}
static void drawBackButton() {
  if (!gfx) return;
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(WHITE, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}

// ---------- list layout ----------
static const int16_t TILE_Y0     = 50;
static const int16_t TILE_H      = 46;
static const int16_t TILE_STRIDE = 50;
static const int     VISIBLE     = 4;
static const int     MOVE_THRESH_SQ = 16 * 16;

static int    assetCount()     { return media_manifest::kAssetCount; }
static const MediaAsset &asset(int i) { return media_manifest::kAssets[i]; }

static int slotAt(uint16_t x, uint16_t y) {
  for (int slot = 0; slot < VISIBLE; slot++) {
    int16_t ty = TILE_Y0 + slot * TILE_STRIDE;
    if (inRect(x, y, 12, ty, W - 24, TILE_H)) return slot;
  }
  return -1;
}

static uint16_t tileColor(int idx) {
  static const uint16_t kPalette[] = {
    NAVY, DARKGREEN, PURPLE, OLIVE, DARKCYAN, MAROON
  };
  return kPalette[idx % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

static const char *kindBadge(MediaKind k) {
  switch (k) {
    case MediaKind::Image:         return "img";
    case MediaKind::Video:         return "vid";
    case MediaKind::BuiltinSample: return "smp";
  }
  return "";
}

// ---------- view methods ----------

void MediaView::onEnter() {
  if (gfx) gfx->fillScreen(BLACK);
  mode = Mode::List;
  needsRedraw = true;
  selectedIdx = -1;
  top = 0;
  pressActive = false;
}

void MediaView::backToList() {
  mode = Mode::List;
  if (gfx) gfx->fillScreen(BLACK);
  needsRedraw = true;
}

void MediaView::render() {
  if (!gfx) return;
  if (mode == Mode::List) {
    if (needsRedraw) { renderList(); needsRedraw = false; }
    return;
  }
  // Player mode — paint the selected asset (blocks for the duration of a
  // video, returning when it ends or the user backs out).
  if (needsRedraw && selectedIdx >= 0 && selectedIdx < assetCount()) {
    play(asset(selectedIdx));
    needsRedraw = false;
  }
}

void MediaView::onEvent(const Event &e) {
  // Player mode: only the player itself drains events while a video is in
  // progress. Once it returns we sit on the final frame (image or last frame
  // of video) until the user goes back.
  if (mode == Mode::Player) {
    if (e.type == EventType::ButtonShort) { backToList(); return; }
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) { backToList(); return; }
    if (e.type == EventType::Touch) { backToList(); return; }
    return;
  }

  // List mode.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }

  if (e.type == EventType::Gesture && assetCount() > VISIBLE) {
    if (e.gesture == Gesture::SwipeUp) {
      top = (top + 1) % assetCount();
      needsRedraw = true; pressConsumed = true; return;
    }
    if (e.gesture == Gesture::SwipeDown) {
      top = (top + assetCount() - 1) % assetCount();
      needsRedraw = true; pressConsumed = true; return;
    }
  }

  if (e.type == EventType::Touch) {
    pressActive   = true;
    pressConsumed = false;
    pressX = e.x; pressY = e.y;
    pressTime = millis();
    pressSlot = slotAt(e.x, e.y);
    return;
  }
  if (e.type == EventType::TouchHold && pressActive) {
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > MOVE_THRESH_SQ) pressConsumed = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    bool quickTap = (millis() - pressTime) < 600;
    if (pressActive && !pressConsumed && pressSlot >= 0 && quickTap) {
      int idx = (top + pressSlot) % assetCount();
      if (idx >= 0 && idx < assetCount()) {
        hapticBuzz(80, 70);
        selectedIdx = idx;
        mode = Mode::Player;
        needsRedraw = true;
      }
    }
    pressActive   = false;
    pressSlot     = -1;
    pressConsumed = false;
  }
}

// ---------- list rendering ----------

void MediaView::renderList() {
  gfx->fillScreen(BLACK);
  drawBackButton();
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(98, 12);
  gfx->print("Media");
  gfx->drawFastHLine(20, 46, 200, DARKGREY);

  const int N = assetCount();
  if (N == 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(40, 130);
    gfx->print("(no media)");
    return;
  }

  const int vc = (N < VISIBLE) ? N : VISIBLE;
  for (int slot = 0; slot < vc; slot++) {
    int idx = (top + slot) % N;
    const MediaAsset &a = asset(idx);
    int16_t y = TILE_Y0 + slot * TILE_STRIDE;
    uint16_t fg = tileColor(idx);
    gfx->fillRoundRect(12, y, W - 24, TILE_H, 6, fg);
    gfx->drawRoundRect(12, y, W - 24, TILE_H, 6, WHITE);
    gfx->setTextColor(WHITE, fg);
    gfx->setTextSize(2);
    gfx->setCursor(20, y + 8);
    gfx->print(a.name);
    // tiny kind badge bottom-right of the tile
    gfx->setTextSize(1);
    gfx->setCursor(W - 50, y + TILE_H - 12);
    gfx->print(kindBadge(a.kind));
  }
  if (N > VISIBLE) {
    int16_t cx = W - 8;
    for (int i = 0; i < N; i++) {
      int16_t py = TILE_Y0 + 6 + i * 8;
      gfx->fillCircle(cx, py, 2, i == top ? WHITE : DARKGREY);
    }
  }
}

// ---------- playback ----------

void MediaView::play(const MediaAsset &a) {
  switch (a.kind) {
    case MediaKind::Image:         drawImage(a);          break;
    case MediaKind::Video:         playVideo(a);          break;
    case MediaKind::BuiltinSample: drawBuiltinSample();   break;
  }
}

void MediaView::drawImage(const MediaAsset &a) {
  if (!a.pixels) return;
  gfx->fillScreen(BLACK);
  int16_t x = (W - a.w) / 2;
  int16_t y = (H - a.h) / 2;
  // Streams the RGB565 array straight from flash to SPI.
  gfx->draw16bitRGBBitmap(x, y, a.pixels, a.w, a.h);
}

bool MediaView::playVideo(const MediaAsset &a) {
  if (!a.pixels || a.frames == 0 || a.fps == 0) return true;
  const int16_t  x = (W - a.w) / 2;
  const int16_t  y = (H - a.h) / 2;
  const uint32_t period = 1000u / a.fps;
  const uint32_t pixelsPerFrame = (uint32_t)a.w * (uint32_t)a.h;

  gfx->fillScreen(BLACK);

  for (uint16_t fi = 0; fi < a.frames; fi++) {
    uint32_t t0 = millis();
    const uint16_t *frame = a.pixels + fi * pixelsPerFrame;
    gfx->draw16bitRGBBitmap(x, y, frame, a.w, a.h);

    // Spend the remainder of this frame's budget polling the event queue,
    // so the back button / back tap stays responsive without us busy-waiting.
    int32_t budget = (int32_t)period - (int32_t)(millis() - t0);
    while (budget > 0) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) == pdPASS) {
        if (e.type == EventType::ButtonShort) return false;
        if (e.type == EventType::Touch && tappedBack(e.x, e.y)) return false;
        if (e.type == EventType::Touch) return false;     // tap anywhere = exit
        // Drop other events (touch holds, gestures, IMU motion, ticks).
      }
      budget = (int32_t)period - (int32_t)(millis() - t0);
    }
  }
  return true;
}

void MediaView::drawBuiltinSample() {
  // Procedural diagnostic pattern — proves the player path works without
  // requiring any encoded assets. Drawn directly with GFX primitives so it
  // costs zero flash for pixel data.
  gfx->fillScreen(BLACK);
  drawBackButton();
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(60, 12);
  gfx->print("Sample");

  // Color-bar gradient.
  for (int16_t i = 0; i < 200; i++) {
    uint8_t r = (uint8_t)(i * 255 / 199);
    uint8_t g = (uint8_t)((199 - i) * 255 / 199);
    uint8_t b = (uint8_t)((i * i) >> 7);
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    gfx->drawFastVLine(20 + i, 60, 60, c);
  }
  // Concentric rings.
  for (int16_t r = 4; r <= 60; r += 4) {
    uint16_t c = ((r * 4) << 5) | 0x801F;
    gfx->drawCircle(120, 200, r, c);
  }
}
