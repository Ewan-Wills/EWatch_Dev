// MediaView — full-screen swipe carousel of compiled-in images and videos.
// Assets are stored as raw RGB565 (the panel's native format) so display is a
// straight flash->SPI stream with no decode. Videos are concatenated frames
// played at a fixed FPS, looping while the asset is on screen.
//
// render() blocks and drains the event queue itself. Swipe up/down moves
// between assets; the physical button or a back-swipe exits; a plain tap is
// ignored. Because render() never returns until the user leaves, the
// controller's auto-sleep timer can't fire while an image/video is showing.
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>

#include "media.h"
#include "display.h"
#include "haptic.h"
#include "event.h"
#include "manifest.h"

static const int16_t W = 240, H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

static int assetCount() { return media_manifest::kAssetCount; }
static const MediaAsset &asset(int i) { return media_manifest::kAssets[i]; }

static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
// `tappedBack` shared via view.h — same BACK_W/BACK_H sized hit zone.

// Small top-left overlay: back chevron + asset name + position counter.
static void drawOverlay(const char *name, int idx, int count) {
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(WHITE, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');

  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(BACK_W + 16, 6);
  gfx->print(name);

  char pos[8];
  snprintf(pos, sizeof(pos), "%d/%d", idx + 1, count);
  gfx->setTextSize(1);
  gfx->setTextColor(DARKGREY, BLACK);
  gfx->setCursor(BACK_W + 16, 28);
  gfx->print(pos);
}

// Procedural diagnostic pattern for the built-in sample asset.
static void drawBuiltinSample() {
  gfx->fillScreen(BLACK);
  for (int16_t i = 0; i < 200; i++) {
    uint8_t r = (uint8_t)(i * 255 / 199);
    uint8_t g = (uint8_t)((199 - i) * 255 / 199);
    uint8_t b = (uint8_t)((i * i) >> 7);
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    gfx->drawFastVLine(20 + i, 70, 70, c);
  }
  for (int16_t r = 4; r <= 60; r += 4) {
    uint16_t c = ((r * 4) << 5) | 0x801F;
    gfx->drawCircle(120, 205, r, c);
  }
}

// Event verdict: 0 = exit, +1 = next, -1 = prev, 99 = ignore / keep waiting.
static int navVerdict(const Event &e, Screen &exitTarget) {
  if (e.type == EventType::ButtonShort) return 0;
  if (e.type == EventType::TimerExpired) { exitTarget = Screen::Timer; return 0; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) return 0;
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp)   return +1;
    if (e.gesture == Gesture::SwipeDown) return -1;
  }
  return 99;   // plain taps, holds, IMU motion, ticks: ignored
}

// Show a static frame (image / sample / empty) and wait for a nav event.
static int showStatic(const char *name, int idx, int count, Screen &exitTarget) {
  drawOverlay(name, idx, count);
  for (;;) {
    Event e;
    if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(250)) != pdPASS) continue;
    int v = navVerdict(e, exitTarget);
    if (v != 99) return v;
  }
}

// Play a video, looping, until the user navigates or exits.
static int showVideo(const MediaAsset &a, int idx, int count, Screen &exitTarget) {
  if (!a.pixels || a.frames == 0 || a.fps == 0) {
    gfx->fillScreen(BLACK);
    return showStatic(a.name, idx, count, exitTarget);
  }
  const int16_t  x = (W - a.w) / 2;
  const int16_t  y = (H - a.h) / 2;
  const uint32_t period = 1000u / a.fps;
  const uint32_t ppf = (uint32_t)a.w * (uint32_t)a.h;
  gfx->fillScreen(BLACK);

  for (;;) {                                  // loop the clip
    for (uint16_t fi = 0; fi < a.frames; fi++) {
      uint32_t t0 = millis();
      gfx->draw16bitRGBBitmap(x, y, a.pixels + fi * ppf, a.w, a.h);
      drawOverlay(a.name, idx, count);

      int32_t budget = (int32_t)period - (int32_t)(millis() - t0);
      while (budget > 0) {
        Event e;
        if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) == pdPASS) {
          int v = navVerdict(e, exitTarget);
          if (v != 99) return v;
        }
        budget = (int32_t)period - (int32_t)(millis() - t0);
      }
    }
  }
}

void MediaView::onEnter() {
  index = 0;
  if (gfx) gfx->fillScreen(BLACK);
}

void MediaView::render() {
  if (!gfx) return;
  const int count = assetCount();
  Screen exitTarget = Screen::AppList;

  if (count == 0) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(DARKGREY, BLACK);
    gfx->setCursor(40, 130);
    gfx->print("(no media)");
    drawOverlay("Media", 0, 1);
    for (;;) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(250)) != pdPASS) continue;
      if (navVerdict(e, exitTarget) == 0) break;
    }
    switchTo(exitTarget);
    return;
  }

  for (;;) {
    const MediaAsset &a = asset(index);
    int verdict;
    if (a.kind == MediaKind::Video) {
      verdict = showVideo(a, index, count, exitTarget);
    } else {
      gfx->fillScreen(BLACK);
      if (a.kind == MediaKind::Image && a.pixels) {
        gfx->draw16bitRGBBitmap((W - a.w) / 2, (H - a.h) / 2, a.pixels, a.w, a.h);
      } else if (a.kind == MediaKind::BuiltinSample) {
        drawBuiltinSample();
      }
      verdict = showStatic(a.name, index, count, exitTarget);
    }

    if (verdict == 0) break;                              // exit
    index = (index + count + verdict) % count;            // next / prev
    hapticBuzz(50, 50);
  }
  switchTo(exitTarget);
}
