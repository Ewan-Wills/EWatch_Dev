#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "carousel.h"

static const int16_t W = 240;
static const int16_t H = 280;
static const int16_t BACK_W = 60, BACK_H = 42;

void CarouselView::onEnter() {
  canvas = frameCanvas();
  if (!canvas) return;
  // Keep `cur` across visits so coming back from a sub-app lands you on the
  // tile you launched from. First-ever entry uses the class default (0).
  if (cur < 0 || cur >= N) cur = 0;
  // Tile slides in from one slot below the target on entry.
  scrollPos      = (float)cur - 1.0f;
  scrollFrom     = (float)cur - 1.0f;
  scrollTo       = (float)cur;
  scrollStart    = millis();
  scrollDuration = 500;
  pulseStart     = 0;
  exitAtMs       = 0;
  exitTarget     = backScreen;
  pressActive    = false;
  pressMoved     = false;
}

void CarouselView::render() {
  if (!canvas) return;
  // Drive frames until either (a) the deferred exit fires (e.g. tap pulse
  // → switchTo) or (b) the animation settles. When settled we return; the
  // controller will call render() again on the next model change.
  for (;;) {
    uint32_t t0 = millis();
    drawFrame();
    canvas->flush();

    if (exitAtMs > 0 && (int32_t)(millis() - exitAtMs) >= 0) {
      Screen target = exitTarget;
      exitAtMs = 0;
      switchTo(target);
      return;
    }
    if (!isAnimating()) return;

    // Drain events with a ~30 fps frame budget so input still feels live
    // while a scroll/pulse animation is running.
    int32_t budget = 33 - (int32_t)(millis() - t0);
    if (budget < 1) budget = 1;
    while (budget > 0) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) != pdPASS) break;
      handleEvent(e);
      if (exitAtMs > 0 && (int32_t)(millis() - exitAtMs) >= 0) break;
      budget = 33 - (int32_t)(millis() - t0);
    }
  }
}

void CarouselView::onEvent(const Event &e) {
  // The settled state delivers events through here; the animated state
  // drains them inline. Both go through handleEvent.
  handleEvent(e);
  // If a new animation kicked off, bump revision so the controller
  // re-enters render() right away.
  if (isAnimating()) { ModelLock lk; model.revision++; }
}

bool CarouselView::isAnimating() const {
  if (scrollPos != scrollTo) return true;
  if (pulseStart > 0 && (millis() - pulseStart) < kPulseMs) return true;
  if (exitAtMs > 0) return true;
  return false;
}

float CarouselView::easeOutCubic(float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  float u = 1.f - t;
  return 1.f - u * u * u;
}

void CarouselView::advanceScrollTween() {
  if (scrollPos == scrollTo) return;
  uint32_t now = millis();
  uint32_t elapsed = now - scrollStart;
  if (elapsed >= scrollDuration) {
    scrollPos = scrollTo;
    if (wrapAround) {
      // Re-anchor into [0, N) so cur and scrollPos agree, and so the next
      // wrap snap doesn't drift further outside the canonical range.
      if (scrollPos < 0)         scrollPos += (float)N;
      if (scrollPos >= (float)N) scrollPos -= (float)N;
      scrollTo   = scrollPos;
      scrollFrom = scrollPos;
    }
  } else {
    float p = (float)elapsed / (float)scrollDuration;
    scrollPos = scrollFrom + (scrollTo - scrollFrom) * easeOutCubic(p);
  }
}

float CarouselView::currentPulseScale() {
  if (pulseStart == 0) return 1.0f;
  uint32_t elapsed = millis() - pulseStart;
  if (elapsed >= kPulseMs) { return 1.0f; }
  float p = (float)elapsed / (float)kPulseMs;
  // Sine bump: 1.0 -> 1.18 -> 1.0
  return 1.0f + 0.18f * sinf(p * 3.14159265f);
}

void CarouselView::startScroll(int newIndex) {
  if (wrapAround) {
    // Snap scrollPos so the wrapped tile sits exactly one slot from center
    // in the natural direction, then tween that one slot. drawTiles + the
    // indicator both use modular arithmetic on idx so out-of-range scrollPos
    // is fine — it represents "we're visually past the end / before the
    // start" and the tween settles back into [0, N) when it completes.
    if (newIndex < 0) {
      scrollPos += (float)N;        // 0 -> N, so cur=N-1 sits at scrollPos-1
      newIndex += N;
    } else if (newIndex >= N) {
      scrollPos -= (float)N;        // N-1 -> -1, so cur=0 sits at scrollPos+1
      newIndex -= N;
    }
    if (newIndex < 0 || newIndex >= N) return;
  } else {
    if (newIndex < 0 || newIndex >= N) return;
  }
  cur            = newIndex;
  scrollFrom     = scrollPos;
  scrollTo       = (float)newIndex;
  scrollStart    = millis();
  scrollDuration = 320;
  hapticBuzz(40, 45);
}

