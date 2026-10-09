// Photos — full-screen gallery over the LittleFS photo store.
//
// Swipe up / down pages through images (matching the old media viewer's
// gesture language; left/right are taken by the global back conversion).
// Long-press sets the current photo as the background (Photo face +
// screensaver) with a confirming double buzz. Button / swipe-right = back.
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "photo_store.h"
#include "gallery.h"

using namespace aura;

void GalleryView::onEnter() {
  photoRefresh();
  n = photoCount();
  if (cur >= n) cur = 0;
  dirty = true;
}

uint16_t GalleryView::desiredFrameMs() const { return 0; }   // event-driven

void GalleryView::render() {
  if (!dirty) return;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv || !gfx) return;
  dirty = false;

  AuraTheme th = auraTheme();
  uint16_t *fb = cv->getFramebuffer();

  if (n == 0) {
    cv->fillScreen(th.bg);
    auraTextCentered(cv, 120, 120, "no photos yet", 2, th.text);
    auraTextCentered(cv, 120, 150, "upload from the watch's", 1, th.textDim);
    auraTextCentered(cv, 120, 164, "web page (Settings > WiFi)", 1, th.textDim);
    cv->flush();
    return;
  }

  char name[16];
  bool drawn = false;
  if (photoName(cur, name)) drawn = photoDecodeTo(name, fb);
  if (!drawn) {
    cv->fillScreen(th.bg);
    auraTextCentered(cv, 120, 130, "decode failed", 2, kRed);
  } else if (strcmp(name, photoBgName()) == 0) {
    // Small badge marking the current background photo.
    auraPill(cv, 240 - 52, 10, 42, 20, th.accent, kText, "BG", 1);
  }

  // Index dots along the bottom.
  if (n > 1) {
    int16_t total = n * 12, x = (240 - total) / 2 + 6;
    for (int i = 0; i < n; i++, x += 12)
      cv->fillCircle(x, 270, i == cur ? 4 : 2, i == cur ? th.accent : th.line);
  }
  cv->flush();
}

void GalleryView::onEvent(const Event &e) {
  switch (e.type) {
    case EventType::Gesture:
      if (n > 0 && e.gesture == Gesture::SwipeUp)   { cur = (cur + 1) % n; bump(); }
      if (n > 0 && e.gesture == Gesture::SwipeDown) { cur = (cur + n - 1) % n; bump(); }
      if (e.gesture == Gesture::LongPress && n > 0) {
        char name[16];
        if (photoName(cur, name)) {
          photoSetBg(name);
          hapticBuzz(40, 50);
          delay(90);
          hapticBuzz(40, 50);
          bump();
        }
      }
      break;
    case EventType::ButtonShort:
      switchTo(Screen::AppList);
      break;
    default:
      break;
  }
}

void GalleryView::bump() {
  dirty = true;
  hapticBuzz(18, 22);
  ModelLock lk; model.revision++;
}