void CarouselView::drawFrame() {
  advanceScrollTween();
  ThemeColors t = theme();
  canvas->fillScreen(t.bg);
  drawChrome(t);
  drawTiles(t);
  drawIndicator(t);
}

void CarouselView::drawChrome(const ThemeColors &t) {
  // Back chevron — accent-coloured with auto-contrast glyph (kept on the
  // bitmap font so the chevron stays visually consistent across styles).
  uint16_t backTxt = contrastFor(t.accent);
  canvas->fillRoundRect(2, 2, BACK_W, BACK_H, 6, t.accent);
  canvas->setTextColor(backTxt, t.accent);
  canvas->setTextSize(3);
  canvas->setCursor(18, 12);
  canvas->print('<');
  // Title in the user's UI font where available.
  canvas->setTextColor(t.fg, t.bg);
  const GFXfont *uiFont = currentUiFont();
  if (uiFont) {
    canvas->setFont(uiFont);
    canvas->setTextSize(1);
    int16_t x1, y1; uint16_t tw, th;
    canvas->getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
    canvas->setCursor((W - (int16_t)tw) / 2 - x1, 28);
    canvas->print(title);
    canvas->setFont(nullptr);
  } else {
    canvas->setTextSize(2);
    int16_t tw = (int16_t)strlen(title) * 12;
    canvas->setCursor((W - tw) / 2, 14);
    canvas->print(title);
  }
  canvas->drawFastHLine(20, 44, 200, t.line);
}

void CarouselView::drawTiles(const ThemeColors &t) {
  float pulse = currentPulseScale();
  // Defensive: guard against a corrupt scrollPos turning the visible-window
  // loop into a giant or undefined range (floor/ceil of NaN/Inf, cast to
  // int → UB). Should never trigger in normal operation.
  if (isnan(scrollPos) || isinf(scrollPos)) {
    scrollPos = 0.f; scrollFrom = 0.f; scrollTo = 0.f;
  }
  // Visible window: index +/-1 around the currently displayed position.
  int low  = (int)floorf(scrollPos) - 1;
  int high = (int)ceilf (scrollPos) + 1;
  if (high - low > 8 || N <= 0) { low = -1; high = 1; }
  for (int idx = low; idx <= high; idx++) {
    int dataIdx;
    if (wrapAround) {
      dataIdx = ((idx % N) + N) % N;
    } else {
      if (idx < 0 || idx >= N) continue;
      dataIdx = idx;
    }
    if (dataIdx < 0 || dataIdx >= N) continue;
    float offset = (float)idx - scrollPos;          // tile units
    int16_t cy = (int16_t)(TILE_CY + offset * STRIDE);
    if (cy - TILE_H / 2 > H || cy + TILE_H / 2 < 50) continue;

    bool centred = (dataIdx == cur) && (scrollPos == scrollTo);
    float s = centred ? pulse : 1.0f;

    int16_t tw = (int16_t)(TILE_W * s);
    int16_t th = (int16_t)(TILE_H * s);
    int16_t tx = (W - tw) / 2;
    int16_t ty = cy - th / 2;

    // App cards all paint in the accent colour; outline + label use the
    // theme line/contrast colours so they stay legible regardless of accent.
    uint16_t tileBg  = t.accent;
    uint16_t tileTxt = contrastFor(t.accent);
    bool fullyOnCanvas = (tx >= 0 && ty >= 0 && tx + tw <= W && ty + th <= H);
    if (fullyOnCanvas) {
      canvas->fillRoundRect(tx, ty, tw, th, 16, tileBg);
      canvas->drawRoundRect(tx, ty, tw, th, 16, t.line);
    } else {
      // Partial off-canvas — clamp fillRect to avoid the GFX library's
      // unclipped write spilling into adjacent heap (caused StoreProhibited
      // crashes inside the WiFi stack last time around).
      int16_t cx0 = tx < 0 ? 0 : tx;
      int16_t cy0 = ty < 0 ? 0 : ty;
      int16_t cx1 = (tx + tw) > W ? W : (tx + tw);
      int16_t cy1 = (ty + th) > H ? H : (ty + th);
      if (cx1 > cx0 && cy1 > cy0) {
        canvas->fillRect(cx0, cy0, cx1 - cx0, cy1 - cy0, tileBg);
      }
    }

    // App name — uses the user's selected font when available, otherwise
    // bitmap size 3. Centred horizontally in the tile, vertically near the
    // tile's centre line. Skip the draw if the cursor lands off-canvas.
    int16_t titleY = cy - 12;
    if (titleY >= 0 && titleY + 24 <= H) {
      canvas->setTextColor(tileTxt, tileBg);
      const GFXfont *uiFont = currentUiFont();
      if (uiFont) {
        canvas->setFont(uiFont);
        canvas->setTextSize(1);
        int16_t x1, y1; uint16_t lw, lh;
        canvas->getTextBounds(entries[dataIdx].name, 0, 0, &x1, &y1, &lw, &lh);
        int16_t cxText = tx + (tw - (int16_t)lw) / 2 - x1;
        int16_t cyText = cy + 6;            // baseline ~= centre + ascender/2
        canvas->setCursor(cxText, cyText);
        canvas->print(entries[dataIdx].name);
        canvas->setFont(nullptr);
      } else {
        canvas->setTextSize(3);
        int16_t labelW = (int16_t)strlen(entries[dataIdx].name) * 18;
        canvas->setCursor(tx + (tw - labelW) / 2, titleY);
        canvas->print(entries[dataIdx].name);
      }
    }
  }
}

void CarouselView::drawIndicator(const ThemeColors &t) {
  if (N <= 1) return;
  int16_t y = H - 14;
  int16_t spacing = 14;
  int16_t totalW = (N - 1) * spacing;
  int16_t startX = (W - totalW) / 2;
  // Normalize scrollPos into [0, N) for dot calculations, and (when
  // wrapping) consider the shorter wrap-around distance so the active dot
  // stays on the actual target during the cross-boundary tween.
  float pos = scrollPos;
  if (wrapAround) {
    pos = fmodf(pos, (float)N);
    if (pos < 0) pos += (float)N;
  }
  for (int i = 0; i < N; i++) {
    float dist = fabsf((float)i - pos);
    if (wrapAround) {
      float dist2 = (float)N - dist;
      if (dist2 < dist) dist = dist2;
    }
    if (dist > 1.f) dist = 1.f;
    int16_t radius = (int16_t)(3.f + (1.f - dist) * 3.f);
    uint16_t col = (dist < 0.5f) ? t.fg : t.line;
    canvas->fillCircle(startX + i * spacing, y, radius, col);
  }
}

void CarouselView::handleEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    exitTarget = backScreen;
    exitAtMs   = millis();
    return;
  }
  if (e.type == EventType::TimerExpired) {
    exitTarget = Screen::Timer;
    exitAtMs   = millis();
    return;
  }
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp)   { startScroll(cur + 1); return; }
    if (e.gesture == Gesture::SwipeDown) { startScroll(cur - 1); return; }
    return;
  }
  if (e.type == EventType::Touch) {
    if (tappedBack(e.x, e.y)) {
      exitTarget = backScreen;
      exitAtMs   = millis();
      return;
    }
    pressActive = true;
    pressMoved  = false;
    pressX = e.x; pressY = e.y;
    pressTime = millis();
    return;
  }
  if (e.type == EventType::TouchHold && pressActive) {
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > kMoveThreshSq) pressMoved = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    bool quick = (millis() - pressTime) < 600;
    if (pressActive && !pressMoved && quick && hitCurrentTile(pressX, pressY)) {
      // Defensive: only launch if cur is in range. The wrap path can't
      // produce an OOB index, but the check keeps the assertion local.
      if (cur < 0 || cur >= N) {
        pressActive = false; pressMoved = false; return;
      }
      pulseStart = millis();
      exitTarget = entries[cur].target;
      exitAtMs   = millis() + 180;
      hapticBuzz(80, 70);
    }
    pressActive = false;
    pressMoved  = false;
    return;
  }
}

bool CarouselView::hitCurrentTile(int x, int y) {
  if (scrollPos != scrollTo) return false;       // ignore taps mid-scroll
  int16_t tx = (W - TILE_W) / 2;
  int16_t ty = TILE_CY - TILE_H / 2;
  return uiInRect((uint16_t)x, (uint16_t)y, tx, ty, TILE_W, TILE_H);
}
